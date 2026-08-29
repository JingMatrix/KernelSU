# Boot pipeline and the ksud handoff

`kernel/runtime/` watches Android start up. KernelSU needs a root process in its own
[SELinux][selinux] domain running at a precise point in early boot, on a device whose
`/system` and `/vendor` it must not modify, and it has no daemon of its own to start; the
answer is to make Android's init start one, by editing init.rc in flight.
[`ksud_integration.c`](ksud_integration.c) holds the mechanism: the syscall hooks that do
that splicing, the exec inspector that recognises, on either [`execve`][execve-2] or
[`execveat`][execveat-2], the two boot milestones the kernel can see for itself, and the
volume-down safe-mode detector. [`boot_event.c`](boot_event.c) holds the policy: three
`on_*` callbacks that turn "a boot stage happened" into side effects, the two global flags
the rest of the tree gates on, and one function that has nothing to do with boot at all. The
headers split the same way, [`ksud.h`](ksud.h) for the mechanism, `KSUD_PATH` and both exec
entry points, [`ksud_boot.h`](ksud_boot.h) for the milestones, the flags and that outlier.
Both objects are unconditional in [`Kbuild`](../Kbuild).

## Android's boot, as far as this code cares

The kernel execs `/init` as PID 1. On an LKM install that binary is
[`ksuinit`](../../userspace/ksuinit/src/init.rs), which inserts `kernelsu.ko` and relinks
`/init` to the real init before returning; an image patched by `ksud boot-patch-v2` gets
there earlier still, loading the module from a bootstrap inside `kernel_init()` and
leaving `/init` untouched. Either way the module is live before Android's init runs at
all. Init's first stage mounts `/dev`, `/proc` and `/sys`, sets up the root filesystem and
loads the SELinux policy, then re-execs itself as `/system/bin/init second_stage`. The
policy live from that point on is the one first stage loaded, and it has no `ksu` type in
it.

Second stage reads `/system/etc/init/hw/init.rc` and turns it into a table of triggers and
actions. Four triggers matter here: `post-fs-data`, which fires once `/data` is mounted
and usable; `nonencrypted` and `property:vold.decrypt=trigger_restart_framework`, the two
paths by which the framework is allowed to start and where Android brings up the
`late_start` service class; and `property:sys.boot_completed=1`. An `exec` inside a
trigger is synchronous, so init waits for the child to exit, which is what lets KernelSU
mount its modules before zygote starts. Zygote is `/system/bin/app_process` invoked with
`-Xzygote`, and its exec is the last event the kernel can observe unaided.

| Boot point | What kernel/runtime does |
| --- | --- |
| module init (PID 1 or built-in) | `ksu_ksud_init()`: hook `__NR_read` and `__NR_fstat`, arm the `input_event` [kprobe][kprobes] |
| `/system/bin/init second_stage` | snapshot the SELinux status page for selinux_hide, patch the sepolicy, cache SIDs, relabel `ksu_cred` into `u:r:ksu:s0` |
| second stage reads `/system/etc/init/hw/init.rc` | proxy that one `struct file`, unhook read/fstat, append `KERNEL_SU_RC` plus `modules.rc` at EOF |
| init runs `ksud post-fs-data` | ksud reports `EVENT_POST_FS_DATA`, which runs `on_post_fs_data()` |
| first `app_process -Xzygote` | `on_post_fs_data()` again, a no-op once the rc path has run, then disarm `ksud_execve_key` |
| `sys.boot_completed=1` -> `ksud boot-completed` | `on_boot_completed()`: set the flag, prune the allowlist |

## Splicing init.rc

The rc injection uses the coarser of the two hooking APIs in
[`syscall_hook.h`](../hook/syscall_hook.h). `ksu_register_syscall_hook()` routes a syscall
through the tracepoint-driven dispatcher, so it fires only for tasks carrying the
syscall-tracepoint work flag; `ksu_syscall_table_hook()` overwrites `sys_call_table[nr]`
and fires for every task on the system. `ksu_ksud_init()` uses the latter for `__NR_read`
and `__NR_fstat`, and drops both again through `ksu_syscall_table_unhook()` as soon as
init.rc has been recognised, so the system-wide cost lasts only until the first init.rc
read. Both installed functions have the raw entry signature
`long (*)(const struct pt_regs *)` and reach their arguments through
[`arch.h`](../include/arch.h).

