# kernel/supercall: the ioctl control plane

Every request userspace makes of the KernelSU module arrives through one file
descriptor and lands in one function. There is no character device, no `/proc`
file, no sysfs attribute and, in this fork, no [`prctl`][prctl-2] hook. The descriptor is an
anonymous inode named `[ksu_driver]`; its `file_operations` route both
`unlocked_ioctl` and `compat_ioctl` into `ksu_supercall_handle_ioctl()`, and that
function is a linear scan over a table pairing each command number with a handler
and a permission predicate.

Three files carry the area: [`supercall.c`](supercall.c) builds the descriptor in
`ksu_install_fd()` and owns the reboot handshake that hands one out,
[`dispatch.c`](dispatch.c) holds the table and all 29 handlers, and
[`perm.c`](perm.c) is thirty lines and five predicates. Only one of the two ways to
acquire a descriptor lives here; the other is in
[`hook/setuid_hook.c`](../hook/setuid_hook.c), which reaches `ksu_install_fd()` through
[`supercall.h`](supercall.h) when zygote forks the manager.
[`internal.h`](internal.h) keeps the intra-area prototypes and
[`supercall.h`](supercall.h) exposes only `ksu_install_fd()`, the init/exit pair and
the table row type. Command numbers and argument structs live outside the kernel
tree, in [`uapi/supercall.h`](../../uapi/supercall.h), because ksud and the Android
manager compile against that header too.

## What the descriptor is

An anonymous inode is a `struct file` with no name anywhere in the filesystem.
`anon_inode_getfile()` attaches it to a single inode the kernel allocates at boot, and
the `alloc_file_pseudo()` underneath gives it only an unhashed pseudo-dentry, so nothing
a path lookup can reach ever names the file: no device node, no directory entry a scan
of `/dev` or `/proc` could turn up. Its only visible trace is the `/proc/<pid>/fd/<n>`
symlink, whose target reads `anon_inode:[ksu_driver]` -- the string ksud and the manager
both scan `/proc/self/fd` for. ksuinit never scans; it only runs the handshake.
`ksu_install_fd()` publishes a descriptor in two phases:
`get_unused_fd_flags(O_CLOEXEC)` reserves a descriptor number,
`anon_inode_getfile()` builds the file, `fd_install()` binds them, and
`put_unused_fd()` returns the reservation if the middle step fails, because a
reserved-but-unbound number leaks for the life of the process.

```c
static const struct file_operations anon_ksu_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = anon_ksu_ioctl,
    .compat_ioctl = anon_ksu_ioctl,
    .release = anon_ksu_release,
};
```

Three properties of that struct do real work. There is no `read`, `write` or
[`mmap`][mmap-2], so `read(2)` and `write(2)` fail with `-EINVAL` inside
`vfs_read()`/`vfs_write()` -- the absent methods leave `FMODE_CAN_READ` and
`FMODE_CAN_WRITE` clear on the file -- and [`mmap(2)`][mmap-2] fails with `-ENODEV` in
`do_mmap()`, all before KernelSU is reached. [`ioctl()`][ioctl-2] is the only operation that
carries data. `private_data` is passed as `NULL`, so the file carries no per-open state:
every descriptor is interchangeable, `anon_ksu_release()` is a single `pr_info()`, and a
whole class of per-descriptor use-after-free bugs cannot arise. And `.owner` is
`THIS_MODULE`, which matters because `__anon_inode_getfile()` runs `if (fops->owner &&
!try_module_get(fops->owner))`, so every live descriptor pins the module and
`delete_module("kernelsu")` fails while one is open. That is the contract
[`userspace/ksud/src/unload.rs`](../../userspace/ksud/src/unload.rs) relies on: it kills
every process holding a `[ksu_driver]` or `[ksu_fdwrapper]` descriptor and closes its own
before calling `delete_module`.

## The install handshake

A [kprobe][kprobes] is a breakpoint a module can plant on any function it can name.
`ksu_supercalls_init()` plants one on `REBOOT_SYMBOL`, which
[`kernel/include/arch.h`](../include/arch.h) defines as `__arm64_sys_reboot` on
arm64 and `__x64_sys_reboot` on x86_64. On both architectures that `SYSCALL_DEFINE4`
wrapper takes exactly one argument, `const struct pt_regs *`, so the pre-handler
must first fetch the user register frame out of its own probe registers -- what
`PT_REAL_REGS(regs)` does -- and only then read the syscall's arguments.

```c
    if (magic1 == KSU_INSTALL_MAGIC1 && magic2 == KSU_INSTALL_MAGIC2) {
        struct ksu_install_fd_tw *tw;
        unsigned long arg4 = (unsigned long)PT_REGS_SYSCALL_PARM4(real_regs);

        tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
        if (!tw)
            return 0;

        tw->outp = (int __user *)arg4;
        tw->cb.func = ksu_install_fd_tw_func;
```

