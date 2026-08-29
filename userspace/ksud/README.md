# ksud

`ksud` is the whole of KernelSU's userspace in one Rust binary. Android's init execs it from
four rc trigger stanzas covering three boot stages; the kernel substitutes it for
`/system/bin/su`; it installs modules, regenerates the init.rc fragment they inject, patches
boot images, edits the live [SELinux][selinux] policy, writes system properties the way
Magisk's `resetprop` does, and drains the kernel's su audit queue to disk. The crate
manifest is [`Cargo.toml`](Cargo.toml) and the module tree is [`main.rs`](src/main.rs); on a
desktop host almost everything behind `#[cfg(target_os = "android")]` compiles away, leaving
a five-command image patcher ([`cli_non_android.rs`](src/cli_non_android.rs)). The patchers
themselves - `boot_patch`, `lkm_image` and `lkm_image_btf` - are among the handful of
modules `main.rs` declares without a cfg, because rewriting an image is the one job that has
to work off-device as well as on it.

Five roles are worth separating, because they have different callers and different failure
modes. As the **boot-time init driver** it runs `post-fs-data`, `services` and
`boot-completed` from stanzas the kernel splices into `init.rc`, and it is what causes
modules to be mounted, by delegating to a metamodule. As the **module manager** it owns
`/data/adb/modules`, the marker-file state machine and the per-module config store. As the
**su implementation** it is entered with an `argv[0]` of `su` after the kernel has already
granted root, so its job is to shape the process, not to escalate it. As the **installer**
it patches and flashes boot images, loads the LKM into a running system, and copies itself
into place. As the **debugging CLI** it exposes the raw ioctls. There is one channel to
the kernel, an anonymous-inode descriptor named `[ksu_driver]`, and one path ksud may live
at, `/data/adb/ksud`, because [`kernel/runtime/ksud.h`](../../kernel/runtime/ksud.h)
hardcodes it as `KSUD_PATH` and [`defs.rs`](src/defs.rs) must agree.

## Three ways in

Before clap sees anything, `run()` in [`cli.rs`](src/cli.rs) inspects `argv[0]`. If it is
`su`, or if it ends in `/su`, control goes to `root_shell()` in [`su.rs`](src/su.rs). The
suffix test replaced an exact comparison against `/system/bin/su`, because argv[0] belongs
to the caller and nothing obliges it to be either of those spellings: a launcher that
resolves the command through `$PATH` and passes the resolved path, or a `su` symlink under
any other directory, produced an argv[0] that the literal match missed, and the process
then fell through to `Args::parse()` and died on an unrecognised subcommand instead of
becoming a root shell. If argv[0] ends in `resetprop` (there is a
`/data/adb/ksu/bin/resetprop` symlink back to the binary for exactly that), the whole argv
goes to `resetprop_main()` in [`resetprop.rs`](src/resetprop.rs), which never returns. Only
then does `Args::parse()` run.

The `su` identity is manufactured by the kernel. `ksu_handle_execve_sucompat()` in
[`kernel/feature/sucompat.c`](../../kernel/feature/sucompat.c) sees an allowed uid execing
`/system/bin/su`, opens `KSUD_PATH` with `O_PATH` under KernelSU's own credentials,
installs it as a temporary descriptor, and rewrites the syscall's register frame in place
so that `execve(path, argv, envp)` becomes `execveat(fd, "", argv, envp, AT_EMPTY_PATH)`,
which execs the file behind an already-open descriptor with no path lookup at all. `argv`
is untouched, which is why `argv[0]` is still whatever the caller wrote when ksud starts.
No setuid binary exists on disk, and no partially-privileged path can be swapped underneath
the caller.

The bionic that shipped in Android 17 QPR2 Beta 3 stopped issuing [`execve(2)`][execve-2]
altogether: its [`execve()`][execve-2] and `execv()` both tail-jump into `execveat(AT_FDCWD,
path, argv, envp, 0)`. A hook on `execve` alone would silently stop firing
there, so `sucompat.c` carries `ksu_handle_execveat_sucompat()` beside
`ksu_handle_execve_sucompat()`. The [`execveat`][execveat-2] handler accepts only the shape
a plain `execve()` degenerates to - directory descriptor `AT_FDCWD`, flags zero - and passes
anything else through to the real syscall untouched; both then meet in
`ksu_handle_execve_sucompat_common()`, which compares the filename against `SU_PATH` and
performs the same register rewrite. What reaches ksud is identical either way.

The third way is an ordinary subcommand, from init through the `KERNEL_SU_RC` block in
[`kernel/runtime/ksud_integration.c`](../../kernel/runtime/ksud_integration.c) or from the
Android app, which ships this binary as `libksud.so` in its `jniLibs` and shells out to it
([`KsuCli.kt`](../../manager/app/src/main/java/me/weishu/kernelsu/ui/util/KsuCli.kt)).

## Subcommands

Every `module` branch first calls `utils::switch_mnt_ns(1)`, so it works inside init's
mount namespace rather than the caller's. A mount namespace is a per-process view of the
mount table, and entering PID 1's view through `/proc/1/ns/mnt` is what makes module
directories visible to a root shell forked from an app.

