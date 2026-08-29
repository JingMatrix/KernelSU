# The KernelSU kernel module

Everything under `kernel/` compiles into a single object named `kernelsu` -- one module, one
entry point, one exit point. Its job is to grant root to processes an on-device policy says
may have it, to give a userspace daemon a private control channel, and to keep both of those
facts out of view of ordinary Android apps. It does that from inside the kernel because most
of what it needs is not reachable from userspace at any privilege level: rewriting a syscall
argument mid-call, editing the live [SELinux][selinux] policy without a reload, filtering
the records a process sees in `/proc/self/mountinfo`.

The module is deliberately not a driver. It creates no device node and no `/proc` entry, and
a release build exposes no sysfs attribute either: both module parameters are registered with
permission `0`, which suppresses the file, and a release LKM build removes its own directory
from `/sys/module` outright. `CONFIG_KSU_DEBUG` is the exception -- it keeps that directory
alive and adds a writable `ksu_debug_manager_appid` parameter, so on a debug build that one
attribute does appear under `/sys/module/kernelsu/parameters/`.

The only *file* userspace ever gets is an anonymous-inode descriptor called `[ksu_driver]`,
described in [`supercall/README.md`](supercall/README.md); every other fd the module hands
out -- `[ksu_sulog]`, the wrapped file from `KSU_IOCTL_GET_WRAPPER_FD` -- comes back from an
ioctl on that one. Two entry points bypass the fd because they have to. A
[`reboot(2)`][reboot-2] carrying the two install magics is what hands out the driver fd in
the first place: a [kprobe][kprobes] on the reboot syscall recognises the magic pair and
queues the installation as task work. And the sucompat hooks on [`execve`][execve-2],
`execveat`, `newfstatat`, `faccessat` and `setresuid` act on ordinary syscalls issued by a
process holding nothing at all; [`hook/README.md`](hook/README.md) covers how those five are
routed.

What follows is the map of the module as a whole: the build modes, the init and exit order,
and the rules a change anywhere in the tree has to keep. Each layer directory has its own
README for the code it holds.

