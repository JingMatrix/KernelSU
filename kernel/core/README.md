# Module entry and exit

`kernel/core/` holds exactly one translation unit, [`init.c`](init.c), the only file in
the tree that owns `module_init` and `module_exit`. Everything else under `kernel/` is a
subsystem this file starts in a particular order and stops in roughly the reverse one.
Four globals live here, declared in [`include/ksu.h`](../include/ksu.h) so every
subsystem can reach them: `ksu_cred`, a privilege template; `ksu_late_loaded`, the
boot-mode flag; and the module parameters `allow_shell` and `ksu_no_custom_rc`.

## Four load situations, two code paths

Built into vmlinux (`CONFIG_KSU=y`) means `MODULE` is undefined: `module_init` degrades
to an initcall, `ksu_late_loaded` is a compile-time `false`, and `kernelsu_exit()` is
`__exit` so the linker discards it - no teardown reasoning below applies to that build.
The other three situations are all LKM loads, separated by one line:

```c
#ifdef MODULE
    ksu_late_loaded = (current->pid != 1);
#else
    ksu_late_loaded = false;
#endif
```

PID 1 is the loader when the module comes from the ramdisk, because
[`userspace/ksuinit`](../../userspace/ksuinit/src/init.rs) replaces `/init` and asserts
`getpid().is_init()` before calling [`init_module`][init-module-2]; a late load is `ksud
late-load` ([`late_load.rs`](../../userspace/ksud/src/late_load.rs)) calling it from an
ordinary daemonized process.

Upstream added a third LKM entry point, and it reaches PID 1 without replacing `/init`
at all. `ksud boot-patch-v2` ([`lkm_image.rs`](../../userspace/ksud/src/lkm_image.rs))
leaves the ramdisk alone and edits the arm64 kernel Image: the module is appended past
`_end`, the `memblock_reserve()` that `arm64_memblock_init()` makes over the kernel's
own range is widened so those appended bytes are never handed to the page allocator, and
the `async_synchronize_full()` call inside `kernel_init()` is redirected into a small
stub in [`lkm_image_bootstrap.S`](../../userspace/ksud/src/lkm_image_bootstrap.S) that
copies the module out and calls `load_module()` on the copy. `kernel_init()` is what PID
1 runs before it execs `/init`, so `current->pid != 1` is false and `kernelsu_init()`
takes the boot path, exactly as it would under ksuinit.

The flag outlives init: it makes `ksu_is_safe_mode()` in
[`ksud_integration.c`](../runtime/ksud_integration.c) return false, because the
volume-down samples it counts were never collected, and it makes
[`dispatch.c`](../supercall/dispatch.c) swallow `EVENT_POST_FS_DATA` and
`EVENT_BOOT_COMPLETED` and raise `KSU_GET_INFO_FLAG_LATE_LOAD` in the get-info reply the
manager reads.

## The x86_64 abort comes first

The first statement of `kernelsu_init()` on x86_64 is a CPU feature test, and an
`#ifndef X86_FEATURE_INDIRECT_SAFE` guard above it refuses to compile at all when the
macro is missing ("FATAL: Your kernel is missing the indirect syscall bypass patches!").

Upstream hardened the x86 syscall entry path by replacing the indirect call through
`sys_call_table[nr]` with a generated chain of direct branches. KernelSU's dispatcher (see
[`hook/README.md`](../hook/README.md)) needs the kernel to read that table, so on a hardened
kernel the entry path never reaches the slot KernelSU patched, while
`ksu_sys_enter_handler()` in [`syscall_hook_manager.c`](../hook/syscall_hook_manager.c)
still rewrites `orig_ax` to the dispatcher slot: the hook does not run, and neither does the
syscall userspace asked for. `boot_cpu_has(X86_FEATURE_INDIRECT_SAFE)` confirms at run time
that the indirection is back; when it is not, `kernelsu_init()` prints a banner naming
kernel panic as the thing the abort avoids and returns `-ENOSYS`, which makes
[`init_module(2)`][init-module-2] fail and frees the module without running the exit path -
hence its position before every allocation and probe.
`CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER=y` drops both checks and has
[`x86_64/syscall_hook.c`](../hook/x86_64/syscall_hook.c) patch `x64_sys_call` instead.