`ksu_sys_read()` pulls the fd out of `PT_REGS_PARM1` and calls `ksu_handle_sys_read()`,
which `fget()`s it, hands the `struct file` to `ksu_install_rc_hook()` and `fput()`s it
again. `ksu_install_rc_hook()` asks `is_init_rc()` four questions: is `current->comm`
exactly `"init"`, is the dentry a regular file, is its short name `init.rc`, and does
`d_path()` render as `/system/etc/init/hw/init.rc`. (The nearby comment still says
`/system/etc/init/init.rc`; the comment is stale.) On the first match a function-scope
`static bool rc_hooked` inside `ksu_install_rc_hook()` latches, `stop_init_rc_hook()`
removes both table hooks, and the proxy goes in.

`file->f_op` points into read-only kernel data, so it cannot be edited in place. The code
copies the whole `struct file_operations` into the file-scope `fops_proxy`, saves
`orig_read` and `orig_read_iter`, overwrites those two slots (each only if the original
was non-NULL, since most filesystems implement `read_iter` and leave `read` NULL), and
repoints `file->f_op` at the copy. That confines the change to init's single open fd. It
also means exactly one file can ever be proxied: `fops_proxy`, `ksu_rc_pos` and
`module_rc_pos` really are file-scope singletons, and the one-shot `rc_hooked` latch is
what keeps a second file from claiming them.

Both proxies append only at end of file:

```c
    ret = orig_read(file, buf, count, pos);
    if (ret != 0) {
        return ret;
    }
```

Init parses init.rc as one stream; splicing text into the middle would cut a stanza in
half. Waiting for `orig_read` to return 0 puts the injected text strictly after the
original, where it registers as the last action for each trigger it names. The two proxies
differ in one easy-to-miss way: `copy_to_user()` returns the bytes it could *not* copy
while `copy_to_iter()` returns the bytes it *did*, so one treats non-zero as failure and
the other treats zero as failure.

The injected text is a compile-time string built from `KERNEL_SU_DOMAIN`
([`selinux.h`](../selinux/selinux.h)) and `KSUD_PATH`:

```c
    "on post-fs-data\n"
    "    start logd\n"
    // We should wait for the post-fs-data finish
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " post-fs-data\n"
```

`start logd` comes first so `catch_bootlog()` in
[`init_event.rs`](../../userspace/ksud/src/init_event.rs) has a logd to talk to. Three
more stanzas follow: `nonencrypted` and `vold.decrypt=trigger_restart_framework` exec
`ksud services`, and `sys.boot_completed=1` execs `ksud boot-completed`.

The `services` stage sitting between post-fs-data and boot-completed is the one with no
kernel counterpart at all. `on_services()` checks the UAPI version and then does exactly
one thing, `run_stage("service", false)`, which walks `/data/adb/service.d` running every
file that carries the executable bit, then the metamodule's `service.sh`, then each
enabled module's `service.sh`. It sends no `EVENT_*` report, so no code in
`kernel/runtime/` learns that the stage happened. The `false` is the `block` argument, and
it is the whole difference between the two script hooks a module author gets:
`post-fs-data.sh` runs with `block` true inside init's synchronous `exec` and holds up the
boot until it returns, while `service.sh` is started and abandoned, in parallel with
everything else `late_start` is bringing up.

`__NR_fstat` is hooked for a second reason. Android's init reads init.rc through libbase's
`ReadFdToString`, which fstats the fd to size its buffer and then reads until `read()`
returns 0. `ksu_sys_fstat()` recognises the same file, lets the real syscall run, then
rewrites `st_size` at `statbuf + offsetof(struct stat, st_size)` to `size + ksu_rc_len +
module_rc_len`, through `copy_from_user_nofault()` and `copy_to_user_nofault()` so that a
non-resident page fails the write instead of faulting inside a syscall hook. Both hooks
are removed together in `stop_init_rc_hook()`; unhooking only read would leave every
`fstat` on the device routed through `ksu_sys_fstat` for the rest of the boot.

## modules.rc and ksu_no_custom_rc