The magic pair is `0xDEADBEEF` and `0xCAFEBABE`, and nothing about the descriptor is
created here. A kprobe pre-handler runs with preemption disabled and possibly with
interrupts off, while `get_unused_fd_flags()`, `anon_inode_getfile()` and
`copy_to_user()` can all sleep or fault, so doing the work inline would deadlock or
oops. The handler allocates a small `GFP_ATOMIC` record and calls
`task_work_add(current, &tw->cb, TWA_RESUME)`, which queues a callback to run on the
task's own return to userspace, where sleeping and faulting are legal.
`ksu_install_fd_tw_func()` then installs the descriptor, copies the number out
through the pointer passed as the syscall's fourth argument, and closes it again
with `ksu_close_fd()` from [`kernel/include/util.h`](../include/util.h) if that copy
faults. Note that the fourth *syscall* argument is not the fourth *C* argument on
x86_64 -- the syscall ABI puts it in `r10`, the C ABI in `rcx` -- which is why
`arch.h` keeps `__PT_SYSCALL_PARM4_REG` and `__PT_CCALL_PARM4_REG` apart.

The pre-handler returns 0, so the real `sys_reboot()` still runs. In the accompanying
6.1 tree it checks `ns_capable(pid_ns->user_ns, CAP_SYS_BOOT)` first and only
afterwards validates `LINUX_REBOOT_MAGIC1`, so an unprivileged caller gets `-EPERM`
and a root caller gets `-EINVAL`. Either way nothing reboots and the return value is
meaningless; userspace reads the out-parameter instead, as `init_driver_fd()` in
[`userspace/ksud/src/ksucalls.rs`](../../userspace/ksud/src/ksucalls.rs) and
`has_kernelsu_v2()` in
[`userspace/ksuinit/src/lib.rs`](../../userspace/ksuinit/src/lib.rs) both do.

The second install path skips the handshake. Android app processes run under a
[seccomp][seccomp-filter] filter that does not permit [`reboot(2)`][reboot-2], so the
manager could never issue the magic pair. Instead `ksu_handle_setresuid()` in
[`hook/setuid_hook.c`](../hook/setuid_hook.c) fires when zygote drops to an app uid. For the
manager's uid it takes `current->sighand->siglock`, sets the `__NR_reboot` bit in the
seccomp constant-action cache through `ksu_seccomp_allow_cache()` in
[`infra/seccomp_cache.c`](../infra/seccomp_cache.c) and marks the task for the syscall
[tracepoint][tracepoints], then drops the lock and calls `ksu_install_fd()` -- which has to
happen outside the spinlock, because installing a descriptor can sleep. The manager app is
therefore born already holding one, which is why `scan_driver_fd()` in
[`manager/app/src/main/cpp/ksu.cc`](../../manager/app/src/main/cpp/ksu.cc) only walks
`/proc/self/fd`. An allow-listed non-manager uid gets the seccomp repair but no descriptor,
and must run the handshake itself. The file is `O_CLOEXEC` and so does not survive
[`execve`][execve-2]: the manager keeps it because zygote forks without exec'ing, while ksud
re-acquires it after every exec by scanning first and falling back to the handshake.

## Why an anonymous inode

A character device needs a major/minor pair, a node under `/dev`, an [SELinux][selinux]
label and a `devnode` rule in Android's policy: four artefacts an integrity check can
enumerate, and four things that must exist before the module can be spoken to. A `/proc` or
sysfs entry has the same problem with a path instead of a device number. A new syscall would
mean a KernelSU-specific `sys_call_table` entry pointing outside vmlinux, which any scan of
that table finds. An anonymous inode has none of that, and comes into existence per-process
at the moment a process is entitled to it. Upstream KernelSU used a `prctl` hook for the
same job; this fork does not, and no source file under `kernel/` mentions `prctl`. Two
client-side probes for the older interface survive -- `has_kernelsu_legacy()` in
[`userspace/ksuinit/src/lib.rs`](../../userspace/ksuinit/src/lib.rs) and `legacy_get_info()`
in the manager's [`ksu.h`](../../manager/app/src/main/cpp/ksu.h), both issuing
`prctl(0xDEADBEEF, 2, ...)` -- reached only as a fallback after the ioctl channel reported a
zero version. Against this kernel the version out-parameter keeps whatever the caller
initialised it to, 0 in ksuinit and -1 in the manager, and both probes read that as absent.

## The dispatch table

