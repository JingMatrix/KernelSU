# uapi: the kernel/userspace ABI contract

Six headers, about 550 lines, define every ioctl number, every command struct and every
constant that crosses the boundary between the KernelSU kernel module and the userspace
programs that drive it. Nothing here is compiled on its own: it is included verbatim by
three build systems using three different compilers, and the layout each computes has to
come out identical, or a `copy_to_user` in the kernel lands on the wrong field in userspace.

That is why this directory sits at the repository root rather than under `kernel/`. There
is no generated marshalling layer, no IDL, no serialisation step: the kernel does
`copy_from_user(&cmd, arg, sizeof(cmd))` and userspace does `ioctl(fd, KSU_IOCTL_X, &cmd)`
on what it believes is the same struct. The contract holds because the headers are
physically the same file, plus one version number consumers check at startup.
[`kernel/supercall/README.md`](../kernel/supercall/README.md) covers what the commands
*do*; this file is the shape of the wire and the rules for changing it.

## Who compiles these headers

| Consumer | How it reaches `uapi/` | Compiler and language |
| --- | --- | --- |
| kernel module | `kernel/include/uapi` symlink to `../../uapi`, plus `-I$(KSU_KERNEL_DIR)/include` from [`kernel/Kbuild`](../kernel/Kbuild) | kernel clang, C |
| `ksud` | bindgen on [`ksu_uapi.h`](../userspace/ksud/src/ksu_uapi.h) with `-I../../` | libclang, **C++** |
| Android manager | `manager/app/src/main/cpp/uapi` symlink to `../../../../../uapi`, plus `target_include_directories(kernelsu PRIVATE .)` | NDK clang, C++ |

All three spell the include with the same `uapi/` prefix and let an include path supply the
directory. The kernel module writes `#include "uapi/supercall.h"`, and likewise for
`app_profile.h`, `feature.h`, `selinux.h` and `sulog.h`; the `kernel/include/uapi` symlink
supplies the directory and `-I$(KSU_KERNEL_DIR)/include` in
[`kernel/Kbuild`](../kernel/Kbuild) puts it on the search path. The manager writes
`#include "uapi/ksu.h"` from [`cpp/ksu.h`](../manager/app/src/main/cpp/ksu.h) and finds it
through the symlink sitting beside that file, which
`target_include_directories(kernelsu PRIVATE .)` in
[`CMakeLists.txt`](../manager/app/src/main/cpp/CMakeLists.txt) puts on the path. ksud
compiles a one-line shim, [`ksu_uapi.h`](../userspace/ksud/src/ksu_uapi.h), whose entire
content is that same include, with the repository root reached by `-I../../`. Both of those
last two name [`ksu.h`](ksu.h), the aggregate header covered at the end of this file.

Binding generation in [`userspace/ksud/build.rs`](../userspace/ksud/build.rs) runs only when
`CARGO_CFG_TARGET_OS` is `android`, since the host build of ksud patches boot images and
never issues an ioctl. It registers `bindgen::CargoCallbacks`, which emits a
`cargo:rerun-if-changed=` line for every header opened, so touching [`feature.h`](feature.h)
rebuilds ksud. Bindgen is no longer the first thing that script does: `main()` calls
`assemble_bootstrap()` unconditionally, ahead of the target-os check, to assemble
`src/lkm_image_bootstrap.S` into an AArch64 object for the LKM image injector, and it panics
when none of the assemblers it tries answers. An edit here that looks like it broke the
bindings may never have reached `configure_bindgen()` at all; setting
`KSU_LKM_BOOTSTRAP_OBJECT` to a prebuilt object gets past that step. Exactly one sanctioned
consumer does not compile these headers --
[`userspace/ksuinit/src/lib.rs`](../userspace/ksuinit/src/lib.rs) hardcodes `0x80104b02`,
`0x80004b02` and the `0xDEADBEEF` / `0xCAFEBABE` reboot-install pair it hands to
[`reboot(2)`][reboot-2] to obtain the driver fd, and re-declares `GetInfoCmd` and
`GetInfoLegacyCmd` by hand, because it runs as PID 1 before ksud exists and does not link
the bindings. Mirror any change to `struct ksu_get_info_cmd` or to the install magics there
yourself; nothing checks it.