Modules can contribute their own init.rc stanzas, but the text has to exist before init
parses init.rc, and that parse happens long before post-fs-data gives ksud a chance to
produce it. The text is therefore generated one boot in advance.
`regenerate_preinit_rc()` in [`module.rs`](../../userspace/ksud/src/module.rs)
concatenates `/data/adb/initrc.d/*.rc` (only files carrying the executable bit, used as an
on/off switch) followed by every enabled module's `initrc/*.rc`, renames the result over
`modules.rc` and labels it `u:object_r:metadata_file:s0`. The destination is
`/metadata/watchdog/ksu/` when `/metadata/watchdog` is a directory and `/metadata/ksu/`
otherwise; the copy at the losing path is deleted so the kernel's two-step probe cannot
pick up a stale file.

`load_module_rc_once()` reads it back with the same two paths in the same order, under
`override_creds(ksu_cred)`. That wrap is needed because the read executes in init's
context under whatever domain init holds at that instant, while `ksu_cred` is the module's
own credential, already transitioned into `u:r:ksu:s0`. It is called from both
`ksu_sys_fstat()` and `ksu_install_rc_hook()`, because the fstat path needs
`module_rc_len` to inflate `st_size` and init fstats before it reads.

The whole path can be switched off. `ksu_no_custom_rc` is declared in
[`ksu.h`](../include/ksu.h) and bound in [`init.c`](../core/init.c) as
`module_param_named(norc, ksu_no_custom_rc, bool, 0)`. The permission is 0, so no sysfs
attribute is created and the only way to set it is the parameter string handed to
[`init_module(2)`][init-module-2]: `ksud boot-patch --no-custom-rc`
([`boot_patch.rs`](../../userspace/ksud/src/boot_patch.rs)) writes `norc=1` into the
ramdisk cpio entry `ksu_config`, which ksuinit passes through verbatim. There is no
runtime toggle.

That switch reaches the kernel only on an image patched through the ramdisk. The newer
`ksud boot-patch-v2` ([`lkm_image.rs`](../../userspace/ksud/src/lkm_image.rs)) appends
`kernelsu.ko` to the kernel Image as a capsule and splices a bootstrap into a text cave,
and that bootstrap calls `load_module()` itself with `ksu_empty_args`, an empty parameter
string. No ksuinit runs and no `ksu_config` entry exists on that path, so an image patched
that way always comes up with `ksu_no_custom_rc` false and always splices modules.rc.

Which of the two patch paths a device can take is settled by its architecture.
`boot-patch-v2` reads the arm64 Image header for the kernel's size and reconstructs a
kallsyms table out of the decompressed Image, choosing between candidate decodings by
requiring that the `__start_BTF`/`__stop_BTF` pair one of them yields really delimits a
vmlinux BTF blob in the same Image -- built-in [BPF][bpf] skeletons embed BTF of their own,
so without that cross-check the match is ambiguous. The selected blob then hands the
injector the `struct load_info` field offsets the bootstrap needs, which are compiled-in
defaults otherwise. From the recovered table it locates `kernel_init`, `arm64_memblock_init`
and `load_module`, and links an aarch64 bootstrap
([`lkm_image_bootstrap.S`](../../userspace/ksud/src/lkm_image_bootstrap.S)) against those
addresses, so it cannot patch an image for any other instruction set. An x86_64 target -- an
AVD ramdisk patched on a host with `ksud boot-patch --ramdisk --arch x86_64` -- goes through
ksuinit instead, and ksuinit is the half of the pipeline that reads `ksu_config`. The `norc`
switch is therefore reachable on x86_64 and unreachable on an arm64 image patched with
`boot-patch-v2`.

## exec inspection: second stage and the first zygote