That option is what the x86_64 module builds actually use.
[`build-all-x64.sh`](../build-all-x64.sh) passes it for every KMI it walks, android12-5.10
through android17-6.18, because the abort above assumes a kernel whose source the user
can edit, and the x86_64 audience - emulators, Android-on-x86, virtual devices - runs
stock kernels it cannot. Patching `x64_sys_call` at run time reaches the same dispatcher
from inside the module, so the LKM works on an unmodified image.

## Why the entry point is a naked function on arm64

A stack protector places a per-boot random canary between a function's locals and its
saved return address and compares it on the way out; a mismatch panics. On arm64 that
canary normally lives in the exported global `__stack_chk_guard`, but commit `baf3f7d4`
records kernels built with a clang new enough to accept
`-mstack-protector-guard=sysreg`, which sets `CC_HAVE_STACKPROTECTOR_SYSREG` and stops
exporting the symbol the GKI-built LKM still references. So `init.c` defines a hidden
copy plus a `no_stack_protector` initializer mirroring `boot_init_stack_canary()`, under
a guard that excludes `CONFIG_STACKPROTECTOR_PER_TASK` (where the canary lives in
`task_struct`). That leaves the question of who writes the guard before the first
protected function runs:

```c
__attribute__((naked)) int __init kernelsu_init_early(void)
{
    asm("mov x19, x30;\n"
        "bl ksu_setup_stack_chk_guard;\n"
        "mov x30, x19;\n"
        "b kernelsu_init;\n");
}
```

A plain C wrapper cannot do this: its prologue would read the still-zero guard onto its
stack, the helper would change the global, and the epilogue would compare the stale
value against the new one, mismatch, and panic. A naked function emits no prologue or
epilogue, so there is nothing to mismatch. The `x19` shuffle exists because `bl`
clobbers `x30`; `x19` is itself callee-saved under AAPCS64 and is not restored here.

## ksu_cred, allocated before any subsystem

`prepare_creds()` returns a fresh, uncommitted copy of the current task's credentials.
`kernelsu_init()` keeps one such copy forever in `ksu_cred` and never commits it: it is
a template, not an identity. Seven call sites across five files wrap a privileged
operation in `override_creds(ksu_cred)` so it runs with KernelSU's credentials rather
than the caller's: writing the allowlist in [`allowlist.c`](../policy/allowlist.c),
three sites around `/data/adb/ksud` in [`sucompat.c`](../feature/sucompat.c), the
module rc file in [`ksud_integration.c`](../runtime/ksud_integration.c), `path_umount()`
in [`kernel_umount.c`](../feature/kernel_umount.c), and joining PID 1's mount namespace
in [`su_mount_ns.c`](../infra/su_mount_ns.c). They all run from a hook, on someone
else's thread, so a NULL `ksu_cred` would oops far from the cause; its allocation is one
of only two failures that abort init. `setup_ksu_cred()` in
[`selinux.c`](../selinux/selinux.c) later moves the template into `u:r:ksu:s0`.

## The bring-up sequence

The same calls run before the mode branch on either path, and only one of them already
looks at the flag:

1. `ksu_init_symbol_resolver()` ([`symbol_resolver.c`](../infra/symbol_resolver.c))
   resolves two kallsyms iterators by address: `kallsyms_on_each_symbol` below 5.19,
   where a module cannot call it directly, and `kallsyms_on_each_match_symbol` from 6.1
   on. `find_kernel_symbol_exact()` uses the match iterator when it has one and
   `kallsyms_lookup_name()` otherwise; the full-walk iterator serves
   `resolve_symbol_variant()`, which has to see every name because the symbol it wants
   may have been renamed with a `.` or `$` suffix.