`ksu_ioctl_handlers[]` is a static array of `struct ksu_ioctl_cmd_map`, each row
naming a command number, a printable name, a handler and a permission predicate,
terminated by a sentinel whose `.handler` is `NULL`.

```c
    for (i = 0; ksu_ioctl_handlers[i].handler; i++) {
        if (cmd == ksu_ioctl_handlers[i].cmd) {
            // Check permission first
            if (ksu_ioctl_handlers[i].perm_check && !ksu_ioctl_handlers[i].perm_check()) {
                pr_warn("ksu ioctl: permission denied for cmd=0x%x uid=%d\n", cmd, current_uid().val);
                return -EPERM;
            }
            // Execute handler
            return ksu_ioctl_handlers[i].handler(argp);
        }
    }
```

A table rather than a `switch`, for two independent reasons. The `KSU_IOCTL_*` values
in `uapi/supercall.h` are `static const __u32` objects, not preprocessor macros, and
a C compiler rejects such an object as a `case` label while an array initializer
accepts it. More importantly, binding the predicate to the command in the same row
makes it structurally impossible to add a command and forget the check; the
alternative -- an `if (!only_root()) return -EPERM;` at the top of each handler -- is
the pattern that leaks a privileged command the first time someone copies a handler
as a template. Running the predicate first also means a rejected caller never gets a
`copy_from_user()` against a pointer the kernel has already decided not to trust. An
unmatched number falls out of the loop to `-ENOTTY`, and
`ksu_supercall_dump_commands()` prints the whole table to dmesg at init.

| Command | Number | Dir | Payload struct | Permission | Handler |
| --- | --- | --- | --- | --- | --- |
| `KSU_IOCTL_GRANT_ROOT` | `0x00004b01` | none | (none) | `allowed_for_su` | `do_grant_root` |
| `KSU_IOCTL_GET_INFO` | `0x80104b02` | R | `ksu_get_info_cmd` | `always_allow` | `do_get_info` |
| `KSU_IOCTL_GET_INFO_LEGACY` | `0x80004b02` | R | `ksu_get_info_legacy_cmd` | `always_allow` | `do_get_info_legacy` |
| `KSU_IOCTL_REPORT_EVENT` | `0x40004b03` | W | `ksu_report_event_cmd` | `only_root` | `do_report_event` |
| `KSU_IOCTL_SET_SEPOLICY` | `0xc0004b04` | RW | `ksu_set_sepolicy_cmd` | `only_root` | `do_set_sepolicy` |
| `KSU_IOCTL_CHECK_SAFEMODE` | `0x80004b05` | R | `ksu_check_safemode_cmd` | `always_allow` | `do_check_safemode` |
| `KSU_IOCTL_GET_ALLOW_LIST` (deprecated) | `0xc0004b06` | RW | `ksu_get_allow_list_cmd` | `manager_or_root` | `do_get_allow_list` |
| `KSU_IOCTL_GET_DENY_LIST` (deprecated) | `0xc0004b07` | RW | `ksu_get_allow_list_cmd` | `manager_or_root` | `do_get_deny_list` |
| `KSU_IOCTL_NEW_GET_ALLOW_LIST` | `0xc0044b06` | RW | `ksu_new_get_allow_list_cmd` | `manager_or_root` | `do_new_get_allow_list` |
| `KSU_IOCTL_NEW_GET_DENY_LIST` | `0xc0044b07` | RW | `ksu_new_get_allow_list_cmd` | `manager_or_root` | `do_new_get_deny_list` |
| `KSU_IOCTL_UID_GRANTED_ROOT` | `0xc0004b08` | RW | `ksu_uid_granted_root_cmd` | `manager_or_root` | `do_uid_granted_root` |
| `KSU_IOCTL_UID_SHOULD_UMOUNT` | `0xc0004b09` | RW | `ksu_uid_should_umount_cmd` | `manager_or_root` | `do_uid_should_umount` |
| `KSU_IOCTL_GET_MANAGER_APPID` | `0x80004b0a` | R | `ksu_get_manager_appid_cmd` | `manager_or_root` | `do_get_manager_appid` |
| `KSU_IOCTL_GET_APP_PROFILE` | `0xc0004b0b` | RW | `ksu_get_app_profile_cmd` | `only_manager` | `do_get_app_profile` |
| `KSU_IOCTL_SET_APP_PROFILE` | `0x40004b0c` | W | `ksu_set_app_profile_cmd` | `only_manager` | `do_set_app_profile` |
| `KSU_IOCTL_GET_FEATURE` | `0xc0004b0d` | RW | `ksu_get_feature_cmd` | `manager_or_root` | `do_get_feature` |
| `KSU_IOCTL_SET_FEATURE` | `0x40004b0e` | W | `ksu_set_feature_cmd` | `manager_or_root` | `do_set_feature` |
| `KSU_IOCTL_GET_WRAPPER_FD` | `0x40004b0f` | W | `ksu_get_wrapper_fd_cmd` | `manager_or_root` | `do_get_wrapper_fd` |
| `KSU_IOCTL_MANAGE_MARK` | `0xc0004b10` | RW | `ksu_manage_mark_cmd` | `manager_or_root` | `do_manage_mark` |
| `KSU_IOCTL_NUKE_EXT4_SYSFS` | `0x40004b11` | W | `ksu_nuke_ext4_sysfs_cmd` | `manager_or_root` | `do_nuke_ext4_sysfs` |
| `KSU_IOCTL_ADD_TRY_UMOUNT` | `0x40004b12` | W | `ksu_add_try_umount_cmd` | `manager_or_root` | `add_try_umount` |
| `KSU_IOCTL_SET_INIT_PGRP` | `0x00004b13` | none | (none) | `only_root` | `do_set_init_pgrp` |
| `KSU_IOCTL_GET_SULOG_FD` | `0x40044b14` | W | `ksu_get_sulog_fd_cmd` | `only_root` | `do_get_sulog_fd` |
| `KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT` | `0x00004b15` | none | (none) | `only_root` | `do_disable_escape_to_root` |
| `KSU_IOCTL_SET_SPOOF_VERSION` | `0x40004b2a` | W | `ksu_set_spoof_version_cmd` | `only_root` | `do_set_spoof_version` |
| `KSU_IOCTL_SET_SPOOF_CPU` | `0x40004b2b` | W | `ksu_set_spoof_cpu_cmd` | `only_root` | `do_set_spoof_cpu` |
| `KSU_IOCTL_SET_SPOOF_MEM` | `0x40004b2c` | W | `ksu_set_spoof_mem_cmd` | `only_root` | `do_set_spoof_mem` |
| `KSU_IOCTL_PTCTL` | `0xc0384b32` | RW | `ksu_ptctl_cmd` | `only_root` | `do_ptctl` |
| `KSU_IOCTL_UHOOK` | `0xc0904b33` | RW | `ksu_uhook_cmd` | `only_root` | `do_uhook` |