Two syscalls can carry a new program image into a process, and the dispatcher watches both.
`ksu_syscall_hook_manager_init()`, in
[`syscall_hook_manager.c`](../hook/syscall_hook_manager.c), registers `ksu_hook_execve()`
for `__NR_execve` and `ksu_hook_execveat()` for `__NR_execveat`; both handlers live in
[`syscall_event_bridge.c`](../hook/syscall_event_bridge.c) and are one-line wrappers over
`ksu_hook_execve_common()`, which carries a `bool execveat` and shifts every register
argument by one when it is set, because [`execveat(2)`][execveat-2] spends its first slot on
a dirfd. Listening on `__NR_execve` alone stopped being enough: bionic in Android 17 QPR2
Beta 3 compiles both [`execve()`][execve-2] and `execv()` down to `__execveat(AT_FDCWD,
path, argv, envp, 0)`, so on such a build every exec on the device -- second stage, zygote,
ksud itself -- arrives as `__NR_execveat`, and a kernel watching only the other number
observes nothing at all. The register shift is not the ksud inspector's problem alone.
Everything else that reads the exec arguments has to move with it, so the same `execveat`
flag picks `ksu_adb_root_handle_execveat()` over `ksu_adb_root_handle_execve()` and
`ksu_handle_execveat_sucompat()` over `ksu_handle_execve_sucompat()` inside the same
function.

The first thing `ksu_hook_execve_common()` does is
`if (static_branch_unlikely(&ksud_execve_key))`, dispatching to `ksu_execve_hook_ksud()` or
`ksu_execveat_hook_ksud()` for the number that actually arrived. `ksud_execve_key` is a
`DEFINE_STATIC_KEY_TRUE`, so the branch is patched in from module load and becomes a
no-op once it is patched out. Those two entry points differ only in which registers they
read. Execve keeps the filename in `PT_REGS_PARM1` and argv in `PT_REGS_PARM2`; execveat
holds them one slot later, in `PT_REGS_PARM2` and `PT_REGS_PARM3`. Both hand the pair to
`ksu_execve_hook_ksud_common()`, which runs the filename through `untagged_addr()` (arm64
top-byte-ignore lets userspace hand the kernel tagged pointers), copies at most 32 bytes of
it, and wraps argv in a `struct user_arg_ptr` -- a private type from `fs/exec.c` re-declared
here along with `count()` and `get_user_arg_ptr()`, including the CONFIG_COMPAT variant.
Because the filename now arrives as the pointer itself rather than the address of a
register slot, the `if (!filename_user)` guard finally has something to catch.

`ksu_handle_execveat_ksud()` then does two prefix comparisons. On `/system/bin/init` with
`argv[1] == "second_stage"` it runs, in this exact order,
`ksu_selinux_hide_handle_second_stage()` ([`selinux_hide.c`](../feature/selinux_hide.c)),
`apply_kernelsu_rules()` ([`rules.c`](../selinux/rules.c)), then `cache_sid()` and
`setup_ksu_cred()` ([`selinux.c`](../selinux/selinux.c)). `apply_kernelsu_rules()` has to
run here and not earlier, because the policy it edits is the one first stage loaded and
the `u:r:ksu:s0` label in the injected rc does not exist until that edit adds the type.
`cache_sid()` and `setup_ksu_cred()` resolve context strings against the just-patched
policy, so they follow it; `ksu_selinux_hide_handle_second_stage()` precedes it because
`apply_kernelsu_rules()` reaches `selinux_status_update_policyload()` and rewrites the
real status page selinux_hide wants to snapshot first.

On `/system/bin/app_process` with `argv[1] == "-Xzygote"`, and only the first time, it
calls `on_post_fs_data()` and then `ksu_stop_ksud_execve_hook()`, which does
`static_branch_disable(&ksud_execve_key)`. Zygote's exec is the belt-and-braces trigger
for post-fs-data: by then `/data` is certainly mounted and decrypted, so
`ksu_load_allow_list()` will find `/data/adb/ksu/.allowlist` even if the injected rc never
ran.

Init reaches the dispatcher at all only because it is marked.
`ksu_mark_running_process_locked()` in [`tp_marker.c`](../hook/tp_marker.c) always sets the
syscall-tracepoint flag on PID 1, and the flag is inherited across [`fork`][fork-2], so
init's forked child that execs `app_process` arrives already marked.

## Getting ksud running