2. `ksu_syscall_hook_init()` resolves `sys_call_table`, claims an unused
   `sys_ni_syscall` slot, patches `ksu_syscall_dispatcher` in and publishes
   `ksu_dispatcher_nr`.
3. `ksu_feature_init()` ([`feature.c`](../policy/feature.c)) NULLs all
   `KSU_FEATURE_MAX` slots of `feature_handlers[]`. That bound is not a count of
   features. A feature id is a wire value - the manager names it in the ioctl and ksud
   writes it into `.feature_config` - so reassigning one silently repoints a saved
   setting at a different feature. Upstream allocates ids upward from 0 and has reached
   5 with `KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT`, so
   [`uapi/feature.h`](../include/uapi/feature.h) parks this fork's own ids at 16 and
   above, clear of the next upstream allocation. `KSU_FEATURE_MOUNT_HIDE` is 16 and
   `KSU_FEATURE_MAX` is therefore 17, leaving ids 6 through 15 as permanent NULL holes.
   The waste is a few pointers, and lookup stays an array index rather than a search.
4. `ksu_sulog_init()` ([`sulog.c`](../feature/sulog.c)), `ksu_adb_root_init()`
   ([`adb_root.c`](../feature/adb_root.c)) and `ksu_selinux_hide_init()`
   ([`selinux_hide.c`](../feature/selinux_hide.c)) register feature handlers; the last
   also patches the selinuxfs status-open slot, and is the one call before the branch
   that already reads `ksu_late_loaded`: on a late load it builds the fake status page
   immediately; otherwise it arms a [static key][static-keys], so that the first real open of the
   selinuxfs status page captures one if nothing else has, while the exec of
   `init second_stage` builds it outright and post-fs-data disarms the key either way.
   `ksu_lsm_hook_init()` ([`lsm_hook.c`](../hook/lsm_hook.c)) is called between the
   second and the third, and it registers nothing and patches nothing - its whole body
   logs how many [LSM][lsm] slots are tracked, which is zero at this point, because each slot
   is patched later by the feature that wants it, as `ksu_selinux_hide_enable()` does
   for `selinux_setprocattr` when selinux_hide is switched on.
5. `ksu_supercalls_init()` ([`supercall.c`](../supercall/supercall.c)) registers the
   [kprobe][kprobes] that hands out the `[ksu_driver]` fd on a magic [`reboot(2)`][reboot-2]
   argument pair.
6. `ksu_app_profile_init()` ([`app_profile.c`](../policy/app_profile.c)) registers
   nothing and patches nothing; on a kernel from 6.6 up to 6.11 it fingerprints one
   kernel function, and everywhere else `NEED_BACKPORT_COMPAT` compiles its body away
   and the call costs nothing. Dropping the caller's [seccomp][seccomp-filter] filter during
   `escape_with_root_profile()` means handing `seccomp_filter_release()` a throwaway
   copy of `current`, and that function refuses to act on a task it does not consider
   already detached. Which mark it looks for moved: from 5.11 it wants `->sighand` NULL,
   from 6.11 it wants `PF_EXITING`, and the change was backported into some 6.6 Android
   kernels but not others, so a single version test cannot decide it - which is why the
   fingerprint is compiled in for exactly that window and nowhere else. The backported
   form takes the siglock and the older one does not, which makes the presence of a call
   to `_raw_spin_lock_irq` inside `seccomp_filter_release` the fingerprint.
   `scan_call_to()`
   ([`arm64/patch_memory.c`](../hook/arm64/patch_memory.c)) walks the function looking
   for a `BL` whose computed target is that address, bounded by the length
   `kallsyms_lookup_size_offset()` reports and by 128 bytes when it reports none. Get
   this wrong and the release trips the kernel's own `WARN_ON()` instead of freeing the
   filter, which is why the answer is settled once at init rather than guessed per
   escalation. Only arm64 implements the scan; the x86_64 `scan_call_to()` returns NULL,
   so that build always takes the `->sighand` branch in the ambiguous window.