| Command | What it does |
| --- | --- |
| `post-fs-data` | The main boot stage: prune, promote, relabel, sepolicy, features, module scripts, mount, `post-mount`. |
| `services` / `boot-completed` | Run every module's `service.sh` / `boot-completed.sh`, non-blocking. `services` exits 0 silently when no KernelSU is present. |
| `soft-reboot` | Re-runs the pipeline without rebooting: the `emulated-soft-reboot` stage, `stop`, post-fs-data, `start`, services, then boot-completed once `sys.boot_completed` flips back. |
| `sulogd` / `late-load [--kmi K] [--allow-shell] [--magica [PORT]] [--post-magica]` | The hidden su-log daemon body, launched by `debug sulogd`; and the loader that inserts `kernelsu.ko` into a running system and replays the boot pipeline inline. |
| `insmod <ko> [params]` | Loads any kernel module through the kallsyms-relocating loader. |
| `install [--libadbroot P] [--data-path P]` | Copies `/proc/self/exe` to `/data/adb/ksud`, extracts assets, migrates boot backups. |
| `unload` / `uninstall [--package-name P]` | Kills every descriptor holder then `delete_module("kernelsu")`; or removes modules and directories, restores boot, uninstalls the app and reboots. LKM only. |
| `module install <zip>` | Stages into `modules_update/<id>` and runs the installer script. |
| `module enable\|disable\|uninstall\|undo-uninstall <id>` | Flips a marker file, then regenerates `modules.rc`. |
| `module action <id>` / `module list` | Runs the module's `action.sh`; prints every module's `module.prop` plus state flags as JSON. |
| `module config [--internal N] get\|set\|list\|delete\|clear` | The per-module key/value store. |
| `sepolicy patch <stmts>` / `apply <file>` / `check <stmts>` | Encode and push policy edits; `check` parses only. |
| `profile get-sepolicy\|set-sepolicy <pkg>` / `get-template\|set-template\|delete-template\|list-templates` | Root-profile policy text and app-profile templates on disk. |
| `feature get <id> [--config]` / `set` / `list` / `check` / `load` / `save` | Kernel feature toggles and their persisted copy. |
| `kernel umount add\|del\|wipe` | Feeds the kernel's umount list. Intended for a metamodule's mount script. |
| `kernel nuke-ext4-sysfs <mnt>` / `notify-module-mounted` | Unregisters an ext4 mount's `/sys/fs/ext4` node; sends `EVENT_MODULE_MOUNTED`. |
| `boot-patch` / `boot-restore` | Patches a boot / init_boot / vendor_boot image or partition through its ramdisk (`--ramdisk` takes a bare cpio instead, for an AVD); restores it from backup or by rebuilding without KernelSU. |
| `boot-patch-v2 --boot I --output O [--module KO] [--force]` | Injects the LKM into the kernel `Image` itself and leaves the ramdisk untouched. Always a boot image, never init_boot or vendor_boot. |
| `boot-info current-kmi\|supported-kmis\|is-ab-device\|default-partition\|available-partitions\|slot-suffix` | Read-only queries used by the app's install screen. |
| `initrc refresh` / `resetprop` | Regenerates `modules.rc` on demand; runs the Magisk-compatible property tool (`-h` is forwarded, not eaten by clap). |
| `debug su [-g]` | Issues `KSU_IOCTL_GRANT_ROOT` and execs `sh`. This is how the app gets its root shell. |
| `debug set-manager [pkg]` / `get-sign <apk>` | Writes `ksu_debug_manager_appid` (needs `CONFIG_KSU_DEBUG`); prints the v2 certificate length and SHA-256 the kernel pins against. |
| `debug mark get\|mark\|unmark\|refresh [pid]` | Manages the per-task syscall-tracepoint mark. |
| `debug info` / `version` / `package` / `test` / `extract-binary` / `sulogd` | Kernel info, versions, asset extraction, daemon launch. |

## Talking to the kernel

[`ksucalls.rs`](src/ksucalls.rs) is the only file that touches the driver.
`scan_driver_fd()` walks `/proc/self/fd`, readlinks each entry and returns the first whose
target contains `[ksu_driver]`, covering every process the kernel armed in advance.
Otherwise `init_driver_fd()` calls `libc::syscall(SYS_reboot, KSU_INSTALL_MAGIC1,
KSU_INSTALL_MAGIC2, 0, &mut fd)` and reads the descriptor number out of the fourth argument.
[`kernel/supercall/supercall.c`](../../kernel/supercall/supercall.c) has a
[kprobe][kprobes], a dynamic breakpoint on a kernel function, planted on the
[`reboot(2)`][reboot-2] syscall wrapper. A kprobe handler runs in atomic context, where
allocating, faulting and installing descriptors are all illegal, so on seeing the magic pair
it queues a `task_work` callback that runs on the task's own return to userspace, where
`anon_inode_getfile("[ksu_driver]", ...)` and `copy_to_user` are safe. An anonymous inode is
a file with no name in any filesystem, visible only as `anon_inode:[ksu_driver]` in
`/proc/<pid>/fd`, so there is no device node to stat and no SELinux label to audit; the
syscall itself then fails harmlessly.

Every command fills a `ksu_*_cmd` struct and hands a raw pointer to `ksuctl()`. Those
structs are not written in Rust: [`build.rs`](build.rs) runs bindgen over
[`ksu_uapi.h`](src/ksu_uapi.h), a one-line file including `uapi/ksu.h`, with
`clang_args(["-x", "c++", "-I../../"])`, and [`ksu_uapi.rs`](src/ksu_uapi.rs) is an
`include!` of the result. The `-x c++` carries weight:
[`uapi/supercall.h`](../../uapi/supercall.h) declares its ioctl numbers as `static const
__u32 X = _IOR('K', 2, struct ...);`, and only as C++ are those constant expressions
bindgen can fold into `pub const` values.

Because most ioctl numbers encode a size of zero, the kernel cannot reject a stale caller
structurally. One integer is the substitute: `ensure_uapi_version_matched()` compares the
kernel's reported `uapi_version` against the compiled-in `KERNEL_SU_UAPI_VERSION` and
aborts on a difference, gating `post-fs-data`, `services`, `boot-completed`,
`soft-reboot`, `module install` and `module action` - every path that would otherwise
write a differently-shaped struct into kernel memory. `get_info()` also falls back to
`KSU_IOCTL_GET_INFO_LEGACY`, because the sized and unsized forms of that command have
different numbers.

## The /data/adb layout

Almost every path constant is built with `const_format::concatcp!` in
[`defs.rs`](src/defs.rs), inside an Android-only inner module; the desktop build sees only
the version strings. Two files extend the set the same way, keeping a constant next to its
only user: `FEATURE_CONFIG_PATH` in [`feature.rs`](src/feature.rs), and `RESETPROP_PATH`,
`BUSYBOX_PATH` and `BOOTCTL_PATH` in [`assets.rs`](src/assets.rs), all four built on
`WORKING_DIR` or `BINARY_DIR` from `defs`.

| Path | Constant | Contents |
| --- | --- | --- |
| `/data/adb/ksud` | `DAEMON_PATH` | The binary, labelled `u:object_r:ksu_file:s0`. |
| `/data/adb/ksu/` | `WORKING_DIR` | Everything ksud owns. |
| `/data/adb/ksu/bin/` | `BINARY_DIR` | Extracted assets, plus the `ksud` and `resetprop` symlinks. |
| `/data/adb/ksu/lib/libadbroot.so` | `LIBADBROOT_PATH` | The `LD_PRELOAD` payload the adb_root feature injects. |
| `/data/adb/ksu/log/` | `LOG_DIR` | `logcat.log`, `dmesg.log`, `sulog-YYYY-MM-DD[-N].log`. |
| `/data/adb/ksu/sulogd.lock` | `SULOGD_LOCK_PATH` | The `flock` that keeps exactly one sulogd alive. |
| `/data/adb/ksu/.feature_config`, `.ksurc` | `FEATURE_CONFIG_PATH` (in [`feature.rs`](src/feature.rs)), `KSURC_PATH` | Persisted feature values; the file `ENV` points at for root shells. |
| `/data/adb/ksu/profile/{selinux,templates}/` | `PROFILE_SELINUX_DIR`, `PROFILE_TEMPLATE_DIR` | Root-profile policy text and app-profile templates. |
| `/data/adb/ksu/module_configs/<id>/` | `MODULE_CONFIG_DIR` | `persist.config` and `tmp.config`. |
| `/data/adb/modules/<id>/`, `modules_update/<id>/` | `MODULE_DIR`, `MODULE_UPDATE_DIR` | The live module set, and the staging tree promoted by rename on the next boot. |
| `/data/adb/metamodule` | `METAMODULE_DIR` | Symlink to the one metamodule. |
| `/data/adb/<stage>.d/`, `/data/adb/initrc.d/*.rc` | (literals) | Global scripts and rc fragments; executable bit required. |
| `/metadata/watchdog/ksu/modules.rc` | `PREINIT_DIR_WATCHDOG` | Preferred location of the generated rc. |
| `/metadata/ksu/modules.rc` | `PREINIT_DIR_DEFAULT` | Fallback when `/metadata/watchdog` is absent. |