Init execs `/data/adb/ksud post-fs-data` with the seclabel `u:r:ksu:s0` supplied by the
`exec` form. On that same exec, whichever of the two syscalls carried it,
`ksu_handle_init_mark_tracker()`, gated on `current->pid != 1 && is_init(current_cred())`,
compares the target against `KSUD_PATH` and calls `escape_to_root_for_init()`
([`app_profile.c`](../policy/app_profile.c)) on a match. The same function clears the
[tracepoint][tracepoints] flag for any exec that is not ksud, `app_process`, `adbd` or
`stub_zygote`, which keeps the marked set small.

ksud then needs a channel into the kernel, and there is no device node or procfs entry to
open. `reboot_handler_pre()` in [`supercall.c`](../supercall/supercall.c) [kprobes][kprobes]
the [`reboot`][reboot-2] syscall wrapper, recognises the magic pair in the first two
arguments, and queues a `task_work` that creates the `[ksu_driver]` anon-inode fd and writes
its number back through the syscall's fourth argument. Over that fd,
`ksucalls::report_post_fs_data()` and its siblings
([`ksucalls.rs`](../../userspace/ksud/src/ksucalls.rs)) issue `KSU_IOCTL_REPORT_EVENT`.
Every stage entry point first calls `ensure_uapi_version_matched()` and abandons the stage
on a mismatch.

## The three milestones

| Event | id | Kernel effect |
| --- | --- | --- |
| `EVENT_POST_FS_DATA` | 1 | load the allowlist, start the packages.list watcher, stop the input hook, hand selinux_hide its post-fs-data transition |
| `EVENT_BOOT_COMPLETED` | 2 | set `ksu_boot_completed`, `track_throne(true)`, drop `backup_sepolicy` if unused |
| `EVENT_MODULE_MOUNTED` | 3 | set `ksu_module_mounted` |

`on_post_fs_data()` carries a `static bool done` guard, because it has two callers: the
zygote exec hook and `do_report_event()` in [`dispatch.c`](../supercall/dispatch.c). The
guard matters most for `ksu_observer_init()`
([`pkg_observer.c`](../manager/pkg_observer.c)), which is not idempotent: it allocates a
new `fsnotify_group` and adds an inode mark, so a second call leaks both.

`on_boot_completed()` sets `ksu_boot_completed` *before* calling `track_throne(true)`
([`throne_tracker.c`](../manager/throne_tracker.c)), and that order is load-bearing.
`ksu_prune_allowlist()` in [`allowlist.c`](../policy/allowlist.c) returns early while
`ksu_boot_completed` is false, so reversing the two lines would silently disable the first
prune of the boot.

On the ioctl side there is a second layer of guards: `do_report_event()` adds
function-static once-flags for `EVENT_POST_FS_DATA` and `EVENT_BOOT_COMPLETED` and
short-circuits both when `ksu_late_loaded` is set; `KSU_IOCTL_REPORT_EVENT` is
`only_root`. `EVENT_MODULE_MOUNTED` has neither guard, because module mounting is done by
an external metamodule script at an arbitrary time, including under late load. No code
path in this tree sends it on its own: the only producer is the `ksud kernel
notify-module-mounted` command ([`cli.rs`](../../userspace/ksud/src/cli.rs)), which a
metamodule's mount script is expected to invoke. Until it does, `ksu_handle_umount()` in
[`kernel_umount.c`](../feature/kernel_umount.c) returns 0 for every app fork, since
`ksu_module_mounted` is its first gate. [`mount_hide.c`](../feature/mount_hide.c) does not
consult the flag at all.