7. `ksu_ptctl_init()` ([`ptctl.c`](../feature/ptctl.c)) and `ksu_uhook_init()`
   ([`uhook.c`](../feature/uhook.c)) resolve kallsyms targets and allocate the uhook
   ring; neither installs a probe here.

Two of those orderings are load-bearing. `ksu_syscall_hook_init()` must precede
everything that calls `ksu_register_syscall_hook()` or `ksu_syscall_table_hook()`,
because it opens with `memset(syscall_hooks, 0, sizeof(syscall_hooks))`, which erases an
earlier registration, and because `ksu_syscall_table` is NULL until it resolves it, so
`ksu_syscall_table_hook()` returns at its first check without patching.
`ksu_feature_init()` must precede every `ksu_register_feature_handler()` call, because
it clears the array: a handler registered earlier is discarded without a word and its
feature reports unsupported for the rest of the boot. There is no unwind path either -
past the two aborts `kernelsu_init()` always returns 0 and most subsystem inits return
void, so a failed lookup yields a module that loads and has one feature quietly inert.

Objects that appear on neither the list above nor the teardown one below are normally pure
helpers - [`tp_marker.c`](../hook/tp_marker.c),
[`seccomp_cache.c`](../infra/seccomp_cache.c) and [`event_queue.c`](../infra/event_queue.c)
register nothing with the kernel, so there is nothing to start or stop. One object is not
like that. [`mem_spoof.c`](../feature/mem_spoof.c) has no init and no exit at all:
`ksu_set_spoof_mem()` resolves `register_kretprobe` through `find_kernel_symbol_exact()` and
arms [kretprobes][kprobes] on `si_meminfo`, `si_mem_available` and `vm_commit_limit` the
first time the manager asks for a spoofed RAM size, then unregisters them when it asks for
zero. The request arrives on the supercall dispatcher, so there is nothing for
`kernelsu_init()` to start - and, by the same token, nothing in `kernelsu_exit()` that takes
those three kretprobes down for a manager that never switched the feature back off.

## The late-load branch

When `ksu_late_loaded` is set, `kernelsu_init()` performs synchronously what the boot
path would otherwise receive as events:

```c
        apply_kernelsu_rules();
        cache_sid();
        setup_ksu_cred();

        // Grant current process (ksud late-load) root
        // with KSU SELinux domain before enforcing SELinux, so it
        // can continue to access /data/app etc. after enforcement.
        escape_to_root_for_init();
```

`apply_kernelsu_rules()` ([`rules.c`](../selinux/rules.c)) duplicates the live policydb,
injects the `ksu` domain and the `ksu_file` type, and RCU-swaps the result into
`selinux_state.policy`. It comes first because the next two calls resolve `u:r:ksu:s0` to a
SID and that type does not exist until the policy is patched. `escape_to_root_for_init()`
([`app_profile.c`](../policy/app_profile.c)) moves the loading process into that domain, and
the comment explains its position relative to the `setenforce(true)` ending the branch: a
late load usually happens while [SELinux][selinux] is permissive, and if enforcement resumed
with ksud still in `u:r:su:s0` it would lose the access to `/data/app` the manager scan
needs. The branch then loads the allowlist, brings up the hook manager, starts the throne
tracker and the `/data/system` fsnotify watch
([`pkg_observer.c`](../manager/pkg_observer.c)), initializes the file wrapper, and sets
`ksu_boot_completed = true` before `track_throne(false)`, because `ksu_prune_allowlist()`
refuses to prune while that flag is false.

## The built-in branch

On a normal boot Android does not exist yet, so the other branch is five calls:
`ksu_syscall_hook_manager_init()`, `ksu_allowlist_init()`, `ksu_throne_tracker_init()`
([`throne_tracker.c`](../manager/throne_tracker.c)), `ksu_ksud_init()` and
`ksu_file_wrapper_init()` ([`file_wrapper.c`](../infra/file_wrapper.c)).