Ten of those have no caller anywhere in this repository: the two deprecated list
commands, `NEW_GET_DENY_LIST`, `UID_GRANTED_ROOT`, `GET_MANAGER_APPID`, the three
`SET_SPOOF_*` commands, and `PTCTL` and `UHOOK`. The last two are implemented in
[`feature/ptctl.c`](../feature/ptctl.c) and [`feature/uhook.c`](../feature/uhook.c),
documented for callers in [`docs/instrumentation.md`](../../docs/instrumentation.md),
and reachable only from a root-owned tool built against the UAPI header.
Three `nr` values are also reused: `nr` 2 carries both `GET_INFO` (size 16) and
`GET_INFO_LEGACY` (size 0), and `nr` 6 and 7 carry both the deprecated 128-entry
list commands (size 0) and their flexible-array replacements (size 4). Only the size
field of the encoding tells them apart.

## Permission classes

All five predicates fit on one screen. They are uid- and SELinux-domain-based, never
`capable()`-based, because the caller is an Android app or a shell whose capability
set says nothing useful about whether KernelSU should trust it.

`always_allow()` returns true and gates `GET_INFO`, `GET_INFO_LEGACY` and
`CHECK_SAFEMODE`, the presence and safe-mode queries any holder of the descriptor
may make. `only_root()` compares `current_uid()`, the *real* uid, against zero: a
setuid-root binary whose real uid is not zero fails it, while a process that dropped
its effective uid but kept a real uid of zero passes. It gates the ten commands that
reconfigure the kernel -- policy loading, boot-event reporting, the sulog stream,
the three spoofing commands, `SET_INIT_PGRP`, the per-thread escalation veto, and
`PTCTL`/`UHOOK`. `only_manager()` is `is_manager()` from
[`manager/manager_identity.h`](../manager/manager_identity.h), which compares
`current_uid().val % KSU_PER_USER_RANGE` -- 100000, so secondary Android users work
-- against the `ksu_manager_appid` global that the throne tracker sets after
verifying the APK's v2 signing certificate. It gates only `GET_APP_PROFILE` and
`SET_APP_PROFILE`: writing per-app policy is the manager's job alone.
`manager_or_root()` is the disjunction of the two, and covers the thirteen commands
in the middle of the table.