The generated rc has to live on `/metadata`, not `/data`, because init reads `init.rc`
long before `/data` is decrypted or even mounted, while `/metadata` is mounted early and
unencrypted. `.allowlist` also sits under `WORKING_DIR`, but the kernel owns that one.

## The boot pipeline

`on_post_data_fs()` in [`init_event.rs`](src/init_event.rs) is the longest ordered
sequence in the crate, and the order is load-bearing. It reports `EVENT_POST_FS_DATA`,
sets the umask to 0, wipes every module's temp config, and spawns two 30-second `timeout`
log captures. It then aborts outright if Magisk is installed, because two systems mounting
`/system` concurrently is a guaranteed bootloop. Safe mode is the next gate:
`utils::is_safe_mode()` ORs the `persist.sys.safemode` and `ro.sys.safemode` properties
with the kernel's volume-down counter.

Two steps sit between that check and the point where safe mode gives up. Outside safe mode
`exec_common_scripts("post-fs-data.d", true)` runs the global scripts under
`/data/adb/post-fs-data.d`, blocking, and it skips any file without the executable bit;
these belong to no module and survive every module being uninstalled, which is what makes
them the place to put a repair hook. `run_stage()` does the same for `post-mount.d`,
`service.d` and `boot-completed.d` later, and for `emulated-soft-reboot.d` and
`late-load.d` on those two paths. Then, safe mode or not, `assets::ensure_binaries(true)`
refills `/data/adb/ksu/bin`; the `true` is `ignore_if_exist`, so a file already there is
left alone and only a missing one is rewritten from the embedded copy. That call is why
`busybox` exists before anything after it needs a shell, including the module scripts
further down. Only now, if safe mode fired, does ksud disable every module and return
without running a single module script.

The rest is: promote staged modules, prune the ones marked for removal, regenerate
`modules.rc`, relabel, apply each module's `sepolicy.rule` and each root profile's policy,
apply the feature config, run the metamodule's `post-fs-data.sh` and then every module's,
load their `system.prop` files, run the metamodule's mount script, then `post-mount`.

Blocking versus non-blocking is not cosmetic. `post-fs-data`, `post-mount` and `late-load`
block because init is holding the boot at that trigger and every filesystem change must be
finished before zygote starts; `service` and `boot-completed` do not, because a module
hanging there would hang the boot with no way back. `exec_script()` in
[`module.rs`](src/module.rs) gives each script a `pre_exec` closure calling
`detach_process_group(true)` and `switch_cgroups()`, so a long-running module service
survives init reaping ksud, and that helper prefers `KSU_IOCTL_SET_INIT_PGRP` over a plain
`setpgid` because reparenting the group to init's outlives ksud's own exit.

## Module lifecycle

Module state is nothing but marker files inside `/data/adb/modules/<id>/`: `disable`,
`remove`, `update`, plus `skip_mount`, `webroot/` and `action.sh`. That is what makes the
[documented bootloop rescue](../../website/docs/guide/rescue-from-bootloop.md) possible,
since the state machine is repairable from a recovery shell with `touch` and `rm`.

Installation keeps the module's payload out of the live set. `install_module_to_system()`
does its own housekeeping before it looks at the zip: it prints the ASCII banner from
[`banner`](src/banner), pulled in with `include_str!` the same way the installer script
is, and calls `assets::ensure_binaries(false)`, whose `false` is `ignore_if_exist` and so
rewrites `/data/adb/ksu/bin` unconditionally, because the installer needs a `busybox` it
can trust rather than whatever a previous module left there. Then it extracts only
`module.prop` into memory, so id validation (`^[a-zA-Z][a-zA-Z0-9._-]+$`) and the
metamodule safety check happen before a byte of the payload reaches the disk; only once
those pass does it unpack into `/data/adb/modules_update/<id>` and run the installer.
After the installer returns it does write into `/data/adb/modules/<id>`, but only to
create the directory, copy `module.prop` in so `module list` can name the module that is
waiting on a reboot, drop the `update` marker, and - for a metamodule - point the
`/data/adb/metamodule` symlink at it. None of the module's files land there until the next
boot promotes them.

Promotion is `handle_updated_modules()`, which removes the old directory and renames the
staged one over it, keeping any `disable` or `remove` marker, so a crashing installer
cannot corrupt the running set. The installer script is
[`installer.sh`](src/installer.sh), embedded with `include_str!` and run as
`busybox sh -c`; it is Magisk `util_functions`-compatible,
exporting `MAGISK_VER=25.2` and `MAGISK_VER_CODE=25200`, sourcing `customize.sh` and
honouring `SKIPUNZIP=1`. `get_common_script_envs()` gives every module script
`ASH_STANDALONE=1`, `KSU=true`, four `KSU_*VER*` stamps, `KSU_RUNTIME_MODE` (`built-in`,
`lkm` or `late-load`), a `PATH` extended with `/data/adb/ksu/bin`, `KSU_MODULE` when the
id is known, and `KSU_LATE_LOAD=1` under late load.

`regenerate_preinit_rc()` is what lets a module contribute init.rc stanzas. It
concatenates `/data/adb/initrc.d/*.rc` (executable bit required) and then every enabled
module's `initrc/*.rc` in module-id order, writes `.modules.rc.tmp`, `sync_all`s, renames
it over `modules.rc`, and relabels the result `u:object_r:metadata_file:s0` so the
kernel's `filp_open` in init context can read it. Init parses that file as one stream, so
a torn or mislabelled write is a boot failure, hence tmp-then-rename. It scans
`modules_update/` before `modules/` so a freshly installed module wins an id collision,
and deletes any stale `modules.rc` at the other candidate directory. Every mutation path
ends with this call.

[`module_config.rs`](src/module_config.rs) is a length-prefixed binary store, magic `KSUM`
(`0x4b53554d`), version 1, at most 32 entries, keys of at most 256 bytes matching the
module-id pattern, values up to 1 MiB with no character restrictions at all - binary
framing is what makes that last part true, so a value can be multi-line JSON with no
escaping contract to get wrong. `persist.config` survives reboots; `tmp.config` is wiped
at the very start of post-fs-data, before any module script runs, giving modules a scratch
space they never have to clean up. Two keys mean something to ksud itself:
`override.description` replaces what `module list` prints, and `manage.<feature>=true`
claims a kernel feature.

## Metamodules