## Most constants are objects, not macros

In [`supercall.h`](supercall.h) and [`selinux.h`](selinux.h) every standalone constant is an
initialised object rather than a `#define` -- `static const __u32`, or `static const __u8`
for the three `KSU_UMOUNT_*` selectors; only the ptctl and uhook op selectors are
enumerators. [`app_profile.h`](app_profile.h) and [`sulog.h`](sulog.h) are the exception:
their bounds and version stamps are ordinary macros.

```c
static const __u32 KERNEL_SU_UAPI_VERSION = 2;
static const __u32 KSU_IOCTL_GET_INFO = _IOR('K', 2, struct ksu_get_info_cmd);
```

Declaring them as objects rather than macros is why bindgen runs with `-x c++`. Parsed as C,
a file-scope `static const` is an object with internal linkage and not an integer constant
expression; bindgen would emit an `extern` static, unusable in a Rust `const` context. Parsed
as C++ the same declarations are constant expressions bindgen folds into plain values,
producing lines of the form `pub const KSU_INSTALL_MAGIC1: __u32 = 3735928559;`. That
folding is what lets [`defs.rs`](../userspace/ksud/src/defs.rs) build `FULL_VERSION` from
`KERNEL_SU_UAPI_VERSION` with `const_format::formatcp!`.

The cost is that these names cannot be tested with `#ifdef`, and that a C compiler may
refuse them where an integer constant expression is required: GCC in C mode rejects
`case KSU_IOCTL_GET_INFO:` with "case label does not reduce to an integer constant". Clang
accepts it, which is why [`kernel/selinux/rules.c`](../kernel/selinux/rules.c) switches on
the `KSU_SEPOLICY_CMD_*` constants, but do not lean on that -- the table of
`struct ksu_ioctl_cmd_map` rows in
[`dispatch.c`](../kernel/supercall/dispatch.c) is the portable form.

## Fixed-width types, and the two exceptions

Field types are `__u8` / `__u16` / `__s32` / `__u32` / `__u64` / `__s64` and arrays of them.
`int`, `long`, `size_t`, `pid_t`, `uid_t` and enum-typed fields must not appear in a struct
that crosses the boundary; their width or signedness varies between the kernel build and a
32-bit caller. Enums do define operation selectors -- `enum ksu_ptctl_op`,
`enum ksu_uhook_action`, `enum ksu_feature_id` -- but the field carrying one is always a
`__u32`, as in `ksu_ptctl_cmd.op` and `ksu_uhook_cmd.action`.

Two non-fixed-width types appear. `char` backs the NUL-terminated arrays
`app_profile.key[256]`, `app_profile.rp_config.template_name[256]`,
`root_profile.selinux_domain[64]` and `ksu_sulog_event.comm[16]`; its signedness differs
between arm64 and x86 but its layout does not. `bool` backs `app_profile.allow_su`, the two
`use_default` fields and `non_root_profile.umount_modules`;
it is one byte everywhere this tree builds, but [`app_profile.h`](app_profile.h) never
defines it -- the kernel gets it from `include/linux/types.h`'s `typedef _Bool bool`, the
C++ consumers get the keyword, and a host C program needs its own `<stdbool.h>`.

## Aligned 64-bit fields, and the compat_ioctl trap

The `anon_ksu_fops` table in
[`kernel/supercall/supercall.c`](../kernel/supercall/supercall.c) points `.unlocked_ioctl`
and `.compat_ioctl` at the same `anon_ksu_ioctl()`, which forwards straight to
`ksu_supercall_handle_ioctl()` with no argument translation of any kind.

