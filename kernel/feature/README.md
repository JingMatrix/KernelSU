# The feature layer

`kernel/feature/` holds the parts of KernelSU a user can name: `su` runs, an app finds no
module mounts in `/proc/self/mountinfo`, `adb shell` comes up as root, a line appears in
the su audit log. Everything else in the module exists to make these nine files possible.

Four of them install nothing of their own. sucompat, kernel_umount, adb_root and sulog are
handler functions and policy decisions that a lower layer calls into at a moment it has
already arranged to observe: sucompat and adb_root from the syscall dispatcher,
kernel_umount from the [`setresuid`][setresuid-2] hook, sulog from the execve path and the
ioctl table. The other five carry their own machinery: mount_hide and selinux_hide write
function pointers into rodata through `ksu_patch_text()`, mem_spoof registers
[kretprobes][kprobes], ptctl registers a [kprobe][kprobes] and per-thread perf breakpoints,
and uhook registers [uprobe][uprobetracer] consumers. How execution reaches any of it is
[`hook/README.md`](../hook/README.md) and [`supercall/README.md`](../supercall/README.md).

## The shape they share

A feature that the user can switch on and off registers a handler. Six of the nine files
declare a `struct ksu_feature_handler` -- seven structs in all, because kernel_umount
declares one for itself and a second for webview_zygote_umount -- and hand each to
`ksu_register_feature_handler()` in [`policy/feature.c`](../policy/feature.c), which
stores the pointer in a fixed array indexed by the id from
[`uapi/feature.h`](../../uapi/feature.h):

```c
static const struct ksu_feature_handler su_compat_handler = {
    .feature_id = KSU_FEATURE_SU_COMPAT,
    .name = "su_compat",
    .get_handler = su_compat_feature_get,
    .set_handler = su_compat_feature_set,
};
```

That function is `__init`, so the compiler puts it in a section the kernel frees once load
finishes: handlers may only be registered during bring-up, and only the pointer is kept,
so the struct needs static storage. `feature_mutex` is held across the callbacks, so a
handler must never call back into `ksu_get_feature()` or `ksu_set_feature()`.

A feature is gated by a flag or by a permission check, not both. Seven ids are registered,
and an id is a wire value rather than an internal label: the manager passes it across the
ioctl boundary and `/data/adb/ksu/.feature_config` stores it on disk, so a number that
changes meaning between two builds silently flips the wrong switch on the next boot.
Upstream allocates from 0 upwards, which is why [`uapi/feature.h`](../../uapi/feature.h)
reserves 16 and above for fork-local features. That reservation was bought the hard way:
upstream took id 5 for `KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT`, which this fork had been using
for `KSU_FEATURE_MOUNT_HIDE`, and mount_hide moved to 16 rather than let a
`.feature_config` written before the rebase enable one feature in the name of the other.

| id | name                    | file            | default |
| -- | ----------------------- | --------------- | ------- |
| 0  | `su_compat`             | sucompat.c      | on      |
| 1  | `kernel_umount`         | kernel_umount.c | on      |
| 2  | `sulog`                 | sulog.c         | off     |
| 3  | `adb_root`              | adb_root.c      | off     |
| 4  | `selinux_hide`          | selinux_hide.c  | off     |
| 5  | `webview_zygote_umount` | kernel_umount.c | off     |
| 16 | `mount_hide`            | mount_hide.c    | on      |

All seven are flipped through `KSU_IOCTL_GET_FEATURE` and `KSU_IOCTL_SET_FEATURE` under
the `manager_or_root` predicate. The kernel keeps no persistence; the on-disk copy is
owned by [`userspace/ksud/src/feature.rs`](../../userspace/ksud/src/feature.rs), whose
`FeatureId` enum carries the same seven numbers, whose `parse_feature_id()` accepts either
the name or the decimal id -- `ksud feature set mount_hide 1` and `ksud feature set 16 1`
are one command -- and whose `list_features()` and `save_config()` walk the same seven. An
id that enum does not recognise is still forwarded to the kernel verbatim by
`apply_config()`, which is what lets a newly added kernel-side id survive a ksud that
predates it. mem_spoof, ptctl and uhook have no id and no toggle: they are armed by
root-only supercalls in [`supercall/dispatch.c`](../supercall/dispatch.c), and
`only_root()` in [`supercall/perm.c`](../supercall/perm.c) is exactly
`current_uid().val == 0` -- any root context on the device, not just the manager or
`ksud`.

A feature must be able to retire itself. Every object installed here -- a kprobe, a
[kretprobe][kprobes], a uprobe consumer, a perf breakpoint, a function pointer written into
rodata -- points at module text, and unloading frees that text, so anything still pointing
into it is a use-after-free the next time it fires. [`core/init.c`](../core/init.c) takes
down uprobe consumers, the killguard kprobe and armed HW breakpoints in a first phase,
before a `synchronize_rcu()` and before any data structure is released. mem_spoof breaks the
rule.

Three different sites do the registering, which matters when tracing why a feature is or
is not live. `ksu_adb_root_init()`, `ksu_sulog_init()` and `ksu_selinux_hide_init()` run
from `kernelsu_init()`; `ksu_sucompat_init()` from `ksu_syscall_hook_manager_init()` in
[`hook/syscall_hook_manager.c`](../hook/syscall_hook_manager.c); and
`ksu_kernel_umount_init()` and `ksu_mount_hide_init()` from `ksu_setuid_hook_init()` in
[`hook/setuid_hook.c`](../hook/setuid_hook.c) -- so mount_hide's rodata patching happens
inside syscall-hook bring-up, not module init, on both the boot and the late-load path.
`ksu_kernel_umount_init()` registers both of its handlers at that point, so
`webview_zygote_umount` answers `KSU_IOCTL_GET_FEATURE` only once the setuid hook is up,
the same moment `kernel_umount` does.