`ksu_ksud_init()` is the piece the late-load branch omits: it patches `__NR_read` and
`__NR_fstat` in the syscall table so KernelSU can splice its own stanzas into `init.rc`
as init reads it, and registers the `input_event` kprobe that counts volume-down presses
for safe mode. The rest is deferred - `apply_kernelsu_rules()`, `cache_sid()` and
`setup_ksu_cred()` fire from `ksu_handle_execveat_ksud()` on the exec of
`/system/bin/init second_stage`, and `ksu_load_allow_list()` and `ksu_observer_init()`
from `on_post_fs_data()` in [`boot_event.c`](../runtime/boot_event.c) on the first
zygote.

Most user-visible features come up one level down, in `ksu_syscall_hook_manager_init()`
([`syscall_hook_manager.c`](../hook/syscall_hook_manager.c)). It opens with two kretprobes,
on `syscall_regfunc` and `syscall_unregfunc` - the pair the kernel calls when a syscall
[tracepoint][tracepoints] gains or loses its first user. Their handlers keep a registration
count and adjust which tasks carry the tracepoint flag: while KernelSU is the only user only
its own processes are marked, and the moment a second user registers, every task is marked
so the other subscriber sees the syscalls KernelSU would otherwise have left untraced. After
the kretprobes come the five dispatcher hooks - `__NR_setresuid`, `__NR_execve`,
`__NR_execveat`, `__NR_newfstatat` and `__NR_faccessat` - then the `sys_enter` tracepoint,
then `ksu_setuid_hook_init()` ([`setuid_hook.c`](../hook/setuid_hook.c)), whose whole body
is `ksu_kernel_umount_init()` and `ksu_mount_hide_init()`
([`mount_hide.c`](../feature/mount_hide.c)), and last `ksu_sucompat_init()`.

`__NR_execveat` is the newest of the five, and a libc change forced it. Bionic from Android
17 QPR2 compiles both [`execve()`][execve-2] and `execv()` down to `execveat(AT_FDCWD, path,
argv, envp, 0)`, so a build that watched only `__NR_execve` would stop seeing process
creation on those devices: no `init second_stage` trigger, no su shim, no adbd `LD_PRELOAD`
injection. `ksu_hook_execveat()` in
[`syscall_event_bridge.c`](../hook/syscall_event_bridge.c) shares one body with
`ksu_hook_execve()` and differs only in which registers hold the filename and argv, since
[`execveat`][execveat-2] pushes both one position along to make room for the dirfd. The
sucompat path guards itself further and declines any call whose dirfd is not `AT_FDCWD` or
whose flags are non-zero, because the rewrite it performs assumes the plain
[`execve`][execve-2] argument shape.

`ksu_kernel_umount_init()` registers two feature handlers rather than one: `kernel_umount`
and `webview_zygote_umount`. The second decides whether the webview zygote,
`WEBVIEW_ZYGOTE_UID` 1053, has KernelSU's mounts taken away at its
[`setresuid`][setresuid-2], which every isolated process it later forks then inherits; it
starts false and waits for the manager to turn it on. `ksu_mount_hide_init()` beside it is a
different mechanism, not a second copy of the same one: it unmounts nothing and instead
filters what `/proc/<pid>/mountinfo`, `/proc/<pid>/mounts` and `/proc/<pid>/mountstats`
render. `ksu_should_hide_mount_for_current()` fixes the reader's identity at `open()`: every
isolated process is filtered, and so is an app uid that `ksu_uid_should_umount()` already
answers yes for, which leaves the manager, su-granted apps and the webview zygote reading
the real view. Because the feature defaults on, it patches the three `.open` slots during
this call instead of waiting for a toggle. One changes what is mounted, the other changes
what is printed, and a build carries both.