A 32-bit process on a 64-bit kernel enters through `compat_ioctl`. With one handler and no
translation layer, a struct a 32-bit caller may pass must have identical layout under both
ABIs. i386 aligns `__u64` to 4 bytes while every 64-bit architecture aligns it to 8, so a
bare `__u64` in the middle of a struct shifts everything after it.
[`supercall.h`](supercall.h) states the rule and supplies the `__aligned_s64` fallback macro
for the signed case; `__aligned_u64` comes from `<linux/types.h>`. The rule is applied where
it was thought about and nowhere else. Compiling this directory for three Android targets:

| struct | aarch64 | armv7a | i686 | uses `__aligned_u64`? |
| --- | --- | --- | --- | --- |
| `ksu_ptctl_cmd` | 56 | 56 | 56 | yes |
| `ksu_uhook_cmd` | 144 | 144 | 144 | yes |
| `ksu_set_sepolicy_cmd` | 16 | 16 | 16 | yes |
| `ksu_get_feature_cmd` | 24 | 24 | **16** | no |
| `ksu_set_feature_cmd` | 16 | 16 | **12** | no |
| `ksu_set_spoof_cpu_cmd` | 32 | 32 | **28** | no |
| `root_profile` | 248 | 248 | **240** | no |
| `app_profile` | 784 | 784 | **768** | no |

`ksu_get_feature_cmd.value` sits at offset 8 on the first two targets and offset 4 on i686.
The 32-bit ARM EABI aligns `long long` to 8, so an arm32 compat caller on an arm64 kernel
sees a native layout; the exposure is specific to an x86_64 kernel with i386 callers.
Half of that pair now exists. Upstream's x64 LKM work put an `ARCH=x86_64` module build in
[`kernel/build-all-x64.sh`](../kernel/build-all-x64.sh), and the ksud workflow stages the
resulting `.ko` into `userspace/ksud/bin/x86_64/` beside the aarch64 one, so an x86_64
kernel running this driver is a shipped configuration rather than a thought experiment. The
other half still does not: every consumer is built 64-bit only -- ksud and ksuinit for
`aarch64-linux-android` and `x86_64-linux-android`, the manager's JNI under
`abiFilters += listOf("arm64-v8a", "x86_64")` -- so nothing in this tree can reach
`compat_ioctl` carrying an i386 layout. If a 32-bit ABI is ever added, the five "no" rows are
the fix -- four edits in practice, since `app_profile` diverges only through the
`root_profile` it embeds -- and fixing them is a layout change, therefore a version bump.

## Pointers cross as `__aligned_u64`

No struct here has a pointer member. Where a command hands the kernel a buffer it passes an
integer: `ksu_set_sepolicy_cmd.data`, `ksu_nuke_ext4_sysfs_cmd.arg`,
`ksu_add_try_umount_cmd.arg`, `ksu_ptctl_cmd.uptr`, `ksu_uhook_cmd.path` and
`ksu_uhook_cmd.uptr`. A `void *` member would be 4 bytes for a compat caller and 8 for the
kernel, and would lose the `__user` annotation marking the address untrusted.

The kernel converts explicitly at the point of use, and never in the dispatcher: `do_ptctl()`
and `do_uhook()` only `copy_from_user` the command struct and hand it on. The casts live in
the feature code. `do_peek()`, `do_regs()` and `hwbp_wait()` in
[`ptctl.c`](../kernel/feature/ptctl.c) write `(void __user *)(uintptr_t)c->uptr`; `uh_add()`
and `uh_read()` in [`uhook.c`](../kernel/feature/uhook.c) write
`(void __user *)(uintptr_t)cmd->uptr`, and `uh_add()` takes the path through
`(const char __user *)(uintptr_t)cmd->path` before `strndup_user()`. `do_set_sepolicy()`
writes `(void __user *)cmd.data`. One place does not -- `do_nuke_ext4_sysfs()` passes
`cmd.arg` straight into `strncpy_from_user()`'s `const char __user *` parameter, which
compiles only because
[`kernel/Kbuild`](../kernel/Kbuild) adds `-Wno-int-conversion`. Copy `add_try_umount()`
instead, which writes `(const char __user *)cmd.arg`.

## A shipped struct is frozen