Each feature ships a header beside its `.c`, and they are thin on purpose: the hook and
supercall layers include the header and never the implementation, so the only names a
caller can reach are the ones the feature chose to publish. Most headers are the init and
exit pair plus those entry points and nothing more -- [`mount_hide.h`](mount_hide.h) is
two declarations, and [`selinux_hide.h`](selinux_hide.h), [`adb_root.h`](adb_root.h) and
[`sulog.h`](sulog.h) add the handful of functions the boot-stage and execve paths call.
[`ptctl.h`](ptctl.h) and [`uhook.h`](uhook.h) forward-declare their command struct rather
than including [`uapi/supercall.h`](../../uapi/supercall.h), so a file that only needs the
entry point does not drag the whole ABI in with it. Two headers publish state a caller
outside the file actually needs, and both have a reason. [`sucompat.h`](sucompat.h)
exports `ksu_su_compat_enabled` because the bridge in
[`hook/syscall_event_bridge.c`](../hook/syscall_event_bridge.c) tests that flag before
routing anything into sucompat, and [`kernel_umount.h`](kernel_umount.h) declares
`struct mount_entry` alongside the `mount_list` and `mount_list_lock` externs, which is
how mount_hide's matcher reads the same list the umount walk consumes. That header also
publishes `ksu_webview_zygote_umount_enabled`, which `ksu_uid_should_umount()` in
[`policy/allowlist.c`](../policy/allowlist.c) reads to answer for uid 1053 -- the one
place a policy file includes a feature header rather than the other way round.
[`hook/setuid_hook.c`](../hook/setuid_hook.c) and
[`supercall/dispatch.c`](../supercall/dispatch.c) include the same header too, for
`ksu_handle_umount()` and for the list dispatch.c owns, but those are a hook and the
supercall layer reaching down, which is the normal direction; a policy file reaching up
into a feature is not. [`mem_spoof.h`](mem_spoof.h) externs `spoof_total_ram_pages` that
nothing outside [`mem_spoof.c`](mem_spoof.c) reads, and declares no init and no exit at
all -- the header-level shape of the teardown gap described below.

## sucompat

The user-visible contract is the legacy one: typing `su`, or calling
`execve("/system/bin/su", ...)` from an app the allowlist grants root to, produces a root
shell, with no `su` binary anywhere on disk.

[`sucompat.c`](sucompat.c) publishes four handlers, reached through
[`hook/syscall_event_bridge.c`](../hook/syscall_event_bridge.c) for `__NR_execve`,
`__NR_execveat`, `__NR_newfstatat` and `__NR_faccessat`. Each opens with
`ksu_is_allow_uid_for_current(current_uid().val)` -- the real uid, so an app that has
already `seteuid`ed cannot dodge it -- and compares the path against `su_path`, declared as
`static const char su_path[] = SU_PATH;` with `#define SU_PATH "/system/bin/su"` above it.
Since `sizeof` counts the NUL, the `memcmp` is an exact 15-byte match.

The stat and access handlers rewrite one register and re-issue the syscall.
`filename_user` is the *address* of the second-argument register inside `pt_regs`, so a
write through it changes what the real syscall entry reads. The handler swaps in a copy of
`/data/adb/ksud` placed below the caller's stack pointer by `userspace_stack_buffer()`,
calls `ksu_syscall_table[orig_nr](regs)` under `override_creds(ksu_cred)`, then restores
the register. The credential override is what makes the reroute succeed: `/data/adb` is
not traversable by an app uid, so without it the syscall returns `EACCES`. The restore
matters because `pt_regs` is the task's live register file -- only `x0` is overwritten by
the syscall return on arm64, so a clobbered `x1` would still be visible afterwards.

The exec handler opens `/data/adb/ksud` with `O_PATH` under `ksu_cred`, installs the
descriptor with `fd_install()`, and rewrites the frame into a five-argument
[`execveat`][execveat-2]:

```c
regs->__PT_PARM5_REG = AT_EMPTY_PATH;
regs->__PT_SYSCALL_PARM4_REG = envp;
regs->__PT_PARM3_REG = (unsigned long)argv_user;
regs->__PT_PARM2_REG = empty_user_path();
regs->__PT_PARM1_REG = tmp_fd;
```

`argv_user` and `envp` are parameters of `ksu_handle_execve_sucompat_common()` rather than
reads of the live frame, which is what lets one body serve two syscalls: the
[`execve`][execve-2] entry point picks them out of the second and third argument registers,
the `execveat` one out of the third and fourth, and neither has to know where the other's
arguments sat. Hooking `execveat` is recent, and not optional -- bionic now routes
[`execve()`][execve-2] through `execveat(AT_FDCWD, path, argv, envp, 0)`, so a build
watching only `__NR_execve` stopped seeing `su` at all on new devices. The shared body
declines anything that is not that exact shape:

```c
if (execveat && ((int)PT_REGS_PARM1(regs) != AT_FDCWD || (int)PT_REGS_PARM5(regs) != 0))
    goto do_orig_execve;
```

A dirfd-relative call or one carrying `AT_EMPTY_PATH` is not a path-based `execve` in
disguise; its filename argument is resolved against a descriptor, or ignored entirely, so
comparing it to `/system/bin/su` would answer a question nobody asked.

`escape_with_root_profile()` in [`policy/app_profile.c`](../policy/app_profile.c) then
applies the caller's root profile and `ksu_syscall_table[__NR_execveat](regs)` runs.
`argv` passes through untouched, so the new process sees `argv[0] == "su"` and
[`userspace/ksud/src/cli.rs`](../../userspace/ksud/src/cli.rs) dispatches to its
root-shell path. Handing over a ready-made `O_PATH` descriptor is what removed the older
design's need to grant the caller `CAP_DAC_READ_SEARCH`. Note that the escalation is never
undone: on a failed `execveat` the handler restores the five registers and returns the
error, but the caller keeps its new credentials.