- [Two ways to build it, four ways to arrive](#two-ways-to-build-it-four-ways-to-arrive) -
  built-in versus LKM, and the four ways `kernelsu_init` is reached
- [The stack, layer by layer](#the-stack-layer-by-layer) - what each subdirectory owns, and
  where the layering is deliberately broken
- [Bring-up order, and why it is that order](#bring-up-order-and-why-it-is-that-order) - the
  fourteen-step prologue, the five positions that cannot move, and the two branches
- [Teardown, and the use-after-free it avoids](#teardown-and-the-use-after-free-it-avoids) -
  the two phases of `kernelsu_exit` and the barrier between them
- [Building](#building) - Kbuild, Kconfig, the version formula, and the scripts around them
- [What check_symbol.c is guarding](#what-check_symbolc-is-guarding) - unexported symbols,
  ksuinit's relocation pass, and the host-side gate that replaces the export check
- [Version and architecture](#version-and-architecture-support-as-the-code-expresses-it) -
  the KMI matrix, the x86_64 syscall hardening, and the arm64 stack canary
- [Rules a maintainer has to respect](#rules-a-maintainer-has-to-respect) - the seven
  invariants a change here has to keep

## Two ways to build it, four ways to arrive

[`Kconfig`](Kconfig) declares `CONFIG_KSU` as a `tristate`, so the same source tree
produces either a built-in component or a loadable module, and the choice changes what the
module can assume about when it runs.

### Built into the kernel

Built into the kernel (`CONFIG_KSU=y`), `module_init(kernelsu_init)` expands to a
`device_initcall`, so bring-up happens during boot with no userspace in existence yet. The
`MODULE` macro is undefined, which has two consequences worth knowing: `ksu_late_loaded`
is assigned `false` unconditionally, and `kernelsu_exit`, which carries `__exit` in every
build, lands in a `.exit.text` that the built-in link throws away. A built-in build has no
teardown path at all. [`setup.sh`](setup.sh) is the integration helper for this mode. Run
it from the kernel source root: it symlinks a KernelSU checkout's `kernel/` into the host
kernel's `drivers/kernelsu`, appends `obj-$(CONFIG_KSU) += kernelsu/` to
`drivers/Makefile`, and inserts `source "drivers/kernelsu/Kconfig"` before the `endmenu`
in `drivers/Kconfig`. As written it clones `https://github.com/tiann/KernelSU` and checks
out that repository's latest tag, so point it at this tree yourself if you want this fork
built in rather than upstream.

### Loaded as a module

Built as a loadable module (`CONFIG_KSU=m`), the resulting `kernelsu.ko` can be loaded at
two very different moments. The module tells them apart by looking at who loaded it and
records the answer in `ksu_late_loaded`, the flag both branches of `kernelsu_init` and the
whole teardown path key off:

```c
#ifdef MODULE
    ksu_late_loaded = (current->pid != 1);
#else
    ksu_late_loaded = false;
#endif
```

The early case is a patched boot image. `ksud boot-patch` writes a small static binary into
the ramdisk as `/init`, moving the stock init to `/init.real`; that binary is
[`userspace/ksuinit`](../userspace/ksuinit/README.md), and it asserts it is PID 1 before
calling [`init_module(2)`][init-module-2]. So `current->pid == 1` and `ksu_late_loaded`
stays false. The late case is `ksud late-load` on a running system, which daemonizes first,
so the loading process is some ordinary PID and the flag becomes true.

`ksud boot-patch-v2` is a second early path, and it never calls `init_module(2)` at all.
The injector appends `kernelsu.ko` to an aarch64 kernel `Image` as a capsule, widens the
reservation made by the `memblock_reserve()` call inside `arm64_memblock_init()` so the
appended bytes are not handed to the page allocator, and replaces one
`bl async_synchronize_full` inside `kernel_init()` with a branch into a bootstrap planted
in a text cave. The bootstrap copies the module into vmalloc space, stamps in the symbol
addresses the injector resolved offline, and calls the kernel's own `load_module()` --
routing its one `strndup_user()` call to a kernel-side duplicate, because the pre-exec PID
1 has no usable userspace mapping to copy an argument string from. `kernel_init()` is that
same PID 1 before it execs `/init`, so `current->pid == 1` here too and `ksu_late_loaded`
is again false. Nothing in this directory can tell the two early paths apart, which is
what makes them interchangeable from the module's side.

### Why the arrival path decides the init path

That distinction matters because the boot path is event-driven and the late path cannot
be. During a normal boot the module installs hooks and then waits: it watches for
`/system/bin/init second_stage` to patch the SELinux policy, for the first
`app_process -Xzygote` to know `/data` is mounted, and for `ksud` to report boot
milestones over the driver fd. A late load arrives after every one of those events has
already happened and can never be observed, so `kernelsu_init` performs the same steps
synchronously instead. Skipping that would leave the module loaded but useless: an
unpatched policy, uncached SELinux SIDs, an unlabelled credential template, an empty
allowlist and no manager.

## The stack, layer by layer

The table below is a reading order, ordered roughly by dependency depth. It is not an
enforced layering, and the note under it says where the includes contradict it. Each of the
ten layer directories carries its own README covering the files it holds; `include/` and
`tools/` hold too little to need one and are described here.

| Directory | What lives there |
| --- | --- |
| [`core/`](core/README.md) | `module_init`/`module_exit`, the global state, the module parameters, the arch workarounds |
| [`infra/`](infra/README.md) | Feature-agnostic plumbing: kallsyms resolution, an fd wrapper, an event queue, seccomp-cache repair, mount-namespace switching |
| [`hook/`](hook/README.md) | The four interception techniques: syscall dispatcher, direct table patching, [LSM][lsm] slot patching, kprobes; plus the rodata write primitive and the call-site scanner beside it |
| [`selinux/`](selinux/README.md) | Live policydb editing, the `ksu` domain, SID caching, setenforce/getenforce |
| [`policy/`](policy/README.md) | Who may become root and what root they get: the allowlist, App Profiles, the feature registry |
| [`manager/`](manager/README.md) | Identifying the manager APK by signature and resolving it to a UID |
| [`runtime/`](runtime/README.md) | The boot pipeline: `init.rc` splicing, safe-mode detection, the `ksud` handoff |
| [`supercall/`](supercall/README.md) | The `[ksu_driver]` anon inode and the ioctl dispatch table |
| [`sulog/`](sulog/README.md) | The su audit event queue and the `[ksu_sulog]` reader fd |
| [`feature/`](feature/README.md) | The user-visible features built on everything above: sucompat, kernel_umount, adb_root, sulog, selinux_hide, mount_hide, mem_spoof, ptctl, uhook |
| `include/` | Four tiny shared headers plus a symlink to the ABI |
| `tools/` | [`check_symbol.c`](tools/check_symbol.c), the host-side build gate |

Three directories are leaves and stay that way: `selinux/` includes no header from any other
layer, `infra/` reaches only `selinux/`, and `manager/` only `policy/`. Everywhere else the
include graph has cycles, two of them deliberate.

- `core/` is the module entry point, so it includes a header from eight of the nine other
  layers -- everything but `sulog/` -- purely to sequence their init and exit calls.
- `hook/` holds both the interception machinery and the call sites that decide what to do
  with an intercepted syscall, so it reaches into `feature/`, `policy/`, `manager/`,
  `runtime/`, `sulog/` and `supercall/`.
- Beyond those two, `policy/`, `runtime/`, `supercall/` and `sulog/` each include at least
  one `feature/` header as well.

So read the table as a reading order, and do not rely on a compile-time layering the
includes do not enforce.

Almost every file the module compiles pulls `include/` in; `infra/symbol_resolver.c`,
`infra/event_queue.c` and `feature/mem_spoof.c` reach neither it nor the `uapi/` symlink
beneath it, directly or through any header they include. [`include/ksu.h`](include/ksu.h)
declares the handful of globals the whole tree keys off -- `ksu_cred` (the privilege template
described under [Bring-up order](#bring-up-order-and-why-it-is-that-order)),
`ksu_late_loaded`, `allow_shell`, `backup_sepolicy` and `ksu_no_custom_rc` -- plus the
version macro.

[`include/klog.h`](include/klog.h) redefines `pr_fmt` so a file that includes it prefixes
every message with `KernelSU:`; it carries an `// IWYU pragma: keep` comment at each include
site because it looks unused. Six translation units skip it, and `klog.h` is the only thing
in the tree that touches `pr_fmt`, so nothing else supplies the tag on their behalf.
`infra/event_queue.c` and `supercall/perm.c` never log at all and lose nothing;
`feature/ptctl.c`, `feature/uhook.c` and `feature/mem_spoof.c` do log, and write their own
prefix into the format string instead; `infra/symbol_resolver.c` prints bare, so its five
lines reach the kernel log with nothing identifying them.

[`include/arch.h`](include/arch.h) abstracts `pt_regs` register names for arm64 and x86_64
and `#error`s on anything else.
[`include/util.h`](include/util.h) carries one version shim for closing a file descriptor.
And `include/uapi` is a symlink to the repository's [`uapi/`](../uapi/README.md), which is
what makes `#include "uapi/supercall.h"` resolve from kernel code.

## Bring-up order, and why it is that order

[`core/init.c`](core/init.c) is the only translation unit that owns `module_init` and
`module_exit`. Its init function runs an unconditional prologue of fourteen steps and then
branches on `ksu_late_loaded`.

### The prologue

| # | Step | Why it sits here |
| --- | --- | --- |
| 1 | x86_64 hardening check | Returns `-ENOSYS` before anything is allocated or patched; see [x86_64 syscall hardening](#x86_64-syscall-hardening) |
| 2 | `ksu_late_loaded` | Both branches below and `kernelsu_exit` read it |
| 3 | `ksu_cred = prepare_creds()` | The privilege template every later `override_creds()` borrows |
| 4 | `ksu_init_symbol_resolver()` | Caches the kallsyms helper the variant scan needs |
| 5 | `ksu_syscall_hook_init()` | Finds a spare `sys_ni_syscall` slot and publishes `ksu_dispatcher_nr` |
| 6 | `ksu_feature_init()` | Clears `feature_handlers[]`, so it must precede every registration |
| 7 | `ksu_sulog_init()` | Registers a feature handler, so it must follow step 6 |
| 8 | `ksu_adb_root_init()` | Registers a feature handler, so it must follow step 6 |
| 9 | `ksu_lsm_hook_init()` | Its whole body is a `pr_info` of the tracked-hook count |
| 10 | `ksu_selinux_hide_init()` | Registers a feature handler and resolves `sel_handle_status_ops`, so it must follow steps 4 and 6 |
| 11 | `ksu_supercalls_init()` | Registers the `reboot` kprobe that hands out the driver fd |
| 12 | `ksu_app_profile_init()` | Scans `seccomp_filter_release`, so it must follow step 4 |
| 13 | `ksu_ptctl_init()` | Resolves its task-access, kprobe and hardware-breakpoint helpers, so it must follow step 4 |
| 14 | `ksu_uhook_init()` | Resolves the uprobe register/unregister set, so it must follow step 4 |

Five of those positions are load-bearing rather than arbitrary, and the rest of this section
is why.

**`ksu_cred` comes first among the allocations** because it is the module's privilege template
-- a credential prepared once at init and never committed, which later code borrows with
`override_creds()` when it needs to touch a path an app UID cannot reach. Five files do
that, including the allowlist's task-work writer and the kernel unmounter. A NULL there
would oops inside a syscall hook, a long way from the failure. It is also why
`put_cred(ksu_cred)` is the last statement of the exit path.

**The symbol resolver comes before everything that resolves a symbol**, but not for the
reason the position suggests. `ksu_init_symbol_resolver()` is `__init` and caches at most one
lookup helper, and which one depends on the build: `kallsyms_on_each_match_symbol` on 6.1 and
newer, `kallsyms_on_each_symbol` below 5.19, and nothing at all in between, where
[`infra/symbol_resolver.c`](infra/symbol_resolver.c) calls `kallsyms_on_each_symbol`
directly. The cached match helper is the fast path `find_kernel_symbol_exact()` takes on 6.1
and newer; below that the same function falls through to `kallsyms_lookup_name()` whatever is
cached, and the cached `kallsyms_on_each_symbol` is read only by `resolve_symbol_variant()`,
which is reached only through `ksu_resolve_symbol_for_functable_hook()`.

So a plain lookup issued before this call still returns the right address. What breaks, and
only on kernels below 5.19, is the variant scan: with no helper cached
`resolve_symbol_variant()` iterates nothing and returns NULL, which is a silent miss rather
than an error. `ksu_app_profile_init()`, `ksu_ptctl_init()` and `ksu_uhook_init()` call
`find_kernel_symbol_exact()` directly; `ksu_syscall_hook_init()` reaches `sys_call_table`
through `ksu_resolve_symbol_for_functable_hook()`.

**`ksu_syscall_hook_init()` must precede `ksu_syscall_hook_manager_init()`** in either
branch, because it is what finds a spare `sys_ni_syscall` slot, patches the dispatcher into
it and publishes `ksu_dispatcher_nr`. The manager registers per-syscall handlers into a table
the dispatcher reads; without a dispatcher slot those registrations route nowhere. The
dispatcher design itself -- the borrowed slot, the tracepoint redirect and the per-task mark
that keeps it narrow -- is [`hook/README.md`](hook/README.md).

**`ksu_feature_init()` must precede every `ksu_register_feature_handler()` call**, and this
one is easy to break silently. The init function walks `feature_handlers[]` and clears
every slot, so a handler registered earlier is discarded without a warning and its feature
reports "unsupported" for the rest of the boot. Nothing enforces the ordering except the
sequence in `kernelsu_init`. Note also that `ksu_register_feature_handler()` is `__init`:
handlers may only be registered during module initialization, and the
`struct ksu_feature_handler` passed in must have static storage, since only the pointer is
kept.

**`ksu_app_profile_init()` registers nothing** and on most kernels compiles to nothing at
all: its whole body sits behind a version window from 6.6 to 6.11. Inside that window it
decides
how `disable_seccomp()` will get a task's [seccomp][seccomp-filter] filter released when an
app escalates. That function clears the seccomp fields of `current` under `siglock` and then
hands a doctored copy of the `task_struct` to the kernel's `seccomp_filter_release()`, which
opens with a self-check and returns having done nothing if the check fails -- older kernels
want a NULL `tsk->sighand`, newer ones want `PF_EXITING` set in `tsk->flags`. Which of the
two a given `android15-6.6` kernel was built with cannot be read from `LINUX_VERSION_CODE`,
because the change reached that branch as a backport some vendors took and some did not. So
the init function reads the running kernel instead: it resolves `seccomp_filter_release`,
asks `kallsyms_lookup_size_offset()` how many bytes long it is (falling back to 128 when
that fails), and scans those bytes for a `BL` whose target is `_raw_spin_lock_irq`, using
`scan_call_to()` from [`hook/patch_memory.h`](hook/patch_memory.h). The new form of the
function takes `siglock` and the old one does not, so a hit selects `PF_EXITING` and a miss
selects the NULL `sighand`. That is why the call sits after `ksu_init_symbol_resolver()`,
and why its answer on x86_64 is always "old form": `scan_call_to()` decodes AArch64 branch
encodings, and the x86_64 file carries a stub that returns NULL.

### The late-load branch

On the late-load branch the ordering constraints are tighter still.
`apply_kernelsu_rules()` runs first because it is what injects the `ksu` type into a
duplicated policydb; `cache_sid()` and `setup_ksu_cred()` immediately after it both
resolve `u:r:ksu:s0` against that freshly patched policy and would fail against the stock
one. `escape_to_root_for_init()` then moves the loading `ksud` process itself into that
domain, and it must happen before `setenforce(true)` -- the comment in the source says
why: once enforcement resumes, a process still in its old domain loses access to
`/data/app`, which the manager search needs. Finally `ksu_boot_completed = true` must
precede `track_throne(false)`, because `ksu_prune_allowlist()` refuses to prune while that
flag is false and would otherwise skip the first prune entirely.

### The boot branch

The boot branch does far less: hook manager, allowlist init, throne tracker,
`ksu_ksud_init()`, file wrapper. Loading the allowlist, starting the package observer and
crowning the manager are all deferred to the events described in
[`runtime/README.md`](runtime/README.md).

## Teardown, and the use-after-free it avoids

`kernelsu_exit` exists only in LKM builds and is split into two phases by explicit
comments.

### Phase one: stop every source of new callbacks

1. `ksu_syscall_hook_manager_exit()`, broken out below.
2. `ksu_uhook_exit()`, then `ksu_ptctl_exit()`.
3. `ksu_supercalls_exit()`.
4. `ksu_ksud_exit()`, and only when `!ksu_late_loaded`, mirroring the fact that
   `ksu_ksud_init()` only ran on the boot branch. Unregistering a kprobe that was never
   registered is not a crash but it is not a contract either.

`ksu_syscall_hook_manager_exit()` is the involved one, and its own order matters. It
unregisters the `sys_enter` [tracepoint][tracepoints] and waits out in-flight probes with
`tracepoint_synchronize_unregister()`; destroys the `syscall_regfunc` and `syscall_unregfunc`
kretprobes; drops the routing-table entries for `__NR_setresuid`, `__NR_execve`,
`__NR_execveat`, `__NR_newfstatat` and `__NR_faccessat`; and then calls
`ksu_syscall_hook_exit()`.

That function restores the patched syscall-table slots first and clears `syscall_hooks[]` and
`ksu_dispatcher_nr` only afterwards, so an in-flight syscall that already reached the
dispatcher still finds a valid slot number and a valid table to read. Two calls follow it:
`ksu_sucompat_exit()` and `ksu_setuid_hook_exit()`. The second is where
`ksu_kernel_umount_exit()` and `ksu_mount_hide_exit()` run, and that is the only place
mount_hide's patched procfs `file_operations` slots are put back.

### The barrier between the phases

A `synchronize_rcu()` separates the phases. [RCU][whatisrcu] is the kernel's
deferred-reclaim mechanism: a reader marks a critical section with `rcu_read_lock()` and
holds no lock, and a writer that has already unlinked an object waits for a grace period --
the point at which every CPU has passed through a context switch -- before freeing it. The
allowlist is an RCU-protected hash table, so a supercall handler can be walking it at the
moment the module is asked to unload. Waiting here is what makes phase two safe.

### Phase two: release data

Phase two then releases data in roughly reverse init order: the package observer, the
throne tracker, the allowlist, selinux_hide, the LSM hooks, adb_root, sulog, the feature
registry, and finally `put_cred(ksu_cred)`. Two of those still restore rodata function
pointers rather than only freeing memory, which is safe here for a reason spelled out under
[Rules a maintainer has to respect](#rules-a-maintainer-has-to-respect).

### The failure this ordering avoids

The failure this ordering avoids is specific. A tracepoint probe, a kprobe pre-handler, a
[uprobe][uprobetracer] consumer, an armed hardware breakpoint and a patched syscall-table
slot are all raw function pointers into the module's `.text`. None of them takes a module
reference. If `module_free()` runs while one is still reachable, the next hit executes freed
memory. Two mechanisms hedge against getting the ordering wrong: `anon_ksu_fops.owner` is
`THIS_MODULE`, so an open `[ksu_driver]` fd pins the module and
[`delete_module(2)`][delete-module-2] fails outright; and ptctl takes `try_module_get()` for
the duration of a hardware-breakpoint hold.

## Building

### Kbuild: objects, version and the signature pins

[`Kbuild`](Kbuild) is the single source of truth for what gets compiled and with which
flags. It lists every object explicitly -- there is no wildcard, so a new `.c` file that
is not added to `kernelsu-objs` is never compiled. It also computes the version, pins
the manager APK signature, and sets the include paths that let SELinux-internal headers
and `uapi/` resolve.

The version is `30000 + git rev-list --count HEAD`, guarded by a check that KernelSU's git
root differs from the kernel's:

```make
KSU_GIT_VERSION := $(shell cd $(GIT_ROOT) && $(GIT) rev-list --count HEAD 2>/dev/null)
```

The guard exists so that a kernel tree which has vendored KernelSU into its own history
does not report the kernel's commit count -- a number in the millions -- as the KernelSU
version. The `git fetch --unshallow` just above it exists because CI and most build
scripts clone with `--depth 1`, and a shallow clone counts one commit. The same formula is
duplicated in [`userspace/ksud/build.rs`](../userspace/ksud/build.rs) and
[`manager/build.gradle.kts`](../manager/build.gradle.kts); all three must agree and must be
built from the same commit.

`KSU_EXPECTED_SIZE` and `KSU_EXPECTED_HASH` pin the manager's APK signing certificate by
byte length and SHA-256; an optional second pair accepts a second key, and Kbuild raises a
hard `$(error)` if a size2 is given without a hash2. That error buys a clear message rather
than a subtle bug: [`manager/apk_sign.c`](manager/apk_sign.c) guards its second check with
`#ifdef EXPECTED_SIZE2` and then passes `EXPECTED_HASH2` unconditionally, so the missing
half would otherwise surface as an undeclared-identifier error deep inside a signature
parser instead of one line naming the variable you forgot. See
[`manager/README.md`](manager/README.md) for what the kernel does with those constants.

### Kconfig, and the external-module build

[`Kconfig`](Kconfig) offers five symbols. `CONFIG_KSU` depends on `KPROBES` and `EXT4_FS`
-- the first because several subsystems place kprobes, the second because the ext4
sysfs-hiding helper calls `ext4_unregister_sysfs()`. `CONFIG_KSU_DEBUG` flips
`allow_shell` on by default, keeps `/sys/module/kernelsu` alive, and enables a debug-only
manager override. `CONFIG_KSU_DISABLE_MANAGER` drops three objects from the link and
redefines "manager" as "UID 0" through header-level inline stubs.
`CONFIG_KSU_DISABLE_POLICY` keeps the allowlist but removes per-app customization.
`CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER` is covered under
[x86_64 syscall hardening](#x86_64-syscall-hardening).

An external-module build (`make -C $KDIR M=...`) does not see this directory's Kconfig at
all -- the options arrive as make variables and never reach `autoconf.h`. Kbuild therefore
re-emits four of them as `-D` flags under `ifdef KBUILD_EXTMOD`. Without that block,
passing `CONFIG_KSU_DISABLE_MANAGER=y` on the command line would drop the three `manager/`
objects from `kernelsu-objs` -- the make conditional at the top of Kbuild reads the
variable directly -- while every `#ifdef CONFIG_KSU_DISABLE_MANAGER` in the headers stayed
false, so the link would go looking for the three files it had just removed.

### Locating this directory from an external build

The other thing an external build has to be told is where this directory actually is. Every
include in the tree is written from the module root -- `#include "policy/allowlist.h"`, not a
relative climb -- so Kbuild computes `KSU_KERNEL_DIR` and feeds it to two `-I` flags.
Arriving at that path takes three cases, and 6.18 added the third.

| Build | What names this directory | Kbuild expression |
| --- | --- | --- |
| In tree | `$(src)`, relative to the kernel source | `$(srctree)/$(src)` |
| Out of tree, up to 6.17 | `src=` overridden back to here, so `$(src)` is absolute | `$(src)` |
| Out of tree, 6.18 and newer | `$(srcroot)`, published alongside `MO=` | `$(srcroot)` |

Up to 6.17 there is no separate output-directory knob for modules: `M=` names the directory
kbuild both reads and writes, so building several KMIs from one checkout means pointing `M=`
at an output directory and overriding `src=` back to here, which makes `$(src)` absolute --
the `$(filter /%,$(src))` test is what distinguishes the first two rows. 6.18 introduced
`MO=` for the module output directory and publishes the module source root separately as
`$(srcroot)`, at which point neither of the older two cases yields this directory. The
`ifneq ($(KBUILD_EXTMOD),)` block takes `$(srcroot)` whenever the kernel offers one and
leaves the earlier logic as the fallback, so one Kbuild covers all three. The kernel's own
account of `M=`, `MO=` and `KBUILD_EXTMOD` is [Building External Modules][kbuild-modules].

### The Makefile and the build scripts

[`Makefile`](Makefile) is the LKM entry point. Its default target builds `check_symbol`
from [`tools/check_symbol.c`](tools/check_symbol.c) with the host compiler, invokes kbuild
out-of-tree, and then runs the checker against the result and the target `vmlinux`. Making
the checker a prerequisite rather than a separate step means a missing or stale binary
cannot silently skip validation. `make format` runs clang-format in place, and
`make check-format` is the `--dry-run --Werror` variant. CI runs that variant on pushes to
`main` and `dev` touching `kernel/**/*.{c,h}`, and on pull requests targeting either branch;
a push to a topic branch is checked by nothing, so run it locally first.

`make compdb` runs [`.vscode/generate_compdb.py`](.vscode/generate_compdb.py) over the
kbuild output directory to produce the `compile_commands.json` an editor needs to see the
same include paths and `-D` flags the real build uses. Copy
[`.clangd.example`](.clangd.example) to `.clangd` to point clangd at one of those
directories. That file is also where `UnusedIncludes: Strict` comes from, which is the
reason a header like `include/klog.h` carries an `// IWYU pragma: keep` comment at every
include site: its only job is redefining `pr_fmt`, no symbol from it is ever named, and
without the pragma clangd offers to delete it.

[`build-all.sh`](build-all.sh) loops the eight KMIs it lists -- or a list passed as its
first argument -- through `ddk build`, one output directory per KMI under `out/`, and
strips each resulting `kernelsu-<kmi>.ko`. `ODIR` is overridden from the command line,
which is what lets several KMIs be built from one checkout without colliding -- the
Makefile passes `src=` explicitly for the same reason, on every KMI but the newest.

[`build-all-x64.sh`](build-all-x64.sh) is the x86_64 counterpart, and it does not go
through `ddk` at all: it calls kbuild directly against a prepared `KDIR` per KMI and writes
to `out-x64/`. Two things it does are not decoration. It puts the exact `clang-r*` release
that KMI was built with at the front of `PATH` for the duration of that one build, and from
android16-6.12 onward the matching `rust-*` toolchain beside it, because a module compiled
by a different toolchain than the kernel it loads into can disagree about decisions the C
ABI never covers. The stack-protector variant described under
[The arm64 stack canary](#the-arm64-stack-canary) is one such disagreement, and the
workaround it forced still sits in [`core/init.c`](core/init.c). And it passes
`CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER=y` for every KMI, for the reason
[x86_64 syscall hardening](#x86_64-syscall-hardening) gives.

Both scripts now list the same eight KMIs, and the newest is the one that changes the
kbuild invocation. [`Makefile`](Makefile) derives `KMI` from the last path component of
`KDIR` and switches on it: android17-6.18 is built with `M=$(MDIR) MO=$(ODIR)`, every older
KMI with `M=$(ODIR) src=$(MDIR)`. `build-all-x64.sh` makes the same split for the kbuild
calls it issues itself.

Those x86_64 `KDIR`s come from
[`../scripts/prepare-ddk-x64.sh`](../scripts/prepare-ddk-x64.sh), which builds one at
`/opt/ddk/kdir-x64/<kmi>` out of each `/opt/ddk/src/<kmi>` source tree -- `gki_defconfig`
for x86_64, every LTO variant switched off, `modules_prepare`, and then
`security/selinux/built-in.a` so that the SELinux headers Kbuild reaches through
`-I$(objtree)/security/selinux` are generated in the x86 output tree at all. That output
path is exactly what [`build-all-x64.sh`](build-all-x64.sh) later hands to `make -C`, and it
is the handoff between the two scripts. android16-6.12 and android17-6.18, the two branches
with a Rust toolchain, additionally get `CONFIG_CFI_ICALL_NORMALIZE_INTEGERS`, the switch
that makes Clang hash integer types by width when it builds KCFI tags, which is the form
Rust and C agree on.

The script also applies four idempotent `sed` edits to `scripts/mod/modpost.c` in the target
source, before `modules_prepare` compiles that host tool. The load-bearing one comments out
the `check_exports(mod)` call, because modpost would otherwise object to every symbol this
module deliberately leaves undefined for `ksuinit` to bind. The other three comment out the
export-binding store beside it, mark `check_exports()` `__attribute__((unused))` so the
now-callerless function does not trip `-Wunused-function`, and give the generated
`__version_ext_names` entry an empty string literal. A `ddk` image's prebuilt aarch64 `KDIR`
already carries those fixes; the source archive that same image ships can predate them, so a
tree configured for x86_64 out of it needs all four reapplied. CI performs the same steps
inline in its own job.

## What check_symbol.c is guarding

The module calls kernel functions that are not exported to modules at all: `path_umount` and
`path_mount`, `find_task_by_vpid`, `ext4_unregister_sysfs`, `avc_ss_reset`, and most of the
SELinux security server. A normally-linked module would be refused at
`init_module` time with "Unknown symbol".

The fork sidesteps that in the loader rather than in the module.
[`ksuinit`](../userspace/ksuinit/README.md) parses `kernelsu.ko` with an ELF reader,
collects every symbol still marked [`SHN_UNDEF`][elf-5] (undefined), looks each name up in
`/proc/kallsyms`, rewrites the symbol table entry in place to `SHN_ABS` with the resolved
address, and only then calls `init_module`. That
bypasses the export table, the GPL check and the namespace check in one move -- and with
them, the checks that would normally have caught a bad symbol reference. `ksuinit` only
emits a `Cannot find symbol` warning for a name it cannot resolve and proceeds, so a
typo'd or since-renamed symbol turns into a failure on a user's device rather than a
failure on the build machine.

`check_symbol.c` restores the guardrail from the other end. It mmaps both ELF files,
requires the `.ko` to have a `__versions` section of size zero (the signature of "modpost
matched nothing; every symbol will be bound from kallsyms at load"), and then requires
every `SHN_UNDEF` named symbol in the module to be present and defined in the target
`vmlinux` symbol table -- which is precisely the table `/proc/kallsyms` will present at
load time. A symbol found but bound neither `STB_GLOBAL` nor `STB_WEAK` produces a warning
rather than an error. An unresolved symbol here means the `.ko` would reach a device with
that entry still `SHN_UNDEF`: `ksuinit` logs `Cannot find symbol`, calls `init_module`
anyway, and the kernel's own resolver -- the thing the relocation pass was meant to skip
past -- finds nothing in the export table and fails the load with "Unknown symbol". The
phone boots without KernelSU instead of the build stopping.

## Version and architecture support, as the code expresses it

### Two architectures, enforced in three places

Architecture support is exactly two entries, and it is enforced in three places rather
than declared anywhere. [`Kbuild`](Kbuild) selects `hook/arm64/` under `CONFIG_ARM64` and
`hook/x86_64/` under `CONFIG_X86_64` with no third branch, so on any other target
`kernelsu-objs` is missing the memory-patching and syscall-hooking implementations.
[`include/arch.h`](include/arch.h) ends its register-mapping cascade with
`#error "Unsupported arch"`. [`hook/patch_memory.h`](hook/patch_memory.h) does the same.

### The KMI matrix, and where the lists disagree

Kernel-version support is expressed as a KMI matrix, and the lists in the tree that carry
one agree again. CI's matrix, [`build-all.sh`](build-all.sh) and
[`build-all-x64.sh`](build-all-x64.sh) all run the same eight:
`android12-5.10`, `android13-5.10`, `android13-5.15`, `android14-5.15`, `android14-6.1`,
`android15-6.6`, `android16-6.12`, `android17-6.18`. CI builds every KMI twice, once for aarch64 through this
directory's Makefile and once for x86_64 against a `KDIR` it configures from source in the
same job. Only the aarch64 leg runs `check_symbol`, and not by choice: the x86_64 tree is
taken as far as `modules_prepare` and never links a `vmlinux`, so the symbol table the
checker compares against does not exist there.

### What pins an artifact to a KMI, and what does not

Two of the things that would normally pin a module to one kernel do not pin these. Symbol
addresses do not, because `ksuinit` resolves them from `/proc/kallsyms` on the device.
Vermagic no longer does either: `ksuinit` retries a rejected `init_module(2)` by reading
the string the kernel said it wanted back out of `/dev/kmsg`, rewriting the `vermagic=`
entry in the module's `.modinfo` and loading again. That answers a string comparison, not
an incompatibility, so it widens which kernels will accept a given artifact without
widening which kernels it is correct on. What still ties an artifact to a single KMI is
everything the compiler baked in -- struct layouts from that KMI's headers, the
`LINUX_VERSION_CODE` branches below -- which is why one artifact per KMI is produced rather
than one universal module.

Within that range the source adapts with `LINUX_VERSION_CODE` guards wherever a kernel API
moved -- the LSM hook lists became static calls at 6.12, the arm64 text-patching header
moved twice, `close_fd` replaced `ksys_close` at 5.11. Not every difference is a version,
though: `KSU_NEW_DCACHE_FLUSH` in Kbuild greps the target's
`arch/arm64/include/asm/cacheflush.h` for `__flush_dcache_area` and records the grep's
*exit status*, because that API change landed in 5.14 upstream but was backported to
`android13-5.10` and not to `android12-5.10`. Both branches report 5.10, so no
`LINUX_VERSION_CODE` comparison can separate them; the header is the only evidence there
is.

### x86_64 syscall hardening

On x86_64, upstream commit `1e3ad78334a6` replaced the indirect call through
`sys_call_table` with a chain of direct branches, and it was backported into nearly every
GKI branch. Under that hardening, writing into the syscall table has no effect at all,
because the kernel never reads the slot. The default build therefore refuses to proceed:
`core/init.c` raises a compile-time `#error` when `X86_FEATURE_INDIRECT_SAFE` is
undefined, and aborts `kernelsu_init` with `-ENOSYS` and a nine-line `pr_alert` banner
when the feature bit is absent at runtime. Loading anyway would leave the module believing
its hooks were live when they were not.

The escape hatch is `CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER`, which compiles out both
checks and instead inline-patches the hardened dispatcher back into an indirect call at load
time. This is the configuration CI uses for its x86_64 artifacts.

On kernels older than 5.16 the same block also tries to patch `do_syscall_64`, because those
kernels predate the change that made the syscall entry path re-read the syscall number after
the tracepoint fires -- the mechanism the whole dispatcher design depends on. That second
patch is attempted, not guaranteed: the replacement calls `syscall_enter_from_user_mode` and
`syscall_exit_to_user_mode` itself, so it is applied only when both resolve from kallsyms. If
either does not, `x64_sys_call` is still patched, `do_syscall_64` is left alone, and the only
record of the difference is the `pr_info` naming the two addresses.

### The arm64 stack canary

One last arch-specific oddity lives in [`core/init.c`](core/init.c). On arm64 LKM builds
with a global stack canary, the module defines its own `__stack_chk_guard` and initializes
it from a naked assembly trampoline:

```c
__attribute__((naked)) int __init kernelsu_init_early(void)
{
    asm("mov x19, x30;\n"
        "bl ksu_setup_stack_chk_guard;\n"
        "mov x30, x19;\n"
        "b kernelsu_init;\n");
}
```

The module ships its own canary because some third-party kernels are built with a
toolchain that supports `-mstack-protector-guard=sysreg` and therefore stops providing the
global symbol that the GKI-toolchain-built module references. The trampoline is naked
rather than a plain C wrapper because a wrapper's prologue would spill the still-zero
canary onto its stack, the helper would then change the global, and the wrapper's epilogue
would compare the two, mismatch, and panic through `__stack_chk_fail()`. A function with
no prologue has nothing to mismatch.

## Rules a maintainer has to respect

**Resolve symbols, do not expect exports.** Anything the module needs that the kernel does
not export goes through `find_kernel_symbol_exact()` or, for a pointer that must compare
equal to a value already stored in a kernel function table,
`ksu_resolve_symbol_for_functable_hook()`. The two are not interchangeable: on kernels
before 6.1 the latter can return a Clang-CFI jump-table thunk, which is the right value to
compare against a table entry and the wrong value to call. Both live in
[`infra/symbol_resolver.c`](infra/symbol_resolver.c).

**Pair every registration with its retirement.** A consumer that resolves a `register_*`
primitive must also resolve the matching `unregister_*` and refuse to arm anything if either
is missing. `uhook` computes a single `uhook_ready` gate from the whole set; `ptctl` refuses
`hwbp_set()` with `-ENOSYS` when either half of the hardware-breakpoint pair is absent;
`mem_spoof` rolls back every already-registered kretprobe if any one of three fails. Arming
something that cannot be retired is a use-after-free across `rmmod`, and half a feature is
frequently worse than none -- a `MemTotal` that moved while `MemAvailable` did not is a
self-inconsistency a detector can read straight out of `/proc/meminfo`.

**Retire each hook in the teardown phase that matches it.** Anything registered in
`kernelsu_init` that leaves a pointer into module text has to be removed in `kernelsu_exit`,
in the order [`core/init.c`](core/init.c) already establishes; text is freed on `rmmod`
whether or not something still points into it. Which of the two phases a removal belongs in
depends on what can still reach the pointer.

- **Phase one, before the module-scope `synchronize_rcu()`:** a uprobe consumer, a kprobe, a
  perf breakpoint, a patched syscall-table slot. Any CPU can reach one of these at any
  moment, and nothing else in the teardown waits out a callback already running.
- **Phase two, after it:** the two rodata function tables `kernelsu_exit` restores there --
  selinux_hide's `selinux_write_op[]` entries and the LSM hook slots. Both paths end in
  `ksu_lsm_unhook()`, which issues a `synchronize_rcu()` of its own after putting a slot
  back, so a barrier does follow those stores; the one between the phases is not what covers
  them. Note also that `ksu_selinux_hide_exit()` unhooks only when the feature was running,
  while `hook_selinux_status_open()` patches `sel_handle_status_ops.open` at init
  unconditionally.

**Keep the allowlist's RCU pairing intact.** Every reader of the allowlist runs inside
`rcu_read_lock()`, every writer holds `allowlist_mutex` and frees through `kfree_rcu()`, and
no change may break that pairing. A reader takes no lock at all, so an entry it is walking
must survive until the next grace period even though the writer already unlinked it;
`kfree_rcu()` is what defers the free that long. The `synchronize_rcu()` between the two
teardown phases is the module-scope version of the same rule.

**Write to read-only kernel data through one primitive.** `ksu_patch_text()` is the only
sanctioned way to modify rodata -- a syscall table entry, an LSM hook slot, a procfs
`file_operations` field. It walks `init_mm`'s page tables, maps the physical page through
the fixmap, and stores inside `stop_machine()`. Flipping the PTE writable instead is what
vendor higher-EL monitors detect, and `stop_machine` is what makes a non-atomic multi-byte
store safe against a CPU fetching a half-written pointer. Details are in
[`hook/README.md`](hook/README.md).

**Define the ABI once, in `uapi/`.** Every ioctl number, every command struct and
`KERNEL_SU_UAPI_VERSION` are defined once, in [`../uapi/supercall.h`](../uapi/supercall.h)
and its siblings. The kernel reaches them through the `include/uapi` symlink, `ksud` through
bindgen, the manager app through a second symlink. Most ioctl numbers deliberately encode a
size of zero, so a stale caller is *not* rejected by the dispatcher -- which makes
`KERNEL_SU_UAPI_VERSION` the only compatibility mechanism there is. Bump it whenever a
struct changes shape. See [`../uapi/README.md`](../uapi/README.md).

Feature ids are wire values too, and they are the ones a rebase is most likely to damage.
`enum ksu_feature_id` in [`../uapi/feature.h`](../uapi/feature.h) indexes the kernel's
`feature_handlers[]` array, is mirrored by `ksud`'s `FeatureId` enum, and is what
`/data/adb/ksu/.feature_config` stores on disk, so an id that shifts does not fail loudly
-- it re-points a setting the user already saved at a different switch. Upstream allocates
from 0 upwards and once took 5 for `KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT`, which is why this
fork's `KSU_FEATURE_MOUNT_HIDE` sits at 16 and why the header reserves 16 and above for
fork-local features: the next upstream feature lands in the gap below rather than on top of
one of ours. Upstream has since retired that feature, so 5 is unclaimed again -- unclaimed
by upstream, which is not the same as available to the fork. Adding a feature on either side of that line means touching the enum, the
handler that registers against it, and every arm of `ksud`'s `from_u32`, `name`,
`description`, `parse_feature_id`, `list_features` and `save_config`.

**Keep the formatting mechanical.** [`.clang-format`](.clang-format) sets `ColumnLimit: 120`,
`IndentWidth: 4`, `UseTab: Never` and `SortIncludes: false`.
[`.github/workflows/clang-format.yml`](../.github/workflows/clang-format.yml) runs
`make check-format` on pushes to `main` and `dev` and on pull requests targeting either
branch, the pull-request trigger also watching `.clang-format` itself; a topic branch is
checked by nothing but you. Both [`Kbuild`](Kbuild) and [`Makefile`](Makefile) end with the
line
`# Keep a new line here!! Because someone may append config`; leave it, and the newline
after it, in place, so that an integrator appending a `ccflags-y` line lands on a line of
its own rather than splicing onto the last real statement.

## See also

- [`../docs/architecture.md`](../docs/architecture.md) -- repository-wide hub: the layers,
  the end-to-end flows, where to start reading
- [`core/README.md`](core/README.md) -- module entry and exit in detail, and the build
  configuration knobs
- [`../uapi/README.md`](../uapi/README.md) -- the kernel/userspace ABI contract
- [`../userspace/ksuinit/README.md`](../userspace/ksuinit/README.md) -- the ramdisk init
  shim that relocates this module's undefined symbols and loads it
- [`../userspace/ksud/README.md`](../userspace/ksud/README.md) -- the daemon on the other
  end of the driver fd
- [`../scripts/README.md`](../scripts/README.md) -- repository build and packaging
  automation
- [`../docs/instrumentation.md`](../docs/instrumentation.md) -- driving ptctl and uhook
  from a root-owned tool, and what each of them leaves visible to the target

<!-- reference links: kernel documentation and man pages -->
[delete-module-2]: https://man7.org/linux/man-pages/man2/delete_module.2.html
[elf-5]: https://man7.org/linux/man-pages/man5/elf.5.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
[kbuild-modules]: https://docs.kernel.org/kbuild/modules.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[lsm]: https://docs.kernel.org/security/lsm.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
[uprobetracer]: https://docs.kernel.org/trace/uprobetracer.html
[whatisrcu]: https://docs.kernel.org/RCU/whatisRCU.html