Mounting is not implemented in ksud at all. A metamodule owns it: a module whose
`module.prop` carries `metamodule=1` or `metamodule=true`.
[`metamodule.rs`](src/metamodule.rs) resolves it through the `/data/adb/metamodule`
symlink, falling back to a scan for the property. Exactly one may exist, because two mount
implementations racing over the same tree produces an unbootable device. Three hooks
exist: `metamount.sh` is the mount script, run near the end of post-fs-data and of late
load with `MODULE_DIR` pointing at `/data/adb/modules/`, and it must exit 0;
`metainstall.sh`, when present and the metamodule is in a stable state, is appended to
`installer.sh` and replaces the default `install_module` call, so it can still call every
helper there; `metauninstall.sh` runs once per module being pruned, with `MODULE_ID` set.

`check_install_safety()` blocks installing a regular module while the metamodule carries
an `update`, `remove` or `disable` marker and has a `metainstall.sh`: such a metamodule
has an installer whose staging layout no longer matches the mount code that will run after
the reboot. `exec_stage_script()` in [`module.rs`](src/module.rs) also skips the
metamodule when iterating regular modules, comparing `canonicalize`d paths to do it:
`/data/adb/metamodule` is a symlink into `/data/adb/modules/<id>`, and a naive comparison
would run its `post-fs-data.sh` twice.

## The su implementation

`root_shell()` in [`su.rs`](src/su.rs) runs after the kernel has already escalated the
caller, so it performs no privilege work; its job is argument compatibility and process
shape. It hand-splits argv before `getopts` sees it, locating the first `-c` and the first
genuine non-option so that `su user cmd args` and `su -c 'cmd args'` behave the way
Magisk's `su` does, and it rewrites `-mm` to `-M` and `-cn` to `-z` so getopt_long aliases
parse. That scan has to know which flags consume the word after them, or it would mistake
an option's value for the user name; `-Z` and `--context=` sit in that list next to `-g`,
`-G`, `-s`, `--group`, `--supp-group=` and `--shell=`, which is what keeps
`su -Z u:r:shell:s0 shell -c id` reading `shell` as the user.

Everything that changes the process happens in `root_shell()` itself, in the statements
between `command.args(args).arg0(arg0)` and `command.exec()`. That work used to live in a
`Command::pre_exec` closure, and moving it out cost nothing: `CommandExt::exec` replaces the
running image instead of spawning anything, so there was never a child process for the
closure to run in. What it bought is error messages. A `pre_exec` closure can only fail with
a bare `io::Error`, so the old code dropped every rustix error with `.ok()` and a rejected
[`setresuid`][setresuid-2] left the process to exec with credentials it never acquired; run
inline, each step propagates through `?` carrying its own context, and the failure names the
operation that was refused - `setgroups`, `setresgid`, `setresuid`, `setcontext` - along
with the value it was refused for. `grant_root()` is the one path in the file that still
uses a `pre_exec` closure, and it does nothing in it but switch mount namespace before
`exec()`ing `sh`.

The order is load-bearing, because `set_identity()` gives up root: the `-M` `setns` into
init's mount namespace, the cgroup writes and the tty wrapping all have to happen while the
process still holds the credentials the kernel handed it on the way in. Inside
`set_identity()` the order is supplementary groups, then gid, then uid, because
`set_thread_groups` needs `CAP_SETGID`, and dropping the uid drops that capability with it.

Two guards sit around the identity work. It runs at all only when the caller asked for an
identity - a free `user` argument, `-g`, or at least one `-G`, which is what
`identity_requested` records. A bare `su` therefore leaves the credentials the kernel built
exactly as they are, rather than calling `setgroups` with an empty list and erasing the
supplementary groups a root profile had just granted through `setup_groups()`. The other
guard is `resolve_uid()`, which returns an error for a name that is neither a passwd entry
nor a number; the code it replaced parsed the name and fell back to `0`, so
`su nosuchuser -c cmd` quietly handed out uid 0 instead of failing.

`-Z CONTEXT` (`--context`) is the last thing to run before the exec. `set_selinux_context()`
writes the string to `/proc/thread-self/attr/current`, the kernel's setcon interface, so
the image about to be exec'd runs in the named domain rather than inheriting ksud's
`u:r:ksu:s0`. Nothing in ksud decides whether that is allowed - the write goes to the
calling thread's own procfs node and the loaded policy either permits the dynamic
transition or returns an error, which arrives with the context in the message. It sits
after `set_identity()` because a domain change is not a credential change and does not need
the root credentials the identity step has already surrendered.

`wrap_tty()` is the subtle part. ksud runs in `u:r:ksu:s0` - init's rc stanzas exec it
there, and for a `su` caller `escape_with_root_profile()` in
[`kernel/policy/app_profile.c`](../../kernel/policy/app_profile.c) puts it there through
`setup_selinux()` - but descriptors 0, 1 and 2 are inherited, and they are a pts whose
*file* security id was fixed at open time and belongs to the calling app's domain, so
every later read and write on them is a denial. Rather than relabel the app's real
terminal, ksud asks the kernel for a proxy. `KSU_IOCTL_GET_WRAPPER_FD` reaches
`ksu_install_file_wrapper()` in
[`kernel/infra/file_wrapper.c`](../../kernel/infra/file_wrapper.c), which builds an
`[ksu_fdwrapper]` anonymous inode that forwards every file operation to the original,
carries `ksu_file_sid` as its own label, and spoofs `d_path` back through a `d_dname`
handler. The result is `dup2`'d over descriptors 0, 1 and 2 unless `-W` was passed.

One option has nothing to do with Magisk compatibility. `--ksu-no-new-privs` calls
`ksucalls::set_ksu_no_new_privs()`, which issues `KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT`
(`_IO('K', 21)`) before the exec. The handler is three lines,
`set_thread_flag(TIF_KSU_DISABLE_ESCAPE_WITH_ROOT)`, and that flag is bit 63 of
`thread_info->flags`, a bit the kernel itself never allocates. `escape_with_root_profile()`
tests it and refuses to escalate, so the shell you are about to exec into, and anything it
execs afterwards on the same thread, can never come back through KernelSU for a second round
of root. The choice of a thread flag rather than something in `cred` is deliberate: a flag
survives `commit_creds()` and [`execve`][execve-2], where cred state does not. Nothing
clears it. This is not `PR_SET_NO_NEW_PRIVS` and does not pretend to be - it closes only
KernelSU's own path back to root, which is the one you care about before handing a shell to
something you do not trust with a second `su`. The kernel side is documented in
[`kernel/policy/README.md`](../../kernel/policy/README.md).

## sepolicy payload encoding

[`sepolicy.rs`](src/sepolicy.rs) parses Magisk-style statements with `nom`, expands brace
groups into a cartesian product of atomic statements, and serialises all of them into one
buffer that goes out in a single `KSU_IOCTL_SET_SEPOLICY`. Each atom is `cmd` and `subcmd`
as native-endian `u32`s followed by exactly `cmd_expected_argc(cmd)` operands, and
`encode_policy_object()` writes each operand as a native-endian `u32` length, the bytes,
then a trailing NUL. `*` and "absent" both encode as length zero, and the kernel decides
per command and position whether ALL is legal there. The argc table is duplicated verbatim
as `sepol_expected_argc()` in [`kernel/selinux/rules.c`](../../kernel/selinux/rules.c),
and since the operand count is never on the wire, a mismatch desynchronises the
receiver's cursor and the batch is rejected.