Both settle the question of who counts as isolated the same way, through
`is_isolated_process()` in [`allowlist.h`](../policy/allowlist.h), which reduces a uid
to an appid with `uid % PER_USER_RANGE` and accepts either [99000, 99999], the classic
isolated range, or [90000, 98999], where the children of an app zygote land. A Chrome
sandboxed renderer is in the second range, so a predicate covering only the first would
leave it with KernelSU's mounts both present and printed - and the two mechanisms would
disagree about it, which is worse than either being off.

## Module parameters and the sysfs hide

`allow_shell` defaults to true under `CONFIG_KSU_DEBUG` and false otherwise. When set,
[`allowlist.c`](../policy/allowlist.c) treats `SHELL_UID` as allow-listed in
`__ksu_is_allow_uid()` and gives it the default full-root profile in
`ksu_get_root_profile()`. `ksu_no_custom_rc`, exposed under the shorter name `norc`,
makes `load_module_rc_once()` skip reading `modules.rc` from `/metadata`, so a boot loop
caused by a module's init stanzas can be broken without reflashing. Both arrive in the
one space-separated string `init_module(2)` takes, which `ksud boot-patch` keeps as the
ramdisk `ksu_config` entry ([`boot_patch.rs`](../../userspace/ksud/src/boot_patch.rs))
for ksuinit to pass through. The image-injected path cannot carry them: its bootstrap
hands `load_module()` a pointer to an empty string, because PID 1 that early has no
userspace mapping for `strndup_user()` to copy from, so both parameters keep their
compile-time defaults there. Both use permission `0`, which tells `moduleparam.h` to
create no sysfs attribute at all - and that is what makes the last statement of
`kernelsu_init()` free, a `kobject_del(&THIS_MODULE->mkobj.kobj)` under `#ifdef MODULE`
and `#ifndef CONFIG_KSU_DEBUG`.

Every loaded module gets a kobject that materializes as `/sys/module/<name>/`. Deleting
it removes that directory, so a detector scanning sysfs for `kernelsu` finds nothing,
and no parameter file is lost because there were none. This hides one directory and no
more: the module is still listed in `/proc/modules` and by `lsmod`. The debug guard
exists for the one parameter that does need a file -
[`apk_sign.c`](../manager/apk_sign.c) exposes `ksu_debug_manager_appid` at mode
`S_IRUSR|S_IWUSR` under that config, and `ksud debug set-manager`
([`debug.rs`](../../userspace/ksud/src/debug.rs)) writes it through
`/sys/module/kernelsu/parameters/`.

## kernelsu_exit and the two phases

Tracepoint callbacks, kprobe handlers, [uprobe][uprobetracer] consumers and patched
syscall-table entries are all raw function pointers into the module's `.text`. If
`module_free()` runs while one is still reachable, the next hit executes freed memory. Phase
1 removes every source of new callbacks, `synchronize_rcu()` waits out readers already
inside one, and Phase 2 frees what they were touching.

Phase 1 starts with `ksu_syscall_hook_manager_exit()`, which unregisters the `sys_enter`
tracepoint and calls `tracepoint_synchronize_unregister()` before touching anything
else, destroys the two kretprobes, unregisters all five dispatcher hooks, and only then
calls `ksu_syscall_hook_exit()`. That function has its own rule, stated in a comment in
[`arm64/syscall_hook.c`](../hook/arm64/syscall_hook.c): it restores every entry recorded
in `hooked_entries[]` while `syscall_hooks[]` and `ksu_dispatcher_nr` are still valid,
clearing them only afterwards, so an in-flight syscall sees either the original handler
or a consistent dispatcher, never a half-torn one. `ksu_syscall_hook_manager_exit()` then
finishes with `ksu_sucompat_exit()` and `ksu_setuid_hook_exit()`, and the second unwinds
what `ksu_setuid_hook_init()` set up on the way in: `ksu_kernel_umount_exit()` drops both
umount feature handlers, and `ksu_mount_hide_exit()` writes the original `.open` back
into `proc_mounts_operations`, `proc_mountinfo_operations` and
`proc_mountstats_operations`, then issues a `synchronize_rcu()` of its own to drain the
shims that entered `.open` just before the restore. Across that drain it deliberately
keeps the saved original pointers non-NULL, because a `seq_file` opened while the slots
were patched still carries a `show` that calls through them, and clearing them would
hand such a reader a call to NULL.