The cost is paid per watched task, not per `su`. A `sys_enter` [tracepoint][tracepoints]
fires only for a task carrying the `SYSCALL_TRACEPOINT` work flag, and
`ksu_mark_running_process_locked()` in [`hook/tp_marker.c`](../hook/tp_marker.c) sets that
flag on KernelSU-domain root tasks, zygote, uid 2000, init and every allow-listed uid, and
clears it on everything else -- so an app that was never granted root never enters
`ksu_sys_enter_handler` at all. A marked task pays that handler on every syscall it makes: a
compat test and one `READ_ONCE` of `syscall_hooks[id]`. The five numbers registered by
`ksu_syscall_hook_manager_init()` -- `setresuid`, `execve`, `execveat`, `newfstatat` and
[`faccessat`][faccessat-2] -- then take the dispatcher detour whether or not the caller is
allowed, because the redirect is keyed on the syscall number and the uid check happens
inside the handler. That accounting widens the moment some other subsystem registers a
syscall tracepoint of its own: the kernel's `syscall_regfunc()` marks every task on the
system, and `syscall_regfunc_handler()` sees the reference count reach one and calls
`ksu_mark_all_process()` rather than re-narrowing, because the other subsystem must get the
tracing it asked for -- and until it unregisters, every process on the device runs
`ksu_sys_enter_handler` on every syscall. An actual `su` costs more than the detour -- the
`newfstatat` and `faccessat` paths each add a `kern_path()` existence probe of
`/data/adb/ksud` and a `copy_to_user()` below the stack pointer, and the execve path adds
`get_unused_fd_flags()`, a `filp_open()` under `ksu_cred`, `fd_install()` and
`escape_with_root_profile()` before the rewritten frame re-enters the syscall table as
`execveat`.

What still detects it: the interception is deliberately narrow. Only `faccessat`,
`newfstatat`, `execve` and `execveat` are hooked, so `faccessat2` and [`statx`][statx-2] see
nothing, an `execveat` against a real dirfd falls through the shape check above, and only
the exact path `/system/bin/su` matches. 32-bit tasks never reach any of it, because
`ksu_sys_enter_handler` returns early -- on `is_compat_task()` under arm64, on
`in_compat_syscall()` under x86_64. And for an allow-listed app the reroute answers honestly
about the wrong file: `newfstatat` returns the `struct stat` of `/data/adb/ksud`, whose
`st_dev`, `st_ino` and multi-megabyte `st_size` describe the daemon on userdata rather than
a plausible `su`. Hiding is the other features' job.

## kernel_umount

For an app that should not see the module mounts, the cleanest answer is to detach them
from that app's mount namespace before it runs a single instruction of its own code.
[`kernel_umount.c`](kernel_umount.c) does that.

A mount namespace is the per-process view of the mount table. Android's zygote gives each
app a private one with `unshare(CLONE_NEWNS)` during specialization, *before* it drops to
the app uid and *before* the [SELinux][selinux] context changes. That ordering is why the
trigger is the `setresuid` hook: at that instant the child owns a private namespace but is
still labelled `u:r:zygote:s0`. `ksu_handle_umount()` applies four gates --
`ksu_module_mounted` from [`runtime/boot_event.c`](../runtime/boot_event.c), the feature
flag, a uid check that admits an app uid, an isolated uid or `WEBVIEW_ZYGOTE_UID`, and
`ksu_uid_should_umount()` from [`policy/allowlist.c`](../policy/allowlist.c) -- then one
more, `is_zygote(current_cred())`, whose absence the source comment calls a disaster: a root
app that `setuid`s to an app uid while sitting in the *global* namespace would otherwise
trigger a system-wide detach of every module.

Isolated processes take a shortcut through the profile gate: the test is
`!ksu_uid_should_umount(new_uid) && !is_isolated_process(new_uid)`, so an isolated uid
overrides a profile that answered false and has the mounts detached whether or not a
profile asks for it -- it can never legitimately need the module view.
Which uids that covers is a fork change worth knowing here, because mount_hide leans on
the same helper. Android hands an isolated process forked from an *app* zygote an appid in
[90000, 98999] instead of the regular [99000, 99999], and the stock helper knew only the
second range, so a Chrome sandboxed renderer was neither unmounted for nor hidden from.
`is_isolated_process()` in
[`policy/allowlist.h`](../policy/allowlist.h) now spans both --
`FIRST_APP_ZYGOTE_ISOLATED_UID` through `LAST_APP_ZYGOTE_ISOLATED_UID` beside the original
pair -- which is why there is one helper and not two.

The mountpoints are not compiled in. `mount_list` and `mount_list_lock` live in
[`supercall/dispatch.c`](../supercall/dispatch.c) and are populated only through
`KSU_IOCTL_ADD_TRY_UMOUNT`, exposed as `ksud kernel umount {add,del,wipe}`. Entries go in
at the head and the walk runs head to tail, so detach order is the reverse of registration
order. The walk runs under `override_creds(ksu_cred)`, because by then the caller is an
unprivileged app uid while `path_umount()` reaches `may_mount()`, which demands
`CAP_SYS_ADMIN` in the namespace's user namespace. `try_umount()` skips any path whose
dentry is not the mount root, which makes the list idempotent. One reference rule must
survive edits: `path_umount()` consumes the path reference on every exit path, so
`try_umount()` calls `path_put()` itself only on the not-a-mount-root return, and not at
all after `ksu_umount_mnt()`; the `kern_path()` failure return has no reference to drop.
The lock order this establishes, `mount_list_lock` read then `namespace_sem` write, is what
forces mount_hide to trylock the same rwsem.

The list is empty on a fresh boot, because nothing populates it automatically. The one
caller of `umount_list_add()` in
[`userspace/ksud/src/ksucalls.rs`](../../userspace/ksud/src/ksucalls.rs) is the
`ksud kernel umount add` arm of the CLI in
[`userspace/ksud/src/cli.rs`](../../userspace/ksud/src/cli.rs), so whatever agent performs
the module mounts has to register its own mountpoints or the feature walks an empty list.

What still detects it: unmounting is itself a deviation. Removing a mount changes the
number of distinct mount views a probe can collect relative to the number of propagation
classes, which is exactly the differential a mount-aware probe looks for. 32-bit apps are
not covered, for the same compat-gate reason as sucompat.

## webview_zygote_umount

Android runs a second zygote at uid 1053, `webview_zygote`, which forks the sandboxed
renderer processes that WebView and Chrome push page content into. It is specialized like
an app -- private mount namespace first, uid drop and domain transition after -- so the
`setresuid` trigger reaches it, and every renderer it forks inherits the namespace it ends
up with. That inheritance is the whole reason this toggle exists in
[`kernel_umount.c`](kernel_umount.c) rather than as a separate mechanism.