Batching rather than one ioctl per rule is a performance and atomicity decision. Each
`SET_SEPOLICY` makes the kernel duplicate the entire policydb, publish the copy with
`rcu_assign_pointer`, wait a full [RCU][whatisrcu] grace period so no reader can still be
walking the old one, and flush the access-vector cache. Per rule, for a hundred-line
`sepolicy.rule`, that would add seconds to every boot and leave the system observably
half-patched in between. The ioctl returns the count accepted, and `apply_rules_batch` warns
when that is short. `sepolicy check` parses in strict mode and applies nothing, which is
what `check_sepolicy` in [`installer.sh`](src/installer.sh) calls at install time.

## The boot image patcher

[`docs/boot-patching.md`](../../docs/boot-patching.md) covers both patch strategies end to end,
including the boundary with the out-of-tree `android-bootimg` crate; this section is the
summary.

[`boot_patch.rs`](src/boot_patch.rs) parses and repacks images in process with the
`android-bootimg` crate; there is no `magiskboot` and no external tool. `patch()` resolves
the KMI and the target (an explicit `--boot`, or a partition path derived from the slot
suffix), mmaps it, rejects Android header versions below 3, extracts the ramdisk cpio, and
performs the substitution that makes the LKM scheme work:

```rust
let is_kernelsu_patched = cpio.exists("kernelsu.ko");
if !is_kernelsu_patched && cpio.exists("init") {
    cpio.mv("init", "init.real")?;
}
cpio.add("init", CpioEntry::regular(0o755, ksu_init))?;
cpio.add("kernelsu.ko", CpioEntry::regular(0o755, kernelsu_ko))?;
```

The `is_kernelsu_patched` guard makes re-patching idempotent; without it each pass would
bury the stock init one level deeper. The new `/init` is `ksuinit`, which becomes PID 1,
loads the module and hands control on. Module parameters cannot be changed after
[`init_module`][init-module-2], so `--allow-shell` and `--no-custom-rc` become tokens in a
`ksu_config` cpio entry (`allow_shell=1`, `norc=1`) that
[`ksuinit/src/init.rs`](../ksuinit/src/init.rs) reads and passes through verbatim.

Two details of writing the result back matter. The source mmap is dropped explicitly
before the flash, because some kernels reject a write to a block device that is still
mapped. And `flash_partition()` first issues `BLKROSET` with a value of 0: Android marks
boot partitions read-only at the block layer, so `open(O_WRONLY)` succeeds but writes
return `EROFS`.

KMI detection has two implementations. `get_current_kmi()` parses `uname -r` and falls
back to `modinfo` over any `.ko` under `/vendor/lib/modules`, because many OEM kernels
drop the `androidNN` token from `uname` while the vendor modules still carry it in
`vermagic`. The other is the free function `parse_kmi()`, which slides a window over a
decompressed kernel image looking for a `<major>.<minor>...androidNN` string, and it is
not a host-only path: `--ota` reaches for it first, through `parse_kmi_from_boot()` on the
*other* slot's `/dev/block/by-name/boot<suffix>`, since the running kernel's KMI says
nothing about the slot being patched. An explicit `--boot` or `--kernel` falls through to
it as well, as does any `uname` that yields nothing usable.
`choose_boot_partition()` prefers `init_boot` unless the kernel is being replaced or the
KMI starts with `android12-`, since `init_boot` only arrived with Android 13.

With `--backup`, or on any first-time `--flash`, ksud computes the SHA-1 of the source
partition, copies the raw bytes to `/data/adb/ksu/ksu_backup_<sha1>` (or to the app's
`boot_backup` directory for `install --data-path` to migrate later, when `/data/adb` is
not yet writable), and records the hash as a `stock_image.sha1` cpio entry. `restore()`
uses that entry to find the byte-exact stock image, the only thing guaranteed to pass AVB
and vbmeta checks, and rebuilds without KernelSU only when the backup is missing.

Two options exist for inputs that are not whole boot images. `--ramdisk` parses the input
as a bare cpio through `BootImage::parse_raw_ramdisk()` rather than as a boot image, which
is how an AVD's ramdisk gets patched; it demands an explicit `--kmi`, since there is no
kernel in the file to auto-detect one from, and it refuses to combine with `--kernel` or
`--flash`. On a desktop host `--arch` (aarch64 by default) selects which embedded subtree
the `ksuinit` binary and the `<kmi>_kernelsu.ko` come from, because a host build embeds
both architectures rather than the one it is running on.

## Injecting the LKM into the kernel image

`boot-patch` needs a ramdisk to hijack. `boot-patch-v2` needs only the kernel:
[`lkm_image.rs`](src/lkm_image.rs) unpacks the boot image, decompresses the kernel,
rewrites the bare ARM64 `Image` behind it, and repacks it in whatever format the source
image used, leaving the ramdisk byte-identical. `parse_arm64_image_size()` demands the
ARM64 `Image` magic at offset 0x38, so a kernel that does not decompress to a bare `Image`
is refused before a byte is patched. Nothing renames `init`, nothing writes a
`kernelsu.ko` into the cpio, and there is no `ksuinit` in the boot path at all.
`patch_boot()` is the entry point and it is reachable from both front ends, since
`lkm_image` carries no cfg. Omit `--module` and it reads the KMI out of the decompressed
kernel with `boot_patch::parse_kmi()` and uses the matching embedded module; `--force` is
required before an existing output is overwritten, and an output that canonicalises to
either input is refused outright. The result is written to a temporary file in the output
directory, given the source image's permissions, and renamed into place. The scheme is
AArch64-only and deliberately has no `--arch`: `recover_arm64_kernel_metadata()` refuses
anything without the ARM64 `Image` magic, the bootstrap is AArch64 assembly, and
`embedded_module_name()` prepends `aarch64/` unconditionally off-device, so the x86_64 GKI
modules `boot-patch --arch x86_64` can reach are out of reach here.

Everything downstream needs the target kernel's symbol addresses, and the target kernel is
not the running one, so `/proc/kallsyms` is no help. `recover_arm64_kernel_metadata()`
reconstructs the table out of the image bytes instead. A kernel stores kallsyms compressed:
a 256-entry token table of short byte sequences, symbol names encoded as indices into it, a
marker array, and an offset array interpreted relative to a base address. The recovery
finds candidate token tables, decodes names against each, and then tries both address
layouts - the pre-6.4 one where the offsets sit below `kallsyms_num_syms`, and the layout
from 6.4 on where they follow the token index - keeping whichever decodes to addresses
that fall inside the image. Ambiguity is refused rather than guessed at: if more than one
candidate survives, the error reports how many token tables and how many complete
candidates were found and names `CONFIG_KALLSYMS_ALL`, which the scheme requires.