Back in `kernelsu_exit()`, `ksu_uhook_exit()` and `ksu_ptctl_exit()` come next, for the
reason `init.c` states: uprobe consumers, ptctl's killguard kprobe and armed hardware
breakpoints all point at module text. `ksu_supercalls_exit()` unregisters the reboot
kprobe and frees the umount list, and `ksu_ksud_exit()` runs under
`if (!ksu_late_loaded)`, matching the branch that called `ksu_ksud_init()`.

Phase 2 releases state in reverse init order: package observer, throne tracker,
allowlist hash table, selinux_hide, LSM hook registry, adb_root, sulog, feature
registry, and finally `put_cred(ksu_cred)`. Ordering is not the only defence:
`anon_ksu_fops.owner` in [`supercall.c`](../supercall/supercall.c) and
`ksu_sulog_fops.owner` in [`sulog/fd.c`](../sulog/fd.c) are both `THIS_MODULE`, so an open
`[ksu_driver]` or `[ksu_sulog]` fd pins the module and `delete_module` fails rather than
racing, and ptctl takes a `try_module_get()` for a hardware-breakpoint hold.

## Build configuration

[`Kbuild`](../Kbuild) lists every object by hand and re-emits the `CONFIG_KSU_*` options
from [`Kconfig`](../Kconfig) as macros for out-of-tree builds, which see them only as
make variables. That list is unconditional apart from two branches: the manager objects
drop out under `CONFIG_KSU_DISABLE_MANAGER`, and `CONFIG_ARM64` or `CONFIG_X86_64` picks
one `patch_memory.o` and one `syscall_hook.o`. Everything the fork adds -
`feature/mount_hide.o`, `feature/mem_spoof.o`, `feature/ptctl.o`, `feature/uhook.o` -
sits in the unconditional part, so a new file nobody spells out there is never compiled
and its `ksu_*_init()` never links. [`Makefile`](../Makefile) then runs
[`tools/check_symbol.c`](../tools/check_symbol.c) on `kernelsu.ko` against the target
`vmlinux`, the only remaining guard that the unexported symbols `ksuinit` binds from
`/proc/kallsyms` really exist there.

Config macros are only half of what Kbuild hands the compiler; the rest are values it
computes. `KSU_VERSION`, which [`ksu.h`](../include/ksu.h) renames `KERNEL_SU_VERSION`
and the supercall reports to the manager, is 30000 plus `git rev-list --count HEAD`,
and that count is taken only when the KernelSU checkout is a different repository from
`$(srctree)` - KernelSU vendored as a subdirectory of a kernel tree would otherwise be
numbered by the kernel's commits. With no separate repo the build warns and falls back
to `-DKSU_VERSION=16`. `KSU_EXPECTED_SIZE` and `KSU_EXPECTED_HASH`, re-emitted as
`EXPECTED_SIZE` and `EXPECTED_HASH`, are the byte length of the signing certificate
inside the manager APK's v2 signature block and that certificate's SHA-256.
`check_block()` in [`apk_sign.c`](../manager/apk_sign.c) walks the length-prefixed
signers, signer, signed-data and digests sequences to reach the certificate, rejects a
length that does not match before it hashes anything, and compares the hex digest. An
optional second pair covers a parallel signing key, which is why a fork that ships its
own manager edits Kbuild rather than C. `KSU_NEW_DCACHE_FLUSH` is stranger: it is the
exit status of a `grep` for `__flush_dcache_area` in the target's
`arch/arm64/include/asm/cacheflush.h`, so nonzero means the old name is absent and
`arm64/patch_memory.c` compiles `dcache_clean_inval_poc()` instead. A version test would
not answer this, because the newer function appeared in 5.14 upstream and was then
backported to android13-5.10 but not to android12-5.10, leaving two kernels that report
the same version disagreeing about which symbol exists.