For a long time the answer was a flat refusal: `ksu_uid_should_umount()` returned false for
`WEBVIEW_ZYGOTE_UID` unconditionally, because detaching the module mounts there removes
them from every page any app renders, and a module that binds a file WebView loads breaks
everywhere at once. The refusal is now a policy question instead of a constant --

```c
if (unlikely(uid == WEBVIEW_ZYGOTE_UID)) {
    return ksu_webview_zygote_umount_enabled;
}
```

-- with the flag defaulting to false, so nothing changes for a user who does not ask. The
manager surfaces it as "Unmount for WebView", with a summary that names both halves of the
trade: it prevents information leaks from the WebView process and it may break modules.
The setting takes effect at the next specialization, which is why the manager also says to
reboot.

The source enumerates six ways a process arrives at the `setresuid` hook, and only four of
them need code:

| # | transition                                    | handled by |
| - | --------------------------------------------- | ---------- |
| 1 | zygote -> app uid                             | the app-uid gate |
| 2 | zygote -> isolated process                    | the isolated-uid gate |
| 3 | zygote -> app zygote (an app uid)             | same as 1 |
| 4 | zygote -> webview zygote                      | this feature |
| 5 | app zygote -> isolated process                | inherited from 3 |
| 6 | webview zygote -> isolated renderer           | inherited from 4 |

Scenarios 5 and 6 are covered without firing a second detach, and the reason is worth
following, because it is the same `is_zygote()` gate that looks like an obstacle
elsewhere. A renderer forked from `webview_zygote` calls `setresuid` while labelled
`u:r:webview_zygote:s0`, not `u:r:zygote:s0`, so `is_zygote(current_cred())`
returns false and `ksu_handle_umount()` bails out with a log line. It does not need to do
anything: the renderer inherited its mount namespace from the webview zygote, and that
namespace already had the module mounts detached at scenario 4. Trying to detach again
would at best repeat work and at worst reach a namespace nobody verified is private.

What this does not do is change what a process *sees* in `/proc` when the mounts are still
there. Unmounting and filtering answer different questions -- one changes the mount table
for uid 1053 and its forked children, the other changes what `/proc/pid/mountinfo` prints
for any isolated reader, in any namespace -- and both are in the tree. A device that leaves
this feature off because a module must stay mounted for WebView still gets mount_hide's
filtering for the renderers, which is the point of having both.

## adb_root

This makes `adbd` a root daemon in KernelSU's own SELinux domain, so `adb shell` starts
privileged. [`adb_root.c`](adb_root.c) is off by default, and
`DEFINE_STATIC_KEY_FALSE(ksu_adb_root)` patches the branch out of the instruction stream
while disabled -- worth a [jump label][static-keys] because it sits on the exec path of
every init-domain fork on the device, `execve` and `execveat` alike.

Detection is a suffix match: `is_exec_adbd()` reads the exec target into a 40-byte buffer
and compares the last six bytes against `"/adbd"` including its NUL, so
`/system/bin/adbd`, `/apex/com.android.adbd/bin/adbd` and any other path ending in `/adbd`
all match. `is_libadbroot_ok()` then requires `/data/adb/ksu/lib/libadbroot.so` to exist;
without it the feature logs a hint to run `ksud install` and does nothing.

The kernel grants exactly one thing. `setup_ld_preload()` writes
`LD_PRELOAD=/data/adb/ksu/lib/libadbroot.so` and `LD_LIBRARY_PATH=/data/adb/ksu/lib` below
the caller's stack pointer, reads the existing `envp` array out of userspace in batches of
sixteen pointers, appends the two entries and a NULL terminator, and writes the address of
the rebuilt array back through the caller-supplied `envp_p`. Writing below the stack
pointer is safe because the exec path copies argv and envp out of the old address space in
`copy_strings()` before `exec_mmap()` tears it down. `escape_to_root_for_adb_root()` in
[`selinux/selinux.c`](../selinux/selinux.c) then moves the pre-exec credential into
`u:r:ksu:s0` with `clear_exec_sid = true`. That argument is load-bearing: init has
already computed an `exec_sid` for the `u:r:adbd:s0` transition, and leaving it set would
move adbd straight back out of the KernelSU domain on the very next exec.

Both exec syscalls reach that path. `ksu_adb_root_handle_execve()` and
`ksu_adb_root_handle_execveat()` test the [static key][static-keys], then call
`do_ksu_adb_root_handle_execve()` with `regs` plus two register-derived pointers -- the
filename and the address of the envp register -- taken from the first and third argument
registers for `execve` and from the second and fourth for `execveat`. Nothing below those
two wrappers reads a register by position, so `is_exec_adbd()` takes a
`const char __user *` and `setup_ld_preload()` takes an explicit `unsigned long *envp_p`;
the write that installs the rebuilt array goes through that pointer and lands in whichever
register the entering syscall actually used.

Uid and capabilities are handled in userspace by
[`manager/app/src/main/cpp/adbroot.cc`](../../manager/app/src/main/cpp/adbroot.cc), which
fakes the `service.adb.root` property so adbd's own `should_drop_privileges()` decides not
to drop. `ksud install --libadbroot` puts it in place
([`userspace/ksud/src/utils.rs`](../../userspace/ksud/src/utils.rs)).

The result is visible in a shell, intentionally: adbd runs as uid 0 in `u:r:ksu:s0`, which
`ps -Z` reports, and the preload appends `/data/adb/ksu/bin` to `PATH`. One gap is marked
in the source as a TODO: the injected entries land after the caller's existing environment
and `getenv()` returns the first match, so an adbd already started with an `LD_PRELOAD`
keeps its own and the feature silently no-ops. The suffix match is also the whole of the
identity check -- any init-domain exec of a path ending in `/adbd` is treated as adbd, and
nothing verifies what the binary is.

## mount_hide

Where kernel_umount removes mounts, mount_hide removes *records*. Nothing is unmounted and
no mount structure is touched; the three procfs mount files are filtered per reader.
[`mount_hide.c`](mount_hide.c) opens with a design comment worth reading in full: it names
the two probes the filter defeats and argues why filtering, not unmounting, is what defeats
them.