One function in [`boot_event.c`](boot_event.c) shares the subject of module mounts but
belongs to no boot stage. `nuke_ext4_sysfs()` takes a mount point, resolves it with
`kern_path()`, rejects anything whose superblock's `s_type->name` is not `"ext4"`, and
otherwise calls `ext4_unregister_sysfs()` on that `struct super_block`; both ways out of a
successful lookup `path_put()` the reference `kern_path()` took. Mounting an ext4 image
makes ext4 publish a directory of tuning and statistics files for it under `/sys/fs/ext4/`,
named after the backing device, and unregistering removes that directory, so a mounted
module image stops advertising itself there. Nothing in the boot path calls this; the only
route in is
`KSU_IOCTL_NUKE_EXT4_SYSFS` ([`supercall.h`](../../uapi/supercall.h)), dispatched to
`do_nuke_ext4_sysfs()` in [`dispatch.c`](../supercall/dispatch.c) behind
`manager_or_root`, and issued from userspace as `ksud kernel nuke-ext4-sysfs <mnt>`. The
call is also the reason `CONFIG_KSU` depends on `EXT4_FS` in [`Kconfig`](../Kconfig):
`ext4_unregister_sysfs()` is not exported to modules, so `kernelsu.ko` reaches the device
with that symbol still [`SHN_UNDEF`][elf-5] and whichever loader runs has to bind it. On the
ramdisk path ksuinit looks it up in `/proc/kallsyms` and rewrites the `Elf64_Sym` before
[`init_module`][init-module-2]; on the `boot-patch-v2` path `collect_module_fixups()`
([`lkm_image.rs`](../../userspace/ksud/src/lkm_image.rs)) resolves it at patch time from
the kallsyms table recovered out of the Image and ships the answer in the capsule, and the
bootstrap writes it in just before calling `load_module()`
([`../README.md`](../README.md)).

## Safe mode

`ksu_ksud_init()` registers a kprobe on `input_event`. Because that is an ordinary C
function and not a syscall, the handler reads its fourth argument through
`PT_REGS_CCALL_PARM4` rather than `PT_REGS_SYSCALL_PARM4`: on x86_64 the fourth C argument
is in `rcx` while the fourth syscall argument is in `r10`.
`ksu_handle_input_handle_event()` counts `EV_KEY` / `KEY_VOLUMEDOWN` events with a
non-zero value (presses, not releases) and calls `ksu_stop_input_hook_runtime()` as soon
as the count reaches three.

`ksu_is_safe_mode()` latches a positive answer, returns `false` immediately when
`ksu_late_loaded`, and otherwise stops the hook before reporting whether the count reached
three. `ksu_stop_input_hook_runtime()` does not unregister inline: it `schedule_work()`s
`do_stop_input_hook`, because `unregister_kprobe()` sleeps and synchronizes and must not
be called from a probe handler. Its `static bool input_hook_stopped` latch carries the
same weight as the deferral. Three callers reach it -- the probe handler once the third
press lands, `ksu_is_safe_mode()`, and `on_post_fs_data()` -- and once the work item has
already run, a second `schedule_work()` would queue a second `unregister_kprobe()` on a
kprobe that is no longer registered. The hook is also torn down unconditionally at
post-fs-data; leaving it armed would cost a probe hit on every input event and let a user
drop into safe mode hours after boot.

Userspace ORs the kernel's answer with two properties: `is_safe_mode()` in
[`utils.rs`](../../userspace/ksud/src/utils.rs) checks `persist.sys.safemode` and
`ro.sys.safemode` first, and only then issues `KSU_IOCTL_CHECK_SAFEMODE` (`always_allow`).
In safe mode `on_post_data_fs()` skips `post-fs-data.d`, calls `disable_all_modules()` and
returns early, and `run_stage()` refuses every later stage.

## Late load

`ksu_late_loaded` is derived in [`init.c`](../core/init.c) as `current->pid != 1` under
`#ifdef MODULE`, and hardcoded false for a built-in build. ksuinit asserts
`getpid().is_init()` before calling `init_module`, so a boot-time LKM comes up with the
flag clear, and so does a boot-patch-v2 image, whose bootstrap replaces the
`async_synchronize_full()` call inside `kernel_init()` and therefore also loads the module
as PID 1; [`late_load.rs`](../../userspace/ksud/src/late_load.rs) daemonizes first, so
`ksud late-load` comes up with it set.

The late-load branch of `kernelsu_init()` never calls `ksu_ksud_init()`. There are no
read/fstat table hooks, no init.rc splicing and no input kprobe, because every event they
wait for has already passed unobserved: init.rc was parsed, second stage ran, `/data` was
mounted and zygote has been up for a while. `kernelsu_init()` does the equivalent work
synchronously instead -- `apply_kernelsu_rules()`, `cache_sid()`, `setup_ksu_cred()`,
`escape_to_root_for_init()`, `ksu_allowlist_init()`, `ksu_load_allow_list()`,
`ksu_throne_tracker_init()`, `ksu_observer_init()`, `ksu_boot_completed = true`,
`track_throne(false)`, and `setenforce(true)` if ksud had left SELinux permissive.
`escape_to_root_for_init()` runs before that `setenforce(true)` so the calling ksud does
not lose `/data/app` the instant enforcement resumes. `kernelsu_exit()` correspondingly
calls `ksu_ksud_exit()` only under `if (!ksu_late_loaded)`; calling it unconditionally
would `unregister_kprobe()` a kprobe that was never registered.