`allowed_for_su()` is the interesting one. It gates `GRANT_ROOT` and reads `is_manager() ||
ksu_is_allow_uid_for_current(current_uid().val)`; that second call reaches
`__ksu_is_allow_uid_for_current()` in [`policy/allowlist.c`](../policy/allowlist.c), where a
non-zero uid is looked up in the [RCU][whatisrcu] hash table under `rcu_read_lock()` for a
`perm_data` with `profile.allow_su` set, while uid 0 is *not* accepted outright but deferred
to `is_ksu_domain()` in [`selinux/selinux.c`](../selinux/selinux.c). An already-root process
is trusted only when it is running in `u:r:ksu:s0`.

There is no daemon token in this fork. `uapi/supercall.h` declares
`struct ksu_become_daemon_cmd { __u8 token[65]; }`, but no `KSU_IOCTL_BECOME_DAEMON`
constant exists, no row references it, no predicate consults a token, and
`git log -S become_daemon` shows the struct arrived with the original supercall
rewrite as a forward declaration no code ever grew into. Both trust anchors this
fork does use are established before any ioctl is issued: the manager APK's
certificate size and SHA-256, pinned at build time in [`kernel/Kbuild`](../Kbuild),
and the `u:r:ksu:s0` domain that [`selinux/rules.c`](../selinux/rules.c) injects into
a duplicated policydb. A shared secret would add nothing on top of those and would
need somewhere safe to live.

## Handler conventions

Every handler has the signature `int (*)(void __user *arg)` and the dispatcher
returns `long`; a negative return becomes the ioctl's errno, anything else reaches
userspace verbatim. Pure-output commands build the struct on the kernel stack and
copy it out whole; in/out commands `copy_from_user`, mutate, copy back. Large
structs are copied piecewise: `do_get_app_profile()` reads only the uid at
`arg + offsetof(struct ksu_get_app_profile_cmd, profile.curr_uid)` and writes back
only the `profile` member. Strings go through `strncpy_from_user()` into a 256-byte
stack buffer, but only `do_nuke_ext4_sysfs()` checks for truncation, returning
`-ENAMETOOLONG` when the result exactly fills the buffer. `add_try_umount()` tests only
for `len <= 0`, then forces a NUL at byte 255, so an over-long mountpoint is silently
shortened and registered in `mount_list` under a path that is not the one the caller
asked to unmount.

Descriptor-returning commands hand the number back as the ioctl return value rather
than through the struct: `do_get_wrapper_fd()` returns
`ksu_install_file_wrapper(cmd.fd)` from
[`infra/file_wrapper.c`](../infra/file_wrapper.c) and `do_get_sulog_fd()` returns
`ksu_install_sulog_fd()` from [`sulog/fd.c`](../sulog/fd.c). That avoids a second
`copy_to_user` and the window in which a descriptor exists in the process's table but
its number has not reached the caller. `do_get_wrapper_fd()` also refuses with
`-EINVAL` while `ksu_file_sid` is zero -- before `cache_sid()` has resolved
`u:object_r:ksu_file:s0` -- since a wrapper created without the relabel would carry
the creating task's SID and defeat its purpose.

`do_ptctl()` and `do_uhook()` copy the command struct back *unconditionally*, before
returning the handler's status:

```c
    if (copy_from_user(&cmd, arg, sizeof(cmd)))
        return -EFAULT;
    ret = ksu_ptctl(&cmd);
    if (copy_to_user(arg, &cmd, sizeof(cmd)))
        return -EFAULT;
    return ret;
```

`do_ptctl()` fans out to twelve operations and `do_uhook()` to five, several of which
fill in `cmd.ret`, `cmd.arg1` and -- for ptctl -- `cmd.arg2`, or `cmd.lost` for a uhook
`READ`, on the way to an error. No other channel exists for those, so an early return on
failure would throw away the only diagnostic.

## ABI rules for a new command

Everything a new command needs on the wire goes under `uapi/` and nowhere else: the
number and its argument struct in [`uapi/supercall.h`](../../uapi/supercall.h), and the
types those structs embed in [`uapi/app_profile.h`](../../uapi/app_profile.h), which
defines the `struct app_profile` both profile commands carry, and
[`uapi/feature.h`](../../uapi/feature.h), which holds the `enum ksu_feature_id` the
feature commands index by. The kernel reaches those headers through the symlink
`kernel/include/uapi -> ../../uapi`, ksud through bindgen on
[`userspace/ksud/src/ksu_uapi.h`](../../userspace/ksud/src/ksu_uapi.h) with `-I../../`,
and the manager through `manager/app/src/main/cpp/uapi -> ../../../../../uapi`. Never
copy a constant into a client that includes the header, and there is exactly one that
does not: ksuinit runs no bindgen, so
[`userspace/ksuinit/src/lib.rs`](../../userspace/ksuinit/src/lib.rs) spells the install
magics and both `GET_INFO` numbers as literals. Grow `struct ksu_get_info_cmd` and the
sized `0x80104b02` matches no row; detection survives only because the probe falls back
to `0x80004b02`, which encodes no size, and what it loses is the `uapi_version` field.
[`uapi/README.md`](../../uapi/README.md) is the contract document; what follows is the
part that bites in this directory.