Neither replaces the other. Detaching a mount only helps a process whose namespace is
private and whose specialization the `setresuid` hook actually observed; filtering helps
any reader, including one reading its own `/proc/self/mountinfo` out of init's global
namespace, which is exactly the Android 17 `zygote_next` case the umount path cannot
reach. Running both means a renderer sees neither the mounts nor the records, and a device
that has to leave webview_zygote_umount off still gets the second half.

The hook point is the `.open` slot of `proc_mounts_operations`,
`proc_mountinfo_operations` and `proc_mountstats_operations`, all `const` and therefore in
rodata. Writing them goes through `ksu_patch_text()`, which has one implementation per
architecture -- [`hook/arm64/patch_memory.c`](../hook/arm64/patch_memory.c) and
[`hook/x86_64/patch_memory.c`](../hook/x86_64/patch_memory.c) -- both of which walk
`init_mm` to the physical address, map the page through the fixmap and store inside
`stop_machine()`; the x86_64 one reaches for `FIX_BTMAP_BEGIN` because modern x86 dropped
`FIX_TEXT_POKE0`. Flipping the PTE writable instead is what vendor hypervisor monitors
detect. The shim calls the saved original, which allocates the per-file
`struct proc_mounts`, then overwrites that object's `show` member if the reader is a hide
target. That swap is a plain store to freshly `kmalloc`ed memory, so the expensive rodata
write happens exactly three times, at init.

`ksu_filter_show()` lets the kernel's own `show_vfsmnt`, `show_mountinfo` or
`show_vfsstat` render the record into the seq buffer, then erases it by rewinding
`m->count`:

```c
if (ksu_region_has_marker(m->buf + start, m->count - start) ||
    ksu_region_has_mount_list(m->buf + start, m->count - start) || ksu_mnt_root_hidden(mnt))
    m->count = start; /* erase the record */
```

Three matchers, because one is not enough. The marker scan is case-insensitive over the
rendered bytes and looks for `/adb/`, `/data/adb`, `kernelsu`, `magisk`, `zygisk` and
`/debug_ramdisk`. But `/proc/pid/mounts` and `mountstats` omit the mount-root field, so a
module bind mount renders there with a stock-looking mountpoint and no marker at all. The
umount-list matcher covers that where the list is populated, and `ksu_mnt_root_hidden()`
covers it unconditionally by scanning `dentry_path_raw(mnt->mnt_root)` instead --
format-independent, and short-circuited by `IS_ROOT()` for whole-filesystem mounts.

Two things look odd and are not. `ksu_region_has_mount_list()` uses
`down_read_trylock(&mount_list_lock)` and skips the matcher on contention, because it runs
inside the seq `->show` callback with `namespace_sem` held for read while the umount path
takes `mount_list_lock` read then `namespace_sem` write; blocking here inverts that order
and deadlocks against a queued writer. And `ksu_unhook_one()` leaves the saved original
non-NULL after restoring the slot, because a `seq_file` opened while hooked still calls
through it.

The hide predicate is `(is_appuid || is_isolated) && (is_isolated ||
ksu_uid_should_umount)`, evaluated once at open against `current`. `is_isolated` is the
shared `is_isolated_process()` from [`policy/allowlist.h`](../policy/allowlist.h), the same
helper the umount path calls and the one whose range the fork widened to cover app-zygote
spawns. Sharing it is deliberate rather than incidental: a uid the two paths disagreed
about would be unmounted for and not filtered, or filtered and not unmounted, and either
mismatch is a differential a probe can read directly. Because the predicate is fixed at
open, every file that reader opens -- its own and every other pid's -- loses the same
records and the views cannot diverge. Unlike the umount path there is no `is_zygote()`
requirement, because filtering a view is safe in the global namespace where unmounting
would be destructive.

The residual surface: root, shell, `system_server` and the manager read the truth, so this
is a property of the reader and not of the mount, and an app whose profile sets
`umount_modules = false` is not filtered either. Disabling the feature does not unhook --
the set path only clears the boolean, so the three rodata pointers keep aiming at module
text until module exit. Only those three files are covered, and unloading the module while
a hide target holds one of them open leaves that file's `show` pointer aimed at freed
text.

## mem_spoof

[`mem_spoof.c`](mem_spoof.c) inflates the machine's apparent RAM. It is the odd file here:
no feature id, no toggle, no persistence, no manager path, and nothing in `userspace/` or
`manager/` that issues its ioctl. The only entry point is `ksu_set_spoof_mem()`, reached
from `do_set_spoof_mem()` on `KSU_IOCTL_SET_SPOOF_MEM` with `only_root`.

Five values move, and only five. Three kretprobes -- a kretprobe fires on function entry
and again on return, so the return handler can rewrite the return register -- sit on
`si_meminfo`, `si_mem_available` and `vm_commit_limit`. The `si_meminfo` probe needs both
halves: its entry handler stashes the caller's `struct sysinfo *` into `ri->data`, and the
return handler writes through it.

```c
if (val && spoof_total_ram_pages > 0) {
    unsigned long real_total = val->totalram;
    if (spoof_total_ram_pages > real_total) {
        unsigned long diff = spoof_total_ram_pages - real_total;
        val->totalram = spoof_total_ram_pages;
        /* Proportionally scale freeram up so free memory ratios look natural */
        val->freeram += diff;
    }
}
```

The `>` guard is why the feature can only inflate: a smaller target would underflow the
unsigned subtraction. `si_meminfo` feeds both `/proc/meminfo`'s `MemTotal` and `MemFree`
and the `sysinfo(2)` syscall, so hooking the shared producer keeps the two consistent by
construction. `si_mem_available` is `MemAvailable` and takes the same absolute offset;
`vm_commit_limit` is `CommitLimit` and is the one value scaled multiplicatively.
`totalcma_pages` is an ordinary `.data` symbol and is not probed at all:
`ksu_spoof_cma_pages()` resolves it once through `find_kernel_symbol_exact()` in
[`infra/symbol_resolver.c`](../infra/symbol_resolver.c), caches the original and writes it
directly.

`register_kretprobe` and `unregister_kretprobe` are themselves resolved by name, so a
kernel without kretprobe support yields `-ENOSYS` rather than an unresolved symbol that
would fail the module's load. All three probes go up together or come down together, for
the same reason mount_hide hooks three files or none: a `MemTotal` that moved while
`MemAvailable` and `CommitLimit` did not is a self-inconsistency.