Most commands use the raw `_IOC(dir, 'K', nr, 0)` form, putting zero in the 14-bit size
field of the ioctl number. Encoding `sizeof(struct)` would make the number change whenever
the struct changes, so a stale caller gets a clean `-ENOTTY`; encoding zero keeps it
stable, so a stale caller is *not* rejected and instead gets a truncated or over-long
`copy_from_user`. Where a struct did change shape under a zero-size number, the fix was not
an in-place edit but a sized number minted on the same `nr`, with the zero-size original
kept alive beside it:

```text
KSU_IOCTL_GET_INFO           _IOR('K', 2, struct ksu_get_info_cmd)              = 0x80104b02
KSU_IOCTL_GET_INFO_LEGACY    _IOC(_IOC_READ, 'K', 2, 0)                         = 0x80004b02
KSU_IOCTL_NEW_GET_ALLOW_LIST _IOWR('K', 6, struct ksu_new_get_allow_list_cmd)   = 0xc0044b06
KSU_IOCTL_GET_ALLOW_LIST     _IOC(_IOC_READ | _IOC_WRITE, 'K', 6, 0)            = 0xc0004b06
KSU_IOCTL_NEW_GET_DENY_LIST  _IOWR('K', 7, struct ksu_new_get_allow_list_cmd)   = 0xc0044b07
KSU_IOCTL_GET_DENY_LIST      _IOC(_IOC_READ | _IOC_WRITE, 'K', 7, 0)            = 0xc0004b07
```

All three pairs share an `nr` and only the size field tells them apart; both handlers of
each stay in the table so an old client keeps working. Everything else gets a fresh `nr`.

The maintainer's rule is therefore stricter than the encoding suggests: once a command has
shipped, treat its struct as immutable. Reorder nothing, resize nothing, repurpose no field;
a new capability means a new `nr` and a new struct. The one safe in-place extension is
filling a field reserved and validated as must-be-zero from the start.
`ksu_get_sulog_fd_cmd.flags` is the model -- `do_get_sulog_fd()` rejects a non-zero value
with `-EINVAL` precisely so a future bit can be added without ambiguity.
`ksu_uhook_cmd.__pad0` is documented the same way but is not validated, so it is not yet
usable.

`KSU_IOCTL_PTCTL` shows what even a size-encoded number cannot catch. The meaning of what
`ksu_ptctl_cmd.uptr` points at changed once -- the GETREGS/SETREGS payload moved from
`struct pt_regs` to the user-visible register view -- leaving the command struct and its
number untouched. The only defence is the length check in
[`ptctl.c`](../kernel/feature/ptctl.c): `if (c->len && c->len != KSU_UREGS_SZ)`.

`KSU_IOCTL_UHOOK` carries the same hole one level down. Its number is sized on
`struct ksu_uhook_cmd`, but `KSU_UHOOK_READ` returns an array of `struct ksu_uhook_record`
through `uptr`, and `uh_read()` in [`uhook.c`](../kernel/feature/uhook.c) recovers the
element count as `cmd->len / sizeof(struct ksu_uhook_record)`. There is no length check here
at all -- no version, no stride negotiation -- so widening `regs[34]` would leave a stale
ksud reading every record but the first at the wrong offset, with no error. That record is as
frozen as any command struct.

## `KERNEL_SU_UAPI_VERSION`, and how a consumer negotiates

Because most command numbers cannot reject a stale caller, the compatibility mechanism is a
single integer. `KERNEL_SU_UAPI_VERSION` is currently 2, with a comment recording what the
bump was for. Bump it whenever the shape of anything here changes: a field added, resized or
reordered, or a semantic change to an existing field. Do not bump it for a purely additive
command with a fresh `nr`, because an old client never issues that command and a new client
discovers its absence as `-ENOTTY`.