Two things about this mode deserve stating plainly. The exec hooks are *not* skipped: the
dispatcher registrations for `__NR_execve` and `__NR_execveat` both happen in
`ksu_syscall_hook_manager_init()`, which runs on both branches, and `ksud_execve_key`
starts armed. If zygote is restarted later (a `stop; start`, a crash, `ksud soft-reboot`),
`ksu_handle_execveat_ksud()` takes its `first_zygote` branch and calls
`on_post_fs_data()`, which under late load has never run
and so is not stopped by its `done` guard: that re-enters `ksu_observer_init()` and
`schedule_work()`s a `struct work_struct` whose `INIT_WORK()` lives in the skipped
`ksu_ksud_init()`. And `ksu_is_safe_mode()` returns false unconditionally, so the
volume-down rescue does not exist in late-load mode; `persist.sys.safemode` and
`ro.sys.safemode` are the only way in.

## Teardown

`ksu_ksud_exit()` unregisters the input kprobe and frees `module_rc_buf`. It does not call
`stop_init_rc_hook()`; that call is commented out with a stale TODO naming a `vfs_read_kp`
that no longer exists. The read/fstat table entries are restored anyway by
`ksu_syscall_hook_exit()`, which walks the `hooked_entries[]` array that
`ksu_syscall_table_hook()` filled and runs inside `ksu_syscall_hook_manager_exit()`, the
first thing `kernelsu_exit()` calls. `ksu_syscall_hook_exit()` has one implementation per
architecture, [`arm64/syscall_hook.c`](../hook/arm64/syscall_hook.c) and
[`x86_64/syscall_hook.c`](../hook/x86_64/syscall_hook.c), and [`Kbuild`](../Kbuild)
compiles exactly one of them according to `CONFIG_ARM64` or `CONFIG_X86_64`. Both keep the
same sixteen-slot table behind `hooked_entries_lock` and restore it the same way, so
nothing in this file has to know which one was linked in.

One failure mode has no teardown at all. If init.rc is never seen, on a recovery boot or a
device whose init reads it from elsewhere, `ksu_install_rc_hook()` never latches and the
`__NR_read` and `__NR_fstat` table hooks stay installed for the whole session.

## See also

- [`../README.md`](../README.md) - build modes, init order, layer map
- [`../core/README.md`](../core/README.md) - `kernelsu_init()` and its two branches
- [`../hook/README.md`](../hook/README.md) - dispatcher, table patching, task marking
- [`../supercall/README.md`](../supercall/README.md) - the ioctl plane and the driver fd
- [`../../docs/boot-patching.md`](../../docs/boot-patching.md) - how the module reaches the kernel at all
- [`../selinux/README.md`](../selinux/README.md) - `apply_kernelsu_rules()`, `ksu` domain
- [`../manager/README.md`](../manager/README.md) - `track_throne()`, the packages watcher
- [`../policy/README.md`](../policy/README.md) - the allowlist and what pruning needs
- [`../feature/README.md`](../feature/README.md) - kernel_umount, mount_hide, selinux_hide
- [`../../uapi/README.md`](../../uapi/README.md) - `EVENT_*` ids and the version handshake
- [`../../userspace/ksud/README.md`](../../userspace/ksud/README.md) - the other half
- [`../../userspace/ksuinit/README.md`](../../userspace/ksuinit/README.md) - the shim
- [`../../docs/architecture.md`](../../docs/architecture.md) - end-to-end flows

<!-- reference links: kernel documentation and man pages -->
[bpf]: https://docs.kernel.org/bpf/index.html
[elf-5]: https://man7.org/linux/man-pages/man5/elf.5.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[execveat-2]: https://man7.org/linux/man-pages/man2/execveat.2.html
[fork-2]: https://man7.org/linux/man-pages/man2/fork.2.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