The costs are larger than the feature. The spoof is global once armed -- no uid, profile
or namespace scoping -- so `system_server` and every in-kernel consumer see the inflated
numbers, and `si_mem_available()` is what sizes the trace ring buffer. There is also no
`ksu_mem_spoof_exit()` and nothing in `kernelsu_exit()` disarms it, so a spoof armed when
the module is unloaded leaves three kretprobes pointing at freed module text and
`totalcma_pages` permanently wrong. Compare the explicit `ksu_uhook_exit()` and
`ksu_ptctl_exit()` calls in [`core/init.c`](../core/init.c), which exist for exactly this
hazard.

What still detects it: only the five values above change. `/proc/zoneinfo`,
`/proc/vmstat`, the memory blocks under `/sys/devices/system/memory` and the device's
advertised specification all continue to describe the real machine.

## selinux_hide

An app that reads selinuxfs should see a stock device. [`selinux_hide.c`](selinux_hide.c)
answers `/sys/fs/selinux/context`, `/sys/fs/selinux/access`, writes to
`/proc/self/attr/current`, and `/sys/fs/selinux/status` from a pristine copy of the policy
taken before KernelSU edited anything. The policy machinery -- how `backup_sepolicy` is
duplicated in [`selinux/rules.c`](../selinux/rules.c) and how a live policydb is edited at
all -- is [`selinux/README.md`](../selinux/README.md); read that first.

The hooks are of three kinds. `write_op[SEL_CONTEXT]` and `write_op[SEL_ACCESS]` are
function pointers in a selinuxfs table, patched with `ksu_patch_text()` exactly as
mount_hide patches `.open`. `selinux_setprocattr` is displaced through
[`hook/lsm_hook.c`](../hook/lsm_hook.c): an [LSM][lsm] hook is an entry in the kernel's
security-hook table, and since `security_add_hooks()` is unavailable to an out-of-tree
module the fork overwrites an entry in place. And `sel_handle_status_ops.open` is patched
unconditionally at module init, ahead of the feature being enabled, which is why that
handler tests `ksu_selinux_hide_enabled` itself.

Three of them -- `my_write_context()`, `my_write_access()` and `my_setprocattr()` -- begin
with the same gate, `current_uid().val < 10000` falling through to the original: system
uids get real answers, app uids get answers computed against `backup_sepolicy`.
`my_write_access()` forges one more field on the way out, pinning `avd.seqno` to 1 before
formatting the reply, because a policy sequence number is a counter and the backup is only
a snapshot: whatever loader brought KernelSU up may have run its own `load_policy` before
the module captured the policy, and answering with the resulting count reports a device
that reloaded its policy at boot; a stock device answers 1. The status handler inverts the
uid test. `my_sel_open_handle_status()` asks for
`current_uid().val >= 10000 && ksu_selinux_hide_enabled` and, when that holds, points
`filp->private_data` at a prebuilt fake page instead of computing anything.
`initialize_fake_status()` allocates that page, copies the real
`struct selinux_kernel_status` into it, and on a late load overwrites the three fields the
snapshot cannot be trusted for. A late load arrives after whatever loader started the
module has already acted, so the captured page can record a `setenforce 0` and a
`load_policy` that no stock boot performs: `enforcing` goes back to 1, and `sequence` and
`policyload` are set to what a stock boot ends at rather than to what was observed. From
6.10 that is 4 and 1 -- one policy load plus one setenforce, each moving the sequence by
two -- and below it 0 and 0, where a stock page is still at the values it was created with.
Serving a fixed pair is what keeps the numbers from encoding the loader's own actions.
`backup_sepolicy` must exist: `ksu_selinux_hide_enable()` returns `-EAGAIN` when it does
not, and `ksu_selinux_hide_drop_backup_if_unused()` frees the backup at
boot-completed if the feature is not running. Enabling selinux_hide on a boot that started
with it disabled therefore requires a reboot, which the manager surfaces as its own
string.

What still detects it: the answers describe the pre-KernelSU policy, which is both the
point and the seam. Any rule `ksud` injected is invisible to a query, but its *effect* is
not -- an operation that succeeds despite the reported policy denying it is a
contradiction an app can construct. Nothing below uid 10000 is covered, and the fake
status page is frozen: `sequence` and `policyload` never move again, so an app that mmaps
it and watches for a policy reload sees a device on which policy is never reloaded, which
on a long-running system is its own anomaly.

## sulog

[`sulog.c`](sulog.c) is 66 lines and does almost nothing: it owns a boolean, registers
`KSU_FEATURE_SULOG` (default off), and calls the init and exit functions of the two files
that do the work. `ksu_sulog_is_enabled()` is tested inside the capture path rather than
at the hook sites, so a disabled log costs one boolean read per event.

The capture, the queue and the reader fd are [`sulog/README.md`](../sulog/README.md). What
belongs here is where events come from: three producers, namely root exec and sucompat
exec from [`hook/syscall_event_bridge.c`](../hook/syscall_event_bridge.c) and
[`sucompat.c`](sucompat.c), plus `KSU_IOCTL_GRANT_ROOT` in
[`supercall/dispatch.c`](../supercall/dispatch.c). Both exec producers sit in the bodies
shared by `execve` and `execveat`, so an `su` that arrives as either syscall is recorded
once and identically. Each exec site is two-phase -- identity and argv before the syscall,
result after -- because a record built afterwards would describe the new program rather
than the caller. The queue in [`infra/event_queue.c`](../infra/event_queue.c) never blocks
a syscall: with no reader attached, or a slow one, records are dropped and the loss is
counted so the drain reports a gap instead of hiding it. One `[ksu_sulog]` fd may be open
at a time and reads are destructive. A process that takes root and never execs is
recorded once, at the grant. This is an audit trail for the device owner, not a
concealment feature.

## ptctl