The kernel publishes the number and nothing more. `do_get_info()` fills
`struct ksu_get_info_cmd` with `KERNEL_SU_VERSION` (the `30000 + git rev-list --count HEAD`
stamp from Kbuild), a flag word of `KSU_GET_INFO_FLAG_LKM` / `_MANAGER` / `_LATE_LOAD` /
`_PR_BUILD`, `KSU_FEATURE_MAX` as `features`, and `KERNEL_SU_UAPI_VERSION` in
`uapi_version`. `GET_INFO` is one of the three commands gated by `always_allow`, so the
handshake works before a caller has proved anything about itself.

Negotiation is entirely the client's job; the kernel never refuses a mismatched caller. Both
clients probe `KSU_IOCTL_GET_INFO` and fall back to `KSU_IOCTL_GET_INFO_LEGACY`, whose
`struct ksu_get_info_legacy_cmd` is the same 12 bytes minus `uapi_version`. There ksud
relies on the field being zero-initialised and
[`manager/app/src/main/cpp/ksu.cc`](../manager/app/src/main/cpp/ksu.cc) assigns
`g_version.uapi_version = 0` explicitly, so 0 reliably means "kernel too old to say".
`ensure_uapi_version_matched()` in [`ksucalls.rs`](../userspace/ksud/src/ksucalls.rs) guards
six entry points: on a mismatch it logs and skips post-fs-data, services, boot-completed and
soft-reboot, and returns the error out of module install and module actions. On the manager
side, `checkUAPIMismatch()` in
[`Natives.kt`](../manager/app/src/main/java/me/weishu/kernelsu/Natives.kt) feeds
`requireNewKernel()`, which locks the manager read-only rather than let it write a malformed
`app_profile` into kernel memory. A client that skips the check mis-marshals silently.

`features` is the finer-grained probe: it reports `KSU_FEATURE_MAX`, so a client knows which
feature ids are in range. Read it as a bound, never as a count. The fork's id space is
sparse, so a kernel answering 17 implements seven features and reports the ten ids between
them as unsupported. Within range, `KSU_IOCTL_GET_FEATURE` answers with a `supported`
byte that is false when the kernel was built without a handler for that id, which is how
the manager hides a switch instead of showing a dead one. Out of range, `ksu_get_feature()`
in [`kernel/policy/feature.c`](../kernel/policy/feature.c) returns `-EINVAL` without writing
`*supported`, and `do_get_feature()` reads that uninitialised local, so bound the loop with
`features`.

## The headers

### [`supercall.h`](supercall.h)

The bulk of the ABI: the version number, the reboot-install magic pair, the event, mark and
umount selectors, the `KSU_GET_INFO_FLAG_*` bits, one argument struct per command plus
`struct ksu_uhook_record`, the capture record `KSU_UHOOK_READ` hands back, all 29
`KSU_IOCTL_*` numbers, and the two op-dispatched blocks for ptctl and uhook. It includes
[`app_profile.h`](app_profile.h) because `ksu_get_app_profile_cmd` embeds
`struct app_profile` by value. Those blocks also carry the fork's normative prose about
ptctl and uhook -- register-index encoding, why `KSU_UHOOK_JUMP` / `SKIP` / `FORCE_RET` are
restricted or refused, what a [uprobe][uprobetracer] stays visible to. See
[`kernel/feature/README.md`](../kernel/feature/README.md) and
[`docs/instrumentation.md`](../docs/instrumentation.md).

Two things here are dead: `struct ksu_become_daemon_cmd` has no ioctl number, no handler and
no caller anywhere in the tree (trust rests on the manager APK signature and the
[SELinux][selinux] domain instead), and `ksu_get_wrapper_fd_cmd.flags` is copied in but
never read.

### [`app_profile.h`](app_profile.h)

`struct root_profile`, `struct non_root_profile` and the `struct app_profile` wrapping them
in a union, plus `KSU_APP_PROFILE_VER` (4), the bounds `KSU_MAX_PACKAGE_NAME` (256),
`KSU_MAX_GROUPS` (32), `KSU_SELINUX_DOMAIN` (64), and `FLAG_KSU_NO_NEW_PRIVS`.