Kbuild also has to point the compiler at KernelSU's own headers, which for an external
module is not a given. It appends `-I$(KSU_KERNEL_DIR)` and
`-I$(KSU_KERNEL_DIR)/include`, deriving that directory from `$(src)`: relative to
`$(srctree)` when kbuild hands it a relative path, taken verbatim when absolute. From
6.18 that stopped being enough. A build that keeps sources and output apart with `M=`
and `MO=` no longer sees the module's source directory in `$(src)`; kbuild exports it as
`$(srcroot)`, so the assignment now prefers `$(srcroot)` whenever `KBUILD_EXTMOD` is set
and that variable is non-empty, and takes the old `$(src)` derivation otherwise - which
is what every pre-6.18 kbuild, where `$(srcroot)` does not exist, still gets. Without
that preference the two `-I` flags point into the output tree and every
`#include "ksu.h"` in the module misses.
[`build-all-x64.sh`](../build-all-x64.sh) is where both invocation shapes sit next to
each other: `M=$MDIR MO=$ODIR` for android17-6.18, `M=$ODIR src=$MDIR` for every older
KMI.

The last lines of [`init.c`](init.c) are module metadata, and two of them are load-time
gates rather than documentation. `MODULE_LICENSE("GPL")` is one: the loader resolves an
`EXPORT_SYMBOL_GPL` symbol only for a module that declares a GPL-compatible license. The
other is the namespace import, written twice because the macro changed shape:

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
#else
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif
```

An export can be placed in a named namespace with `EXPORT_SYMBOL_NS_GPL`, and the loader
refuses to resolve it for a module that has not named that namespace with
`MODULE_IMPORT_NS`; the module is rejected at load time and the offending symbol is
named in the log. The namespace above is the one the VFS uses for exports it treats as
filesystem-internal, so a module reaching into mount and file internals has to claim it
up front. From 6.13 the macro takes its argument as a string literal, before that as a
bare token, which is the only thing the `#if` decides.

This gate applies to symbols the module links against in the ordinary way. It says
nothing about the functions the GKI kernel does not export at all - `path_umount()` in
[`kernel_umount.c`](../feature/kernel_umount.c) is the clearest case - which are not
reachable through the loader and are relocated by `ksuinit` from `/proc/kallsyms`
instead.

## See also

- [`kernel/README.md`](../README.md) - build modes, init order, layer map
- [`kernel/hook/README.md`](../hook/README.md) - the dispatcher, LSM patching, probes
- [`kernel/runtime/README.md`](../runtime/README.md) - the boot pipeline this file defers to
- [`kernel/supercall/README.md`](../supercall/README.md) - the ioctl control plane
- [`kernel/selinux/README.md`](../selinux/README.md) - `apply_kernelsu_rules()` and the ksu domain
- [`kernel/policy/README.md`](../policy/README.md) - allowlist, profiles, feature registry
- [`userspace/ksuinit/README.md`](../../userspace/ksuinit/README.md) - the loader and `/ksu_config`
- [`docs/architecture.md`](../../docs/architecture.md) - the repository-wide hub
- [`docs/instrumentation.md`](../../docs/instrumentation.md) - driving ptctl and uhook

<!-- reference links: kernel documentation and man pages -->
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[execveat-2]: https://man7.org/linux/man-pages/man2/execveat.2.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[lsm]: https://docs.kernel.org/security/lsm.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[setresuid-2]: https://man7.org/linux/man-pages/man2/setresuid.2.html
[static-keys]: https://docs.kernel.org/staging/static-keys.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
[uprobetracer]: https://docs.kernel.org/trace/uprobetracer.html