[`ptctl.c`](ptctl.c) is a process-control and debugging surface for a root process: read and
write another task's memory and user registers, query it, signal it, make a thread group
survive injected lethal signals, and park a thread on a hardware breakpoint so the other
verbs can rewrite its live frame before it resumes. Twelve operations behind one ioctl,
`KSU_IOCTL_PTCTL`, none of them going through [`ptrace`][ptrace-2] -- the target's
`TracerPid` stays 0, so a self-attaching anti-debug guard sees nothing. `PEEK` and `POKE`
use `access_process_vm()` with `FOLL_FORCE` on both sides, capped at 64 KiB per call by
`PTCTL_MAX_CHUNK` so the chunking is the caller's job -- a larger `len` is refused with
`-EINVAL`, never split -- and translate a zero return to `-EIO` because that function
reports total failure as a zero byte count rather than an errno. How to drive the two
features from a tool is [`docs/instrumentation.md`](../../docs/instrumentation.md); the
per-argument contract is in [`uapi/supercall.h`](../../uapi/supercall.h).

Register access carries the most reasoning per line. Only the user-visible frame crosses
the boundary -- `struct user_pt_regs` (272 bytes) on arm64, `struct pt_regs` (168 bytes)
on x86_64 -- because arm64's `pt_regs` carries 64 bytes of kernel-private tail that a
userspace write turns into a crash. An incoming frame is sanitised into a local copy
through `valid_user_regs()` before anything is committed, mirroring `PTRACE_SETREGSET`;
skipping that turns a root ioctl into EL1 code execution, since SPSR and pc are reloaded
verbatim by the `ERET` out of `kernel_exit`. The transfer runs inside `task_call_func()`,
which pins the target under its `pi_lock` with interrupts disabled, and the callback
refuses anything still on a runqueue:

```c
if (st == TASK_RUNNING || st == TASK_WAKING || p->on_rq)
    return -EBUSY;
```

`task_call_func()` holds off a de-schedule but does not stop a running task, so without
that gate a concurrent kernel entry overwrites the frame being written and tears the one
being read. A write is refused again when the target sits inside a syscall, which is the
usual off-CPU state for an Android thread: on resume `syscall_set_return_value()` rewrites
`x0` and `do_signal()` can rewind pc by four and restore `x0` from `orig_x0`. Both
`orig_x0` and `syscallno` live in the kernel-private tail, outside the writable view, so
the caller cannot suppress that rewrite the way a full `pt_regs` write once could -- the
frame would be silently undone rather than rejected. x86_64 reaches the same refusal by
testing `(long)regs->orig_ax >= 0`, which is how `syscall_get_nr()` recognises a syscall
frame there. `task_pt_regs()` is dereferenced only under `try_get_task_stack()`, because
`get_task_struct()` pins the `task_struct` but not the separately refcounted kernel stack
`pt_regs` lives on.

KILLGUARD puts a kprobe at offset 0 of `do_send_sig_info()` and, for a guarded thread
group, emulates a return of 0 without executing one instruction of the body. Rewriting the
`sig` argument to zero would not work: the function has no zero check, and
`sigaddset(&pending->signal, 0)` computes `1UL << -1`, which AArch64 truncates to a shift
of 63 and marks SIGRTMAX pending -- the target dies of signal 64 instead. Two guards keep
the emulation honest. `sig_kp` carries an empty `.post_handler` that is never invoked,
purely because `optimize_kprobe()` refuses to jump-optimize a probe that has one; on the
x86_64 optprobe path the pre-handler's return value and its pc and sp writes are all
discarded, so KILLGUARD would report success while protecting nothing. Registration also
refuses if `KPROBE_FLAG_FTRACE` comes back set, because an [ftrace][ftrace] kprobe sits at
`__fentry__`, where neither `x30` nor the top of stack describes the caller's frame.

The HWBP hold is the most intricate machinery here. `register_user_hw_breakpoint()` arms
an execute breakpoint on up to 640 threads of a thread group. The overflow handler runs in
debug-exception context and cannot sleep, so it snapshots the frame, takes a module
reference and queues `task_work` with `TWA_RESUME`; that callback runs on the way back to
userspace, inside the same exception and before the `ERET`, which is why `task_pt_regs()`
of the parked thread is the live frame it is about to resume with. One trick deserves
naming: `hwbp_mark_default_step()` plants `perf_event_output_forward` into the event's
`orig_overflow_handler`, a pointer that is never called, solely because arm64's
`breakpoint_handler()` gates its step-past-the-breakpoint dance on a pointer comparison
against that symbol. Without it the thread re-traps forever. That field exists only up to
v6.11: v6.12 calls the [BPF][bpf] overflow handler directly and removed both it and
`uses_default_overflow_handler()`, so on a newer kernel the write is compiled out and
`hwbp_prepare_step()` performs the same dance by hand.

The costs: any uid-0 context can drive all of this. `POKE` is the one verb that changes
bytes -- the write COWs the target's text page and `copy_to_user_page()` flushes the
I-cache, so a checksum over the process's own mapping sees it. Once KILLGUARD is armed
every signal send on the system takes a debug exception, and the probe appears in
`/sys/kernel/debug/kprobes/list`. The guard table stores bare tgids with no exit hook, so
an entry left by a dead process is inherited by whatever later process recycles that tgid.
`SEND_SIG_PRIV` is never intercepted, because blocking an OOM kill would let the reaper
unmap a live process and pin `oom_victims` above zero, breaking suspend. Nothing in
`userspace/` or `manager/` issues this ioctl, and `KSU_PTCTL_DETACH_TRACER` returns
`-ENOSYS`.

## uhook

[`uhook.c`](uhook.c) is the persistent counterpart to ptctl's interactive hold: a uprobe
keyed by `(real inode, file offset)`, scoped to one thread group, that
evaluates an in-kernel condition on each hit and applies one action. A uprobe is the
userspace analogue of a kprobe -- the kernel writes a `BRK` into a private copy-on-write
copy of the target's text page, so the file on disk is untouched but the running process
traps at that address. Keying on the file rather than an address makes a hook immune to
ASLR.

Path resolution uses `d_real_inode()`, never `d_inode()`, and holds the whole `struct
path` for the hook's lifetime. The first is not a style choice: on overlayfs the overlay
inode has no `->read_folio` and `uprobe_register()` refuses it with `-EIO`, which would
make everything under an overlayed `/system` unhookable. The second is the uprobe core's
contract, and its cost is that a hooked filesystem cannot be unmounted until the hook is
removed.