This struct has a second life the others lack: it is also the on-disk record format of
`/data/adb/ksu/.allowlist`, dumped raw with no length prefix, so a shape change breaks
something no handshake can rescue, and
[`kernel/policy/allowlist.c`](../kernel/policy/allowlist.c) carries the machinery.
`profile_valid()` rejects any record whose `version` is not `KSU_APP_PROFILE_VER`,
`migrate_profile()` upgrades an older one, and `ksu_load_allow_list()` picks the record
stride: `kAppProfileSizePreV4 = 776` against `sizeof(struct app_profile)` = 784, an 8-byte
gap that is exactly the `flags` field added in v4.

Commit `46ad8dcb` is the worked example. Adding `__u64 flags` to `root_profile` required, in
one commit: `KSU_APP_PROFILE_VER` 3 to 4, `KERNEL_SU_UAPI_VERSION` 1 to 2, a brand-new
`migrate_profile()` whose `case 3` stamps `FLAG_KSU_NO_NEW_PRIVS` on every su-allowed
record, `profile_valid()`'s version test tightened from `<` to `!=` so that an unmigrated
record can no longer slip through, the new stride constant in the loader, a `flagsField` in
[`jni.cc`](../manager/app/src/main/cpp/jni.cc), and `FLAG_KSU_NO_NEW_PRIVS` in
[`Natives.kt`](../manager/app/src/main/java/me/weishu/kernelsu/Natives.kt). The ioctl
numbers of `GET_APP_PROFILE` and `SET_APP_PROFILE` did not move, being the size-zero form.
Note also
that `root_profile.namespaces` draws its values from
[`kernel/infra/su_mount_ns.h`](../kernel/infra/su_mount_ns.h), outside this directory.

### [`feature.h`](feature.h)

Twenty-two lines, no includes: `enum ksu_feature_id` numbering seven toggles, terminated by
`KSU_FEATURE_MAX`. That terminator is itself part of the ABI, since `do_get_info()` reports
it as `ksu_get_info_cmd.features`, so adding a feature changes what a running client sees
without any struct changing shape. `ksud` hand-mirrors the enum as `FeatureId` in
[`feature.rs`](../userspace/ksud/src/feature.rs) rather than the bindgen constants, so a new
id has to be added in seven places there -- the `#[repr(u32)]` variant, `from_u32()`,
`name()`, `description()`, `parse_feature_id()`, the `all_features` array in
`list_features()` and the separate one in `save_config()`. Miss the last two and the feature
works from the command line but never survives a `ksud feature save`. Two clap help strings
in [`cli.rs`](../userspace/ksud/src/cli.rs) spell the accepted names out again, on
`feature get` and `feature check`, and go stale without any compiler noticing; see
[`kernel/policy/README.md`](../kernel/policy/README.md).

An id is a wire value with the same standing as an ioctl `nr`, and unlike an `nr` it is
persisted as well as transmitted. `save_config()` collects the current states and hands them
to `save_binary_config()`, which writes `/data/adb/ksu/.feature_config` as a magic, a
version, a `u32` pair count and then that many `[u32 id][u64 value]` pairs;
`load_binary_config()` reads those numbers back verbatim, and `init_features()` -- reached
from `on_post_data_fs()` and from the late-load path -- hands each one to
`KSU_IOCTL_SET_FEATURE` before the manager has started. Nothing in that file records what an
id meant when it was written. Renumber a shipped feature and every stored pair silently
retargets: yesterday's "mount hide off" becomes "whatever now owns id 5, off", applied at
boot with no error anywhere. The manager
fails the other way round. [`ksu.cc`](../manager/app/src/main/cpp/ksu.cc) calls
`get_feature()` and `set_feature()` with `KSU_FEATURE_MOUNT_HIDE` and its siblings by name,
so the numbering is frozen into the APK at build time and an installed manager keeps driving
the ids it was compiled with against a kernel that has since re-assigned them. Module
metadata is a third consumer, matching by the string from `name()` through
`parse_feature_id()`, which means a rename breaks module-managed features exactly where a
renumber breaks the config file.