Feature ids are wire values on the same footing. `GET_FEATURE` and `SET_FEATURE` carry
an index into `enum ksu_feature_id`, and ksud writes those raw `__u32` ids into
`.feature_config` in its working directory and replays them at the next boot through
`apply_config()` in
[`userspace/ksud/src/feature.rs`](../../userspace/ksud/src/feature.rs), so an id is
durable state on the device rather than a compile-time agreement between two headers.
Upstream allocates from 0 upward and owns 0 through 5, the last being
`KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT`. That one is why the reservation comment in
[`uapi/feature.h`](../../uapi/feature.h) exists at all: it claimed id 5, which this
fork's `KSU_FEATURE_MOUNT_HIDE` had been occupying, so mount hide moved to 16 and
everything from 16 up is now spoken for by fork-local features. Without that gap the
next upstream feature renumbers the fork's own again, and every `.feature_config`
already written to a device toggles the wrong thing at the following boot. Closing the
gap by dropping one of the two is not available either, because they answer different
questions: id 5 unmounts, changing what is mounted for the WebView zygote's uid and the
children it forks, while id 16 filters, changing what `/proc/<pid>/mountinfo` prints for
any isolated reader.

Every id also has to be mirrored into ksud by hand. The one header `ksu_uapi.h` includes
is [`uapi/ksu.h`](../../uapi/ksu.h), which pulls in `uapi/feature.h` along with the rest,
so a generated `ksu_feature_id` is sitting right there in the bindings -- but
`feature.rs` ignores it and declares its own `FeatureId` with literal discriminants,
which leaves the ids written down twice in one binary with nothing to catch them
drifting apart. A new id is seven edits in that file: the `FeatureId` enum, `from_u32()`,
`name()`, `description()`, `parse_feature_id()`, and the `all_features` arrays inside
`list_features()` and `save_config()`. Missing the last of those is the quiet failure.
`apply_config()` falls back to a raw `set_feature(id, value)` for an id it does not
recognise, so replaying an existing config keeps working and nothing looks broken, but
`save_config()` only walks its own array -- so the feature vanishes from
`.feature_config` the next time ksud writes state back out.

The consequence for this directory is that `do_get_info()` publishes `KSU_FEATURE_MAX`,
now 17, in `cmd.features` while 6 through 15 stay unassigned: an upper bound with a hole
in it, never a count. The hole itself costs nothing, because `feature_handlers[]` in
[`policy/feature.c`](../policy/feature.c) is sized `KSU_FEATURE_MAX` and an unassigned id
is an ordinary index that finds a NULL entry. A client that wants to know what the kernel
supports still asks `GET_FEATURE` per id and reads `cmd.supported`, which
`ksu_get_feature()` sets from whether a handler is registered.

Add a row with a non-NULL `.perm_check`. The dispatcher's guard is
`if (perm_check && !perm_check())`, so a row that omits the predicate is treated as
*allowed*, not denied; there is no default-deny. Keep the sentinel row -- `.cmd = 0`
with a NULL `.name`, `.handler` and `.perm_check` -- last, because both the scan loop and
`ksu_supercall_dump_commands()` stop at the first NULL handler, and a NULL handler
in the middle silently truncates the table.

Force-align every 64-bit field. `.compat_ioctl` and `.unlocked_ioctl` point at the
same function and `anon_ksu_ioctl()` casts `arg` straight to `void __user *` with no
`compat_ptr()`, so a 32-bit caller executes the identical handler body against the
identical struct -- which only holds if the layout is identical under both ABIs, and
i386 aligns `__u64` to 4 bytes where every 64-bit architecture aligns it to 8. The
header applies `__aligned_u64` / `__aligned_s64` to the ptctl and uhook structs and
to every pointer-carrying field, but not everywhere: compiled `-m32` against `-m64`,
`struct app_profile` measures 768 versus 784 bytes, `struct ksu_get_feature_cmd` 16
versus 24 with `value` at offset 4 versus 8, and `struct ksu_set_spoof_cpu_cmd` 28
versus 32. On arm64 that is latent, because the 32-bit arm EABI aligns `long long`
to 8; on x86_64 with an i386 client it is a live mismatch, and x86_64 is no longer a
corner case now that [`build-all-x64.sh`](../build-all-x64.sh) builds the module for
every KMI from android12-5.10 to android17-6.18. Use the aligned types unconditionally
for anything new.