BTF breaks that tie when the image has it, and supplies an ABI detail besides. A GKI vmlinux
embeds a BTF blob describing every kernel type, and
[`lkm_image_btf.rs`](src/lkm_image_btf.rs) parses it far enough to locate `struct load_info`
and read back its size and the byte offsets of its `hdr` and `len` members. Those three
numbers are the entire dependency on the kernel's private module-loader ABI, and they move
between releases; reading them out of the image is what lets one ksud patch several kernel
series. Built-in [BPF][bpf] skeletons can embed BTF blobs of their own, so the parser hands
back every candidate and `validate_kallsyms_btf_boundaries()` keeps the one whose file range
is bracketed by the kallsyms symbols, which is the vmlinux blob. Without BTF the built-in
`GKI_ABI` constants stand in - `hdr` at 16, `len` at 24, 256 bytes of stack storage,
`GFP_KERNEL` as `0xcc0`. The release check is not part of that fallback. `recover_gki_abi()`
runs on every image, BTF or not: it reads the release string out of `linux_banner` and
refuses anything outside the validated 5.10, 5.15, 6.1, 6.6 and 6.12 series rather than
patching a kernel whose layout nobody checked. BTF supplies the three `load_info` numbers,
not licence to guess at the rest, which is why an android17-6.18 boot image is rejected here
even though the DDK build produces a module for it.

The module rides along in a capsule appended past `_end`. `build_capsule()` writes a 96-byte
header - the magic `KSULKM1`, a version, the header length, the capsule length, the module's
offset and size, the fixup table's offset and count, a flag word, and the module's SHA-256 -
then the `kernelsu.ko` ELF and the fixup table, padded so the image ends on a 4 KiB
boundary, and finally rewrites `image_size` at offset 0x10 of the ARM64 header so the
bootloader loads the whole thing as one image. The fixup table is what
`collect_module_fixups()` produced: every [`SHN_UNDEF`][elf-5] symbol in the module,
resolved against the recovered map, stored as a 16-byte pair of the `Elf64_Sym`'s file
offset and the target's distance from `_text`. Distances rather than addresses, because
KASLR slides the kernel at boot and only the delta from `_text` survives that. A symbol the
map cannot resolve is not fatal - it is reported as a "native resolver symbol" and left for
the module's own loader.

Three `BL` instructions carry the whole scheme, and `analyze_patch_sites()` locates them by
decoding instructions within the enclosing function rather than by any fixed offset: the
`async_synchronize_full()` call inside `kernel_init()`, the `strndup_user()` call inside
`load_module()`, and the `memblock_reserve()` call inside `arm64_memblock_init()` that
reserves the kernel image. Each of the first two must resolve to exactly one match or the
patch aborts. `arm64_memblock_init()` calls `memblock_reserve()` more than once, so
`find_kernel_image_memblock_reserve_call()` picks the right one semantically, by requiring
the two instructions before the branch to be the `SUB` pair that builds the size into `x1`
and the start into `x0` from the same kernel-start register. The same pass recovers
`PAGE_OFFSET` by decoding the `ORR` immediate that `arm64_memblock_init` uses to construct
it, and insists the result be a page-aligned high-half value.

The code those branches jump to is [`lkm_image_bootstrap.S`](src/lkm_image_bootstrap.S),
assembled at build time and relocated into the image by a small ET_REL linker inside
`lkm_image.rs`. It lands in the tail of the permanent text: `find_text_tail_cave()` walks
back from `_etext` across proven-zero padding, rejects any range that a symbol other than a
section boundary falls inside, and returns that hole. Linking rather than hand-patching is
what lets the bootstrap name its dependencies as ordinary symbols -
`ksu_ext_memblock_reserve`, `ksu_ext_memstart_addr`, `ksu_ext_kimage_voffset`,
`ksu_ext_vmalloc`, `ksu_ext_memcpy`, `ksu_ext_load_module`, `ksu_ext_kstrdup`,
`ksu_ext_strndup_user` and the rest - and have them resolved to the recovered addresses,
while its internal branches and `ADRP` references stay PC-relative and therefore survive
KASLR.

At boot the three patches fire in order. `ksu_memblock_reserve_wrapper` adds the capsule's
length to the size argument and tail-calls the real `memblock_reserve()`, so the appended
bytes are reserved along with the kernel image and never reach the page allocator.
`ksu_bootstrap`, entered where `kernel_init()` would have branched to
`async_synchronize_full()`, performs the call it displaced first so nothing upstream
notices, then computes the capsule's address from `_text`, `kimage_voffset`,
`memstart_addr` and `PAGE_OFFSET`. It has to go through the linear map rather than the
kernel image mapping, because the capsule sits beyond the link-time `_end` and the image
mapping does not cover it. Every header field is then compared against a value baked into
the bootstrap at patch time, so a truncated or mismatched capsule stops there rather than
being fed to `load_module()`. Only after that does it `vmalloc` a copy of the module,
`memcpy` it in, walk the fixup table setting each `Elf64_Sym`'s `st_shndx` to `SHN_ABS` and
`st_value` to `_text` plus the recorded distance, build a zeroed `struct load_info` on the
stack with only `hdr` and `len` seeded, and call `load_module()`.

`ksu_strndup_user_adapter` covers the one thing that call would otherwise get wrong.
`load_module()` copies its module-parameter string in from userspace with
`strndup_user()`, and this early in `kernel_init()` there is no userspace to copy from. The
adapter compares the incoming pointer against the kernel-resident empty string the
bootstrap passed, `kstrdup`s it when they match, and tail-calls the original
`strndup_user()` when they do not - which is why every later module load, `insmod`
included, keeps working unchanged.

## Late load and unload

[`late_load.rs`](src/late_load.rs) is the path for a device whose boot image cannot be
replaced. It daemonises, checks `ksuinit::has_kernelsu()`, pulls `<kmi>_kernelsu.ko` from
the embedded assets, and calls `ksuinit::load_module()`, which relocates the module's
undefined symbols against `/proc/kallsyms` before `init_module`; GKI kernels export only a
curated symbol set, so an ordinary insmod fails with "Unknown symbol". A second recovery
sits beside that relocation: if `init_module` fails for any reason, `load_module()`
re-reads the `/dev/kmsg` records it just produced, looks for the kernel's
`version magic '...' should be '...'` complaint, rewrites the module's own `vermagic=`
string in place and retries once, so one prebuilt `.ko` still loads on a kernel whose
version magic drifted from the KMI it was built for. `--kmi` overrides the `uname`-based
detection when the running kernel does not name its KMI, and `--allow-shell` reaches
`init_module` as the module parameter `allow_shell=1` - the same switch
`boot-patch --allow-shell` bakes into `ksu_config` for the ramdisk path. Then it replays
the boot pipeline inline: install, promote, prune, relabel, sepolicy, features, the
`late-load` stage, `system.prop`, the metamodule mount script, `post-mount`, `service`,
`boot-completed`, and a restart of the manager app so it picks up a fresh driver
descriptor. `post-fs-data` is not replayed, because re-running it on a live filesystem
would be destructive. One ordering detail is worth repeating: `reset_std()` runs *after*
the module loads, because sending descriptors over binder is denied while their file
security id is still `u:r:su:s0`.