Hence the reservation comment in the header. Upstream allocates ids densely from 0 upwards,
and PR #3630 took 5 for `KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT` -- the id this fork had already
shipped for `KSU_FEATURE_MOUNT_HIDE`. Rebasing onto that moved the fork's feature to 16, a
break taken once and deliberately in exchange for a rule that prevents the next one:
upstream owns 0 through 15, fork-local features start at 16. The gap is not free. Ids are
array indices in [`kernel/policy/feature.c`](../kernel/policy/feature.c), where
`feature_handlers[KSU_FEATURE_MAX]` now carries ten permanently NULL slots, and every
`ksu_register_feature_handler()` bound check compares against a `KSU_FEATURE_MAX` of 17. Ten
pointers is the cheaper side of the trade, because a collision is undetectable by anything
else in this directory: `ksu_get_info_cmd.features` reports a bound,
`KERNEL_SU_UAPI_VERSION` covers struct layout, and neither carries an opinion about what id
5 means. A fork-local feature that upstream later adopts should keep its 16+ id rather than
move down into the dense range.

### [`selinux.h`](selinux.h)

The opcodes for the sepolicy batch that `KSU_IOCTL_SET_SEPOLICY` carries: nine
`KSU_SEPOLICY_CMD_*` values and their `KSU_SEPOLICY_SUBCMD_*` subtypes. The payload framing
is not a struct; a comment in [`supercall.h`](supercall.h) describes it as a
`struct ksu_sepolicy_cmd_hdr` followed by `[u32 len][len bytes][NUL]` operands, with
`len == 0` meaning ALL and the operand count derived from the command. That count table is
duplicated three times -- in the header comment, in `sepol_expected_argc()` in
[`kernel/selinux/rules.c`](../kernel/selinux/rules.c), and in `cmd_expected_argc()` in
[`userspace/ksud/src/sepolicy.rs`](../userspace/ksud/src/sepolicy.rs). A disagreement
desynchronises the decoder's cursor and the kernel discards the whole batch; see
[`kernel/selinux/README.md`](../kernel/selinux/README.md).

### [`sulog.h`](sulog.h)

`struct ksu_sulog_event`, the fixed header preceding the variable-length filename and argv
blobs in an audit record, plus `KSU_SULOG_EVENT_VERSION` and the three event types. This is
the least self-contained header here: it includes `<linux/sched.h>`, uses the kernel's
`__packed` attribute, and defends `TASK_COMM_LEN` with an `#ifndef` fallback of 16. The
fallback is not idle defence, it fires on every kernel new enough to have turned
`TASK_COMM_LEN` into an anonymous *enum* constant instead of a macro -- 5.19 onwards, so the
6.1 through 6.18 half of the LKM build matrix but not the android12/13 5.10 and 5.15 half,
where it is still a plain `#define` and the `#ifndef` does nothing. Where it does fire, the
preprocessor test cannot see an enumerator, so the macro is defined anyway, shadowing it for
the rest of the translation unit. That is harmless only because both are 16, and
`get_task_comm()` carries a `BUILD_BUG_ON(sizeof(buf) != TASK_COMM_LEN)` that turns a
mismatch into a build failure rather than runtime corruption. Userspace's sanitised
`<linux/sched.h>` does not define the name at all, which is what makes the header usable by
bindgen.

Note what is *not* here: the 24-byte framing header wrapping each event on the wire,
`struct ksu_event_record_hdr`, lives in
[`kernel/infra/event_queue.h`](../kernel/infra/event_queue.h), so bindgen does not cover it.
`ksud` hand-writes `#[repr(C, packed)]` mirrors of both it and `ksu_sulog_event` in
[`sulog.rs`](../userspace/ksud/src/sulog.rs), and a one-sided edit turns every record into a
logged parse failure. [`kernel/sulog/README.md`](../kernel/sulog/README.md) has the detail.

### [`ksu.h`](ksu.h)