A handler that writes into its argument struct has to copy it back even when it fails.
An op-multiplexing command fills `cmd.ret` or `cmd.arg1` on the way to an error, and the
ioctl return value carries nothing but the errno, so returning early on failure destroys
the only diagnostic the caller will ever see. `do_ptctl()` and `do_uhook()` are the
pattern to copy: `copy_from_user`, call the implementation, `copy_to_user`, and only
then return its status.

Any layout change costs a `KERNEL_SU_UAPI_VERSION` bump. Most commands use the raw
`_IOC(dir, 'K', nr, 0)` form, deliberately encoding a size of zero. Encoding
`sizeof(struct)` would change the number whenever the struct changed, turning a
stale caller into a clean `-ENOTTY`; encoding zero keeps the number stable so a
struct can grow without burning a new `nr`. The cost is that the dispatcher cannot
detect a mismatched caller at all, and the compensating mechanism is the integer
`KERNEL_SU_UAPI_VERSION`, reported by `do_get_info()` and checked by
`ensure_uapi_version_matched()` at each ksud boot stage in
[`userspace/ksud/src/init_event.rs`](../../userspace/ksud/src/init_event.rs) and again
before a module install in
[`userspace/ksud/src/module.rs`](../../userspace/ksud/src/module.rs), and by
`checkUAPIMismatch()` in the manager's
[`Natives.kt`](../../manager/app/src/main/java/me/weishu/kernelsu/Natives.kt), which the
home screen re-runs on every refresh. The kernel never enforces it, so a client that
skips the check mis-marshals in silence. For a new number prefer the sized form --
`_IOR` / `_IOW` / `_IOWR` with the struct, the way `GET_SULOG_FD`, `PTCTL` and `UHOOK`
already do. The stability argument above says why the numbers already shipped cannot be
re-minted, not why to add another one: a sized number turns a future layout mistake into
`-ENOTTY` at the dispatcher instead of a truncated `copy_from_user` inside the handler.

Watch the init order. `do_set_spoof_cpu()` calls `find_kernel_symbol_exact()` from
[`infra/symbol_resolver.c`](../infra/symbol_resolver.c), so
[`core/init.c`](../core/init.c) must keep `ksu_init_symbol_resolver()` ahead of
`ksu_supercalls_init()`, which it does with room to spare: the resolver is the first
subsystem `kernelsu_init()` brings up, with six init calls between it and the supercall
area. The call sequenced immediately after this area has the same dependency and is
worth knowing about, because it decides how `GRANT_ROOT` behaves.
`ksu_app_profile_init()` in
[`policy/app_profile.c`](../policy/app_profile.c) resolves `seccomp_filter_release`
through that same resolver and scans its body for a call to `_raw_spin_lock_irq`.
Kernels from 6.6 through 6.10 ship that function both with and without an AOSP backport
that changed how a task must look to it, and `disable_seccomp()` -- which `GRANT_ROOT`
reaches by way of `escape_with_root_profile()` -- fakes `PF_EXITING` when the backport
is present and a NULL `sighand` when it is not. The scan runs one call *after*
`register_kprobe()` has armed the reboot probe, so a `GRANT_ROOT` squeezed into that
window would take the NULL-`sighand` branch by default. Nothing in a normal boot gets
there, but a handler added here with its own init dependency should be sequenced on
purpose rather than by luck.

## State this area owns

`mount_list` and `mount_list_lock` are defined in [`dispatch.c`](dispatch.c), even
though their readers live in [`feature/kernel_umount.c`](../feature/kernel_umount.c)
and [`feature/mount_hide.c`](../feature/mount_hide.c) and only the `extern`
declarations sit in [`feature/kernel_umount.h`](../feature/kernel_umount.h).
`add_try_umount()` is the only writer userspace can reach; the other is
`ksu_supercall_cleanup_state()`, which drains the list at module exit. The ioctl serves
`KSU_UMOUNT_WIPE`, `KSU_UMOUNT_ADD` (which rejects a duplicate path with `-EEXIST`) and
`KSU_UMOUNT_DEL`. Entries go in at the head, so unmount order is the reverse of
registration order. The lock order constrains anyone adding a writer here.
`ksu_handle_umount()` takes
`mount_list_lock` for read and then calls `path_umount()`, which takes the VFS
`namespace_sem` for write. `ksu_region_has_mount_list()` in `mount_hide.c` runs
inside a seq-file `->show` callback with `namespace_sem` already held for read, so it
uses `down_read_trylock()` and skips its check on contention rather than inverting
that order and deadlocking against a queued writer -- and `add_try_umount()` is
exactly such a writer, so any new `down_write(&mount_list_lock)` must come from a
context holding no VFS namespace lock.