A variant entry lives in [`magica.rs`](src/magica.rs), reached as
`late-load --magica [PORT]`. It exists because some exploits yield a process with uid 0
but the wrong SELinux domain and a trimmed capability set: enough to write the
property-area mapping, not enough to `init_module` and keep working. It sets
`ro.debuggable=1` and `ro.adb.secure=0` directly in that mapping (`ro.*` is enforced only
by `property_service`, not by the mapping),
restarts `adbd` in root mode on a TCP port, and re-invokes `ksud late-load --post-magica`
over that connection, which lands in a usable domain. `--post-magica` is what makes the
inner load revert the property edits afterwards.

Going the other way is harder than it looks. `delete_module` fails while the module's
reference count is non-zero, and every open `[ksu_driver]` or `[ksu_fdwrapper]` file holds
a reference through `f_op->owner`. So [`unload.rs`](src/unload.rs) stops Android's
services, SIGKILLs every process whose `/proc/<pid>/attr/current` is exactly `u:r:ksu:s0`
and every process holding one of those descriptors, closes its own, calls
`delete_module("kernelsu")`, and starts services again. It does kill the manager, sulogd
and every root shell but the caller.

## Features and the su-log daemon

[`feature.rs`](src/feature.rs) mirrors `enum ksu_feature_id` from
[`uapi/feature.h`](../../uapi/feature.h) as `FeatureId` and persists values in
`/data/adb/ksu/.feature_config` (magic `0x7f4b5355`, version 1). Features live in the
kernel but must survive a reboot, so something has to replay them; `init_features()` does,
before any module script runs. First it asks `module::get_managed_features()` which
features a module claimed with `manage.<feature>=true` and *removes* those from the map,
because a module whose whole job is to own, say, `kernel_umount` would otherwise fight the
saved config every boot; `feature set` likewise refuses to change a claimed feature unless
the caller's `KSU_MODULE` names a claiming module.

Seven features exist and the numbering has a deliberate hole in it. `su_compat`,
`kernel_umount`, `sulog`, `adb_root`, `selinux_hide` and `webview_zygote_umount` are 0
through 5, allocated upstream; `mount_hide` is 16, because
[`uapi/feature.h`](../../uapi/feature.h) reserves everything from 16 up for features this
fork adds. The reservation is not tidiness. A feature id is a wire value in three places at
once - the manager sends it, the kernel switches on it, and `.feature_config` stores it as
a raw `u32` - so a fork-local feature squatting on a low number is a rebase away from
colliding with whatever upstream allocates next, and the collision is silent: a saved config
would come back enabling a different feature than it recorded. That is exactly what
happened to `mount_hide`, which held 5 until upstream took it for
`webview_zygote_umount`. `FeatureId`, `from_u32`, `name`, `description`, `parse_feature_id`
and the `all_features` arrays that `list_features()` and `save_config()` walk all carry
every variant, so the sparse id behaves like the dense ones under `feature list` and
`feature save`.

`webview_zygote_umount` and `mount_hide` read like the same idea and are not; they are
complementary, and running both is normal. Unmounting changes what is mounted: the kernel
drops module mounts for uid 1053 and the isolated processes it forks, so the files are
genuinely absent from that view. Filtering changes what is reported: `mount_hide` leaves
the mounts in place and blanks the matching records in
`/proc/<pid>/{mounts,mountinfo,mountstats}`. Its reader test is fixed once at `open()` and
reads `(is_appuid || is_isolated) && (is_isolated || ksu_uid_should_umount)`, so an
isolated process is filtered unconditionally while an ordinary app is filtered only when
the umount policy already says it should not see modules; root, shell and the manager keep
the real view. `is_isolated` is the shared `is_isolated_process()` from
[`kernel/policy/allowlist.h`](../../kernel/policy/allowlist.h), which spans both the
app-zygote range [90000, 98999] and the regular one [99000, 99999], so the umount path and
the filter never disagree about who counts as isolated. A probe that stats a path inside
its own namespace is answered only by the unmount; a probe that reads another process's
mountinfo out of init's global namespace is answered only by the filter.

[`sulog.rs`](src/sulog.rs) drains the kernel's su audit queue. `ensure_sulogd_running()`
is called whenever the `sulog` feature is set non-zero, so it has to be idempotent without
a pid file that could go stale; a `flock` on `/data/adb/ksu/sulogd.lock` is the entire
mechanism. A session opens `[ksu_sulog]` through `KSU_IOCTL_GET_SULOG_FD`
([`kernel/sulog/fd.c`](../../kernel/sulog/fd.c)), sets `O_NONBLOCK` and epolls it for
`EPOLLIN | EPOLLERR | EPOLLHUP`; the hangup half is what lets a single thread notice the
module being unloaded and restart cleanly instead of blocking forever in `read`. Frames
are a 24-byte header plus payload, and a `record_type` of `u16::MAX` marks a synthetic
*dropped* record, which the kernel emits instead of blocking a traced task when its ring
overruns. Records become escaped `key=value` lines in daily files under
`/data/adb/ksu/log/`, with retention and size limits read from the module-config store
under `internal.ksud.sulogd`.

## resetprop, and the rest

Magisk's property tool is folded into the binary rather than shipped beside it:
[`resetprop.rs`](src/resetprop.rs) is a clap front end over the `prop-rs-android` crate,
reached through the `bin/resetprop` symlink or the `resetprop` subcommand. The flag
surface is Magisk's: `-n`, `-p`, `-P`, `-d`, `-v`, `-w` with `--timeout`, `-f`,
`-c`/`--rebuild`, `-Z`, `--force`. Every in-process use, from
module `system.prop` files to the `sys.boot_completed` manipulation in `soft_reboot`,
builds `ResetProp { skip_svc: true, .. }`, because `ro.*` properties are read-only to
`property_service` by contract and the only way a module's `system.prop` can set
`ro.product.model` is to write the property-area mapping directly.

Reading a property goes the other way, through `utils::getprop()`, which declares bionic's
`__system_property_find()` and `__system_property_read_callback()` and copies the value out
of the callback into an owned `String`. It used to call the `android-properties` crate,
whose bionic backend built an empty `CString` and handed that one-byte allocation to
`__system_property_get()`, which writes up to `PROP_VALUE_MAX` bytes into whatever it is
given. Reading a property as short as `sys.boot_completed` therefore scribbled past the
allocation; Scudo's size-class slack usually swallowed it, but an allocator with guard
padding caught the overwrite on free and aborted the process, which surfaced as module
installs failing with exit status 135. The callback API takes no caller-supplied buffer at
all, so there is nothing left to size wrongly.

Four smaller files round it out: [`restorecon.rs`](src/restorecon.rs) (SELinux xattr
helpers), [`profile.rs`](src/profile.rs) (the on-disk half of App Profiles),
[`apk_sign.rs`](src/apk_sign.rs) (the APK v2 signature block behind `get-sign`) and
[`debug.rs`](src/debug.rs) (the developer-only commands).

## Embedded assets