Five `#include` lines under one guard, and nothing else. It exists so that bindgen has a
single translation unit to parse: [`build.rs`](../userspace/ksud/build.rs) points at the
[`ksu_uapi.h`](../userspace/ksud/src/ksu_uapi.h) shim, the shim includes this file, and this
file pulls in the other five. The manager takes the same route from
[`cpp/ksu.h`](../manager/app/src/main/cpp/ksu.h). Only the kernel module includes the
individual headers directly, because Kbuild compiles each `.c` separately and there is
nothing to gain from dragging `sulog.h` and its `<linux/sched.h>` into a file that wants
only `app_profile.h`.

That makes this file the gate for both userspace consumers. A new header dropped into this
directory is invisible to ksud and to the manager's JNI until it is listed here, and the
symptom is an unresolved name at compile time -- a missing `pub const` in the generated
bindings, or an unknown struct in `jni.cc` -- rather than anything visible at runtime. Add
the `#include` in the same commit as the header.

## Adding a command

1. Put the argument struct in [`supercall.h`](supercall.h): fixed-width fields only,
   `__aligned_u64` / `__aligned_s64` for every 64-bit field and every user pointer, and a
   documented must-be-zero contract on any reserved field.
2. Add the `KSU_IOCTL_*` constant with a **fresh** `nr`. Prefer the sized form
   (`_IOR` / `_IOW` / `_IOWR` with the struct) for anything new: it costs nothing and turns
   a future layout mistake into `-ENOTTY` instead of silent corruption.
3. Write the handler in [`kernel/supercall/dispatch.c`](../kernel/supercall/dispatch.c) as
   `static int do_x(void __user *arg)`, returning a negative errno or a non-negative value
   the caller can use (an fd, a count). Cast user pointers through `(uintptr_t)`.
4. Add a row to `ksu_ioctl_handlers[]` with a non-NULL `.perm_check` from
   [`kernel/supercall/perm.c`](../kernel/supercall/perm.c). A NULL `.perm_check` is treated
   as allowed -- the dispatcher calls it only when it is non-NULL -- so there is no default
   deny. Keep the row before the sentinel row whose `.handler` is NULL, since a NULL handler
   in the middle truncates the table for both the scan loop and
   `ksu_supercall_dump_commands()`.
5. If you changed the shape of an *existing* struct, bump `KERNEL_SU_UAPI_VERSION` and
   record why in the comment above it. If the struct was `app_profile`, also bump
   `KSU_APP_PROFILE_VER`, add a `migrate_profile()` case and a new stride constant in
   [`kernel/policy/allowlist.c`](../kernel/policy/allowlist.c).
6. Rebuild every consumer the change touches: the kernel module, `ksud` (bindgen picks it
   up automatically), the manager's JNI, and, if `struct ksu_get_info_cmd` moved, the
   copies in [`userspace/ksuinit/src/lib.rs`](../userspace/ksuinit/src/lib.rs).

Nothing in CI checks any of this. These headers are not even covered by the clang-format
workflow (`make check-format` runs `find .` from `kernel/`, which does not follow the
`include/uapi` symlink), no test compares the version number against the header contents,
and no build step diffs the consumers against each other. Review is the gate.

## See also

- [`kernel/supercall/README.md`](../kernel/supercall/README.md) -- the ioctl control plane
- [`kernel/policy/README.md`](../kernel/policy/README.md) -- app profiles, feature registry
- [`kernel/selinux/README.md`](../kernel/selinux/README.md) -- the sepolicy batch receiver
- [`kernel/README.md`](../kernel/README.md) -- module layers, build modes, init order
- [`userspace/ksud/README.md`](../userspace/ksud/README.md) -- the Rust client and bindgen
- [`userspace/ksuinit/README.md`](../userspace/ksuinit/README.md) -- the hardcoded exception
- [`manager/README.md`](../manager/README.md) -- the JNI bridge and the Kotlin mirrors
- [`docs/architecture.md`](../docs/architecture.md) -- the end-to-end picture

<!-- reference links: kernel documentation and man pages -->
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[uprobetracer]: https://docs.kernel.org/trace/uprobetracer.html