## Teardown

`ksu_supercalls_exit()` unregisters the reboot kprobe and then drains `mount_list`, freeing
each `kstrdup`'d path. It unregisters unconditionally even when `register_kprobe()` failed
at init, which is safe because `unregister_kprobe()` on a probe that was never armed returns
without touching anything. Ordering in `kernelsu_exit()` matters, and the comment in
[`core/init.c`](../core/init.c) says why: `ksu_syscall_hook_manager_exit()` goes first, to
stop new callbacks arriving, and `ksu_uhook_exit()` and `ksu_ptctl_exit()` follow
immediately, because [uprobe][uprobetracer] consumers, the killguard kprobe and any armed
hardware breakpoints hold pointers into module text that is about to be freed. Only then
does `ksu_supercalls_exit()` run, and only after that the `synchronize_rcu()` separating
hook teardown from data-structure teardown.

## Rough edges worth knowing

The kprobe fires at the *entry* of the reboot syscall wrapper, before
`ns_capable(CAP_SYS_BOOT)`. Any process permitted to issue `reboot(2)` at all can
obtain a `[ksu_driver]` descriptor regardless of capabilities. All confidentiality
therefore rests on the per-command predicate, and the three `always_allow` commands
make the descriptor a reliable KernelSU-presence oracle for anyone holding one.

`do_get_feature()` declares `bool supported;` without an initializer, and
`ksu_get_feature()` in [`policy/feature.c`](../policy/feature.c) returns `-EINVAL`
for an out-of-range `feature_id` *before* touching either output. The subsequent
`cmd.supported = supported ? 1 : 0;` and `if (ret && supported)` therefore read
uninitialised stack, and an out-of-range feature id is not reliably reported. Out of
range here means at or above `KSU_FEATURE_MAX`; the unassigned ids 6 through 15 that the
fork-local gap opened up are ordinary indices, find a NULL handler and come back
correctly as `supported = 0`, `value = 0`. An id of 17 or more is the one that takes the
bad path, and when the stale stack byte happens to read false the handler copies the
struct back and returns 0, leaving `cmd.value` holding whatever the caller sent in.

`GRANT_ROOT` returns 0 whether it escalated or declined: `escape_with_root_profile()`
in [`policy/app_profile.c`](../policy/app_profile.c) initialises `ret` to 0 and
reaches `out_abort_creds` with it unchanged both for "Already root, don't escape"
and for a thread carrying `TIF_KSU_DISABLE_ESCAPE_WITH_ROOT`. Separately, all of
`do_set_spoof_cpu()`'s spoofing work sits inside
`#if defined(CONFIG_ARM64) || defined(__aarch64__)` -- only the `copy_from_user()` and
the `cmd.cpu_index >= num_possible_cpus()` bounds check are outside it -- so on x86_64
the handler resolves nothing, writes nothing, logs
`"KernelSU Stealth: CPU %u identity spoofed (MIDR: 0x%08x)"` and returns 0. On arm64
every symbol-resolution failure is only a `pr_warn()`, so a partial spoof also
reports success.

## See also

- [`uapi/README.md`](../../uapi/README.md) -- the ABI contract these commands live in
- [`kernel/README.md`](../README.md) -- the module's layers and init order
- [`kernel/core/README.md`](../core/README.md) -- where init and exit sequence this area
- [`kernel/hook/README.md`](../hook/README.md) -- the setresuid path that installs the fd
- [`kernel/policy/README.md`](../policy/README.md) -- allowlist, profiles and features
- [`kernel/manager/README.md`](../manager/README.md) -- how `is_manager()` gets its answer
- [`kernel/infra/README.md`](../infra/README.md) -- file wrapper, seccomp cache, resolver
- [`kernel/feature/README.md`](../feature/README.md) -- ptctl, uhook, umount list readers
- [`userspace/ksud/README.md`](../../userspace/ksud/README.md) -- the Rust client
- [`userspace/ksuinit/README.md`](../../userspace/ksuinit/README.md) --
  the handshake caller, and the hardcoded numbers
- [`docs/architecture.md`](../../docs/architecture.md) -- the end-to-end picture

<!-- reference links: kernel documentation and man pages -->
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[ioctl-2]: https://man7.org/linux/man-pages/man2/ioctl.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[mmap-2]: https://man7.org/linux/man-pages/man2/mmap.2.html
[prctl-2]: https://man7.org/linux/man-pages/man2/prctl.2.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
[uprobetracer]: https://docs.kernel.org/trace/uprobetracer.html
[whatisrcu]: https://docs.kernel.org/RCU/whatisRCU.html