[`assets.rs`](src/assets.rs) uses `rust-embed` with a folder chosen by cfg: `bin/aarch64`
on aarch64 Android, `bin/x86_64` on x86_64 Android, and the whole `bin` directory on a
desktop host, which is why a host build embeds both architectures and addresses its assets
by a prefixed name - `aarch64/android14-6.1_kernelsu.ko` rather than
`android14-6.1_kernelsu.ko`. That prefix is what `boot-patch --arch` selects and what
`lkm_image::embedded_module_name()` prepends off-device, and it also means host
`supported-kmis` prints the arch along with the KMI. `ensure_binaries()` writes every
embedded file to `/data/adb/ksu/bin/` mode 0755 with two deliberate exclusions, anything
named `ksuinit` and anything ending in `.ko`, because those are only ever consumed in
memory (written into a cpio, or fed to `init_module`) and a `kernelsu.ko` left in a
world-readable directory would be a free detection signal. `list_supported_kmi()` derives
the KMI list by stripping the `_kernelsu.ko` suffix from embedded names, which is what
`boot-info supported-kmis` prints.

Only `busybox` and `bootctl` are committed to [`bin/`](bin/); `bin/.gitignore` excludes
`**/*.ko` and `**/ksuinit`, and CI copies the per-KMI modules and the `ksuinit` binary in
before `cargo build` ([`ksud.yml`](../../.github/workflows/ksud.yml)), so building ksud
before those artifacts exist yields a binary whose `list_supported_kmi()` is empty. The
`compression` feature on `rust-embed` caches the compressed blob, so swapping a file under
`bin/` without a clean build can embed the stale one. And `utils::install()` in
[`utils.rs`](src/utils.rs) copies `/proc/self/exe` rather than a resolved path: resolving
it would make `/data/adb/ksud install` delete the very file it is executing.

## Building and linting

The workspace root is [`Cargo.toml`](../../Cargo.toml); ksud and
[`ksuinit`](../ksuinit/README.md) are both members and both default members. Cross
compilation needs an Android NDK: generate `.cargo/config.toml` once with
[`setup_cargo_config.py`](../../scripts/setup_cargo_config.py) (it will not overwrite an
existing file without `--force`), or copy
[`config.example.toml`](../../.cargo/config.example.toml) and fill in the paths by hand.

```sh
python3 scripts/setup_cargo_config.py
cargo build --target aarch64-linux-android --release
cargo build --target x86_64-linux-android --release

cargo fmt --all --check                                  # rustfmt gate
cargo clippy --target aarch64-linux-android --release     # clippy gate
cargo clippy --target x86_64-linux-android --release
```

Those last three are the lint gates that run on every push touching `userspace/**`. CI
does not use the generated cargo config: it sources
[`setup-rust-build.sh`](../../.github/scripts/setup-rust-build.sh), which exports the
per-triple `CC_*`, `AR_*`, `CARGO_TARGET_*_LINKER` and `BINDGEN_EXTRA_CLANG_ARGS_*`
variables from `ANDROID_NDK_HOME` at API level 26. The [`justfile`](../../justfile) offers
a third route through `cross`: `just build_ksud`, `just build_manager` (which also copies
ksud into the app's jniLibs and builds the APK) and `just clippy`.

Clippy runs with `RUSTFLAGS=-Dwarnings`, and [`main.rs`](src/main.rs) already sets
`#![deny(clippy::all, clippy::pedantic)]` plus `#![warn(clippy::nursery)]`, so a new lint
is a build failure rather than a note. Version stamps come from git in
[`build.rs`](build.rs): `VERSION_CODE` is `30000 + git rev-list --count HEAD`, the same
formula `kernel/Kbuild` and the manager's root `build.gradle.kts` use, so a shallow clone
yields a version that disagrees with the kernel's.

One build-time variable has no default worth trusting in this fork. `DEFAULT_PACKAGE_NAME`
in [`defs.rs`](src/defs.rs) is `env!("KSU_PACKAGE_NAME")`, and [`build.rs`](build.rs) fills
that in with `me.weishu.kernelsu` when the variable is unset, while the manager here builds
as `org.matrix.su` ([`manager/app/build.gradle.kts`](../../manager/app/build.gradle.kts)).
A plain `cargo build` therefore yields a ksud whose `--package-name` defaults,
`debug package` output and late-load manager restart all name the upstream app; export
`KSU_PACKAGE_NAME=org.matrix.su` to match the manager that is actually installed.

[`build.rs`](build.rs) has a second job that is easy to trip over. `lkm_image.rs` embeds
the assembled bootstrap with `include_bytes!`, unconditionally, so **every** ksud build
needs an AArch64 assembler - the x86_64 Android target and a desktop host included, since
the object is data rather than code the host executes. `assemble_bootstrap()` tries, in
order: a prebuilt object named by `KSU_LKM_BOOTSTRAP_OBJECT`; a
`userspace/ksud/.lkm_image_bootstrap.o` prepared beside the manifest, which is how the
`cross` CI job hands one in from the host before entering the container; a compiler named
by `KSU_LKM_BOOTSTRAP_CC`; `aarch64-linux-gnu-gcc`; the NDK's clang found by walking
`$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/*/bin/`; plain `clang` with
`--target=aarch64-linux-gnu`; and finally `llvm-mc`. Whatever produces the object,
`validate_bootstrap_object()` checks the ELF header for a little-endian AArch64 ELF64
`ET_REL` before it is used, so a wrong-architecture object fails the build instead of being
linked into an image that will not boot. Bindgen still runs only for the Android targets.

## See also

- [`userspace/README.md`](../README.md) - the Rust workspace as a whole
- [`userspace/ksuinit/README.md`](../ksuinit/README.md) - the ramdisk init shim and LKM loader
- [`uapi/README.md`](../../uapi/README.md) - the ioctl numbers and structs bindgen turns into `ksu_uapi`
- [`kernel/supercall/README.md`](../../kernel/supercall/README.md) - the other end of every ioctl here
- [`kernel/runtime/README.md`](../../kernel/runtime/README.md) - init.rc splicing and the boot events
- [`kernel/selinux/README.md`](../../kernel/selinux/README.md) - what a sepolicy batch does once it lands
- [`kernel/sulog/README.md`](../../kernel/sulog/README.md) - the producer behind `sulogd`
- [`kernel/policy/README.md`](../../kernel/policy/README.md) - allowlist, app profiles, feature registry
- [`manager/README.md`](../../manager/README.md) - the Android app that ships and drives this binary
- [`docs/boot-patching.md`](../../docs/boot-patching.md) - both boot-patch strategies in full
- [`docs/architecture.md`](../../docs/architecture.md) - repository-wide map

<!-- reference links: kernel documentation and man pages -->
[bpf]: https://docs.kernel.org/bpf/index.html
[elf-5]: https://man7.org/linux/man-pages/man5/elf.5.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[execveat-2]: https://man7.org/linux/man-pages/man2/execveat.2.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[setresuid-2]: https://man7.org/linux/man-pages/man2/setresuid.2.html
[whatisrcu]: https://docs.kernel.org/RCU/whatisRCU.html