Scope is mandatory, and enforced twice. `ADD` refuses `filter_tgid` 0 with `-EINVAL`; the
consumer's `filter` callback then decides where the breakpoint is *inserted*:

```c
if (mm == current->mm)
    return current->tgid == h->filter_tgid;
return h->filter_mm && h->filter_mm == mm;
```

and `uh_apply()` re-checks the tgid in the hitting thread. Both halves are needed: without
the filter a single hook breakpoints the file in every process that maps it, zygote
included, and without the second check a [`fork()`][fork-2]ed child inherits both the
patched page through COW and `MMF_HAS_UPROBES` through `uprobe_dup_mmap()`, so it traps in
an address space the filter never approved. The anchor is an `mm_struct` held by `mm_count`
rather than a task, because `exit_mm()` clears `task->mm` when a thread exits and a task
anchor would silently disarm; the tgid branch keeps a hook alive across `execve()`, which
replaces the mm entirely.

Neither gate ever had a bug -- both read `if (!h->filter_tgid) return true;` first, exactly
as the header documented, and an unscoped hook does fire: an `ADD` on `libc.so` was measured
landing hits in threads of several unrelated processes. The value was withdrawn because that
is the wrong thing to have done, and because one of its failure modes is unfalsifiable:
`uprobe_register()` returns 0 whether it planted the breakpoint everywhere or nowhere, so an
unscoped `ADD` that armed nothing looks identical to a probe on code the target never runs --
and unscoped is the one setting where nothing in the request had to name a process that
exists. The sibling failure, an unrelated mapper aborting the registration, is at least loud:
that error reaches `uh_add()` and fails the `ADD`.

Action verbs are restricted by site, enforced at ADD time rather than discovered at fire
time. `JUMP`, `SKIP` and writing pc are accepted only at `ON_RET`, because at an entry
site the uprobe core single-steps the probed instruction out of line and
`arch_uprobe_post_xol()` resets pc unconditionally -- and on an arm64 simulated branch it
re-reads the handler-corrupted pc and uses it as the branch base, landing the thread at a
wild address. At a return site `handle_trampoline()` sets pc from the saved return address
*before* running the handlers and never touches it again, so those verbs are reliable
there and `SETREG` of index 0 genuinely forges the function's integer or pointer return
value. `FORCE_RET` is rejected at both sites. The software-step bit is forced to match
`TIF_SINGLESTEP` in both directions, because clearing it strands a thread mid-step:
`user_enable_single_step()` writes it only on the flag's 0-to-1 transition.

The costs: uhook's mechanism *is* a modified instruction, so a target that checksums its
own mapping sees the `BRK`. Once any probe fires, `/proc/<pid>/maps` grows a `[uprobes]`
line and `/proc/<pid>/smaps` reports a non-zero `Anonymous:` inside an `r-xp` file mapping
that normally has none -- `Anonymous:` only, since the COW'd text page is accounted clean
(measured: 4 kB `Anonymous`, 4 kB `Private_Clean`, 0 kB `Private_Dirty`). One global ring
of 512 records is shared by all 32 hooks and reads are destructive, so two consumers steal
each other's records with no in-band signal -- measured, one drain took 26 records and a
concurrent one took 0, neither told anything. The same ring is why a busy hook hides a quiet
one: an unscoped probe on `libc`'s `open` produced 6996 records in eight seconds against the
target's own 8, and a single drain afterwards returned 512 survivors with none of the target's
among them. There is no spare output word to carry a
signal without moving the ioctl number, so `KSU_UHOOK_LIST`'s per-hook
`traps`/`hits`/`fails` counters are the way to notice it. A handler that faults on user
memory sleeps holding the uprobe's `register_rwsem` for read, stalling removal and every
other thread at the same probe. Two version gates split the file. `UHOOK_NEW_UPROBE`
selects the 6.12 uprobe ABI, where `uprobe_register()` absorbed the `ref_ctr_offset`
argument and returns a `struct uprobe *`, `uprobe_unregister()` split into `_nosync` and
`_sync`, and the filter lost its `uprobe_filter_ctx`; `UHOOK_SESSION_COOKIE` selects the
6.13 handler signatures that carry a trailing `__u64 *data`. Both branches are built now:
`.github/workflows/build-lkm.yml` compiles `android16-6.12`, which takes the new uprobe
ABI with the old handler signature, and, after the DDK bump, `android17-6.18`, the first
KMI here to take both. Compiling is not exercising, and the file says as much: the 6.12+
`uh_filter()` still carries an UNVERIFIED note asking that the prototype be confirmed,
because a mismatch there is a kCFI trap rather than a build warning. Neither target has
fired a probe on a device.

## See also

- [`kernel/README.md`](../README.md) -- build modes, init order, the layer map
- [`kernel/hook/README.md`](../hook/README.md) -- the dispatcher and LSM slot patching
- [`kernel/policy/README.md`](../policy/README.md) -- allowlist and App Profiles
- [`kernel/supercall/README.md`](../supercall/README.md) -- the driver fd and ioctl table
- [`kernel/selinux/README.md`](../selinux/README.md) -- live policy editing
- [`kernel/sulog/README.md`](../sulog/README.md) -- the event queue and the reader fd
- [`uapi/README.md`](../../uapi/README.md) -- the wire format of every command named here
- [`docs/architecture.md`](../../docs/architecture.md) -- the repository-wide hub

<!-- reference links: kernel documentation and man pages -->
[bpf]: https://docs.kernel.org/bpf/index.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[execveat-2]: https://man7.org/linux/man-pages/man2/execveat.2.html
[faccessat-2]: https://man7.org/linux/man-pages/man2/faccessat.2.html
[fork-2]: https://man7.org/linux/man-pages/man2/fork.2.html
[ftrace]: https://docs.kernel.org/trace/ftrace.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[lsm]: https://docs.kernel.org/security/lsm.html
[ptrace-2]: https://man7.org/linux/man-pages/man2/ptrace.2.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[setresuid-2]: https://man7.org/linux/man-pages/man2/setresuid.2.html
[static-keys]: https://docs.kernel.org/staging/static-keys.html
[statx-2]: https://man7.org/linux/man-pages/man2/statx.2.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
[uprobetracer]: https://docs.kernel.org/trace/uprobetracer.html
