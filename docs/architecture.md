# KernelSU architecture

KernelSU grants root by moving the decision into the kernel. There is no setuid binary holding
the privilege and no daemon that hands it out on request: a kernel module keeps the list of
which application ids may become root, what a root-granted process is allowed to do, and which
processes must not see that any of it is happening. Everything in userspace is a client of that
module.

Three artifacts make up a running installation, and they meet at exactly one place:

- **[`kernel/`](../kernel/README.md)** - a Linux kernel module, `kernelsu.ko`, either compiled
  into the kernel image or loaded as an out-of-tree module. It owns the policy and does all the
  hooking.
- **[`userspace/ksud/`](../userspace/ksud/README.md)** - a Rust binary at `/data/adb/ksud`. It
  drives the boot sequence, installs and mounts modules, implements `su`, and doubles as the
  command-line interface to the whole system.
- **[`manager/`](../manager/README.md)** - an Android app. It is the only client the kernel
  authenticates by identity rather than by uid, and it is how a human edits policy.

The meeting point is a single [ioctl][ioctl-2] channel described in
[`uapi/`](../uapi/README.md), called the *supercall*. If you read nothing else before touching
this codebase, read that header directory and
[`kernel/supercall/`](../kernel/supercall/README.md).

The rest of this document, in order:

- [The pieces](#the-pieces) - one row per directory, each linking to its own README.
- [The one channel](#the-one-channel) - how userspace gets a descriptor, and what it may ask.
- [Two ways the module gets into the kernel](#two-ways-the-module-gets-into-the-kernel) - and
  what the second way gives up.
- [From power-on to a rooted device](#from-power-on-to-a-rooted-device) - the boot sequence.
- [What happens when an app asks for root](#what-happens-when-an-app-asks-for-root) - where
  the credential change actually happens.
- [Keeping modules out of sight](#keeping-modules-out-of-sight) - unmount, filter, spoof.
- [Where this fork differs from upstream](#where-this-fork-differs-from-upstream).
- [Rules that cut across every layer](#rules-that-cut-across-every-layer) - read this before
  adding a feature.
- [Where to start reading](#where-to-start-reading).

## The pieces

| Path | What it is |
| --- | --- |
| [`kernel/`](../kernel/README.md) | The module: build modes, init order, and the layer map |
| [`kernel/core/`](../kernel/core/README.md) | `module_init` / `module_exit` and the order everything comes up in |
| [`kernel/hook/`](../kernel/hook/README.md) | [LSM][lsm] hooks, [kprobes][kprobes], [tracepoints][tracepoints] and syscall-table patching |
| [`kernel/supercall/`](../kernel/supercall/README.md) | The ioctl control plane and its permission classes |
| [`kernel/policy/`](../kernel/policy/README.md) | Allowlist, app profiles, feature flags |
| [`kernel/infra/`](../kernel/infra/README.md) | Symbol resolution, file wrapper, event queue, [seccomp][seccomp-filter] cache, `su` [mount namespaces][mount-namespaces-7] |
| [`kernel/manager/`](../kernel/manager/README.md) | Recognising the manager APK and tracking its uid |
| [`kernel/runtime/`](../kernel/runtime/README.md) | The boot pipeline and the handoff to ksud |
| [`kernel/selinux/`](../kernel/selinux/README.md) | Editing the live policydb and installing the ksu domain |
| [`kernel/sulog/`](../kernel/sulog/README.md) | The audit trail for root grants |
| [`kernel/feature/`](../kernel/feature/README.md) | Everything user-visible, built on the layers above |
| [`uapi/`](../uapi/README.md) | The frozen kernel/userspace ABI |
| [`userspace/`](../userspace/README.md) | The Rust workspace |
| [`userspace/ksud/`](../userspace/ksud/README.md) | The daemon, installer and CLI |
| [`userspace/ksuinit/`](../userspace/ksuinit/README.md) | The ramdisk shim that loads the module before init runs |
| [`manager/`](../manager/README.md) | The Android app and its JNI bridge |
| [`scripts/`](../scripts/README.md) | Build automation, packaging, CI |
| [`website/`](../website/README.md) | The user-facing documentation site |

## The one channel

Userspace cannot open a device node for KernelSU, because there isn't one. There is no
`/dev/kernelsu` to find in a directory listing and no new syscall number to spot in a
seccomp policy. Instead the module watches the [`reboot`][reboot-2] syscall through a
[kprobe][kprobes] registered in [`supercall/supercall.c`](../kernel/supercall/supercall.c),
and a caller that passes two magic values gets a file descriptor back:

```c
reboot(KSU_INSTALL_MAGIC1 /* 0xDEADBEEF */, KSU_INSTALL_MAGIC2 /* 0xCAFEBABE */, 0, &fd);
```

The kprobe cannot install the descriptor itself. It runs from a breakpoint exception with
interrupts in an unknown state, where allocating a file and touching userspace memory is not
allowed. So `reboot_handler_pre()` queues a `task_work` item, which the kernel runs on the way
back out to userspace, on the calling thread, where `anon_inode_getfile()`, `fd_install()` and
`copy_to_user()` are all safe. That deferral is the whole trick, and it is worth understanding
before adding anything to this path.

The resulting file is an anonymous inode named `[ksu_driver]`, which is what
[`ksucalls.rs`](../userspace/ksud/src/ksucalls.rs) scans `/proc/self/fd` for before falling back
to the magic `reboot`. A second name, `[ksu_driver_su]`, appears on the descriptor the kernel
hands a `su` exec after applying its root profile; it authorizes two commands its domain might
otherwise be refused, and the scan prefers it when both are present. That scan is not defensive: the manager is normally handed a descriptor
without asking for one. [`hook/setuid_hook.c`](../kernel/hook/setuid_hook.c) installs it the
moment zygote calls [`setresuid`][setresuid-2] to the manager's uid, and in the same breath
caches `__NR_reboot` in that process's seccomp filter, which is what keeps the fallback usable
at all. Deciding which uid is the manager's belongs to
[`kernel/manager/`](../kernel/manager/README.md).

Every command is an ioctl on that descriptor, dispatched through a table in
[`supercall/dispatch.c`](../kernel/supercall/dispatch.c) where each entry names a handler and a
permission class: `always_allow`, `only_root`, `only_manager`, `manager_or_root`, or
`allowed_for_su`. The classes live in [`supercall/perm.c`](../kernel/supercall/perm.c) and are
three lines each; read them before assuming what a command is protected by. A caller the class
rejects gets `-EPERM` and the handler never runs.

Two more descriptors exist, and both are obtained *through* that first one: the sulog reader
(`KSU_IOCTL_GET_SULOG_FD`) and the file wrapper (`KSU_IOCTL_GET_WRAPPER_FD`).

## Two ways the module gets into the kernel

**Compiled in.** `CONFIG_KSU=y` in [`kernel/Kconfig`](../kernel/Kconfig), the module's init runs
as part of kernel startup, and `ksu_late_loaded` is false. This is what a kernel maintainer
shipping KernelSU in their source tree produces.

**Loaded as an LKM.** The module is built out-of-tree against a GKI kernel and inserted at
runtime. There are two sub-cases, and the module distinguishes them with one line in
[`core/init.c`](../kernel/core/init.c):

```c
ksu_late_loaded = (current->pid != 1);
```

If [`ksuinit`](../userspace/ksuinit/README.md) has been patched into the boot ramdisk in
place of `/init`, it is pid 1 when it calls [`init_module`][init-module-2], so
`ksu_late_loaded` stays false and the module comes up before Android's init has run at all -
early enough to intercept init reading its own `init.rc`. If instead `ksud insmod` loads the
module on an already-booted system, the caller is not pid 1, `ksu_late_loaded` is true, and
`kernelsu_init()` takes the branch described in
[`kernel/core/README.md`](../kernel/core/README.md#the-late-load-branch).

What that branch gives up is one call, `ksu_ksud_init()`, and everything it would have
registered: the `input_event` kprobe and the direct `read` and `fstat` syscall-table hooks. So a
late-loaded module cannot inject anything into `init.rc`, and `ksu_is_safe_mode()` returns false
outright instead of counting key presses. The `reboot` kprobe is not part of that trade -
`ksu_supercalls_init()` arms it before the branch, on both paths.

In exchange the branch does at once what a boot event would otherwise have driven later: it
applies its [SELinux][selinux] rules, escalates the loading process so it keeps working once
SELinux is enforcing again, and calls `track_throne(false)` - the pass in
[`kernel/manager/`](../kernel/manager/README.md) that parses `/data/system/packages.list` and
resolves the manager's uid - rather than waiting for a boot event that will never arrive.

That single comparison is why the same `.ko` behaves differently depending on who inserts it.

## From power-on to a rooted device

1. `kernelsu_init()` in [`core/init.c`](../kernel/core/init.c) resolves kernel symbols, installs
   the syscall hooks, registers the feature handlers, and arms the `reboot` kprobe.
2. Android's init starts and reads `/system/etc/init/hw/init.rc`. The module is watching:
   [`runtime/ksud_integration.c`](../kernel/runtime/ksud_integration.c) recognises the first
   read of that exact path by a task named `init`, and because `file_operations` live in
   read-only memory, it swaps in a *proxy* `file_operations` on the open file rather than
   patching the original. The proxy waits for the read that returns 0 - init reads to EOF - and
   appends two blobs there: KernelSU's own rc content, then whatever
   `/metadata/watchdog/ksu/modules.rc` holds, or `/metadata/ksu/modules.rc` when the first path
   is absent. Loading the module with `norc` suppresses the second blob and nothing else.
   Android parses the result as if it had always been in the file.
3. That rc content starts `ksud`, which reports `EVENT_POST_FS_DATA` back through the supercall.
   The module loads the allowlist from disk and starts watching for the manager package.
4. ksud mounts modules from `/data/adb/modules/` and reports `EVENT_MODULE_MOUNTED`, which sets
   `ksu_module_mounted`. Until that flag is set, the kernel unmount path does nothing, because
   there is nothing to unmount.
5. At `EVENT_BOOT_COMPLETED`, [`runtime/boot_event.c`](../kernel/runtime/boot_event.c) runs
   `track_throne(true)` to settle the manager's uid. The `true` means prune only: the package
   observer started in step 3 has been running the full discovery pass on every
   `packages.list` write since then.

Safe mode short-circuits this. The module counts volume-down presses through a kprobe on the
input layer; at three presses `ksu_is_safe_mode()` returns true, ksud skips every module, and
the device boots clean. That is the recovery path when a module bootloops the phone, and it is
one of the two things a late-loaded module does not have - see
[Two ways the module gets into the kernel](#two-ways-the-module-gets-into-the-kernel).

## What happens when an app asks for root

The credential change happens in the kernel. `escape_with_root_profile()` in
[`policy/app_profile.c`](../kernel/policy/app_profile.c) is the single place it happens, and it
is where the app profile turns into uid, gid, supplementary groups, capability sets, SELinux
context and - through [`infra/su_mount_ns.c`](../kernel/infra/su_mount_ns.c) - the mount
namespace the profile asks for. It refuses a caller that is already euid 0. Nothing in
userspace grants anything: `su::root_shell()` in [`su.rs`](../userspace/ksud/src/su.rs) opens
with the comment `we are root now, this was set in kernel!` and proceeds to argument parsing.

Two routes reach that function.

### Through `su`

[`feature/sucompat.c`](../kernel/feature/sucompat.c) intercepts four syscalls and compares
their path argument against `/system/bin/su`. Nothing is installed there and the file does not
need to exist. Two conditions have to hold before the path is examined at all, and a third
decides whether a match does anything:

- The `KSU_FEATURE_SU_COMPAT` flag is on. It defaults to on; the tests are in
  [`hook/syscall_event_bridge.c`](../kernel/hook/syscall_event_bridge.c), and with the flag off
  every one of these calls goes straight to the original syscall.
- `ksu_is_allow_uid_for_current()` already admits the caller. An uninvited uid never reaches
  the comparison, so `su` is not merely refused for it - it is invisible.
- `su_target_path()` has somewhere to send it. That is `/data/adb/ksud`, and only when ksud
  exists: requiring it is what stops a chrooted process, which cannot see it, from being
  redirected. When ksud is absent the redirect is dropped and the syscall runs untouched, with
  one exception - a module loaded with `allow_shell` (the default in `CONFIG_KSU_DEBUG` builds)
  sends uid 2000, and only uid 2000, to `/system/bin/sh`.

What a match then does depends on which syscall it was:

| Syscall | Mechanism | Escalates? |
| --- | --- | --- |
| [`execve`][execve-2], [`execveat`][execveat-2] | Opens the target `O_PATH` into a temporary descriptor and re-enters as `execveat(fd, "", argv, envp, AT_EMPTY_PATH)` | Yes |
| [`faccessat`][faccessat-2], [`newfstatat`][stat-2] | Swings `*filename_user` at a copy of the target path, runs the original syscall, restores the pointer | No |

`execveat` is taken up only when it is being used as a plain `execve` - dirfd `AT_FDCWD` and
zero flags. Anything else reaches the original syscall before the path is even read.

The exec path never rewrites `argv`, so the target sees the arguments the caller passed,
`argv[0]` included. `escape_with_root_profile()` runs after the registers are rewritten and
before the re-entry, which is why the new binary is already privileged at its first
instruction. If the re-entered syscall fails, the original registers are restored and the
temporary descriptor is closed.

The stat-family path escalates nothing; it only makes `su` answer as though it were installed.
The replacement path it points at is written below the caller's own stack pointer, so no
mapping has to be created to hold it.

ksud, running as root, finds `argv[0]` is `su` or ends in `/su`, and enters `su::root_shell()`.

### Through the manager

The app's JNI bridge in [`ksu.cc`](../manager/app/src/main/cpp/ksu.cc) issues nine commands,
and not one of them grants root. All nine read or edit policy: the allowlist, app profiles,
feature flags, the safe-mode check, the per-uid umount decision, the version handshake. For
work that needs privilege the app builds a shell instead. `createRootShell()` in
[`KsuCli.kt`](../manager/app/src/main/java/me/weishu/kernelsu/ui/util/KsuCli.kt) spawns the
manager's own bundled ksud as `libksud.so debug su`, falling back to a plain `su` - which is
the first route - and then to a plain `sh`.

`ksud debug su` is the only caller of `KSU_IOCTL_GRANT_ROOT` in this tree. That ioctl exists
for a caller that is already running and wants to be escalated in place, rather than across an
`execve`: `allowed_for_su()` gates it, so the caller must be the manager or hold an allowlisted
uid, and its handler calls the same `escape_with_root_profile()` before ksud execs a shell of
its own.

### What is recorded

A record goes into the [sulog](../kernel/sulog/README.md) when a grant is attempted, but only
while the `KSU_FEATURE_SULOG` flag is on. It defaults to off: until the manager turns it on
`ksu_sulog_capture()` returns NULL and every emitter downstream of it is a no-op.

What is logged is a request that got past the permission gate, with the `retval` field
recording whether `escape_with_root_profile()` then succeeded or failed. A request the gate
rejects leaves no trace at all - the ioctl returns `-EPERM` before its handler runs, and a uid
the allowlist does not admit never reaches the `su` interception in the first place.

## Keeping modules out of sight

Three mechanisms, three different layers, and they are not interchangeable:

- **Unmounting** ([`feature/kernel_umount.c`](../kernel/feature/kernel_umount.c)) removes module
  mounts from a process's mount namespace as it drops privilege in `setresuid`. It only works
  where the process has a private namespace to modify, and it stays inert until
  `ksu_module_mounted` is set at step 4 of
  [the boot sequence](#from-power-on-to-a-rooted-device).
- **Filtering** ([`feature/mount_hide.c`](../kernel/feature/mount_hide.c)) leaves the mounts
  alone and edits what `/proc/<pid>/{mountinfo,mounts,mountstats}` *prints*, keyed on the reader.
  It works in the global namespace, where unmounting would be catastrophic. It is deliberately
  *not* gated on `ksu_module_mounted`: module mounts can be established outside KernelSU's own
  path, and the filter only ever drops records that match a marker or the umount list, so
  running it when there is nothing to hide removes nothing.
- **Spoofing** ([`feature/selinux_hide.c`](../kernel/feature/selinux_hide.c)) sanitises what
  `/sys/fs/selinux` reports, so the policy edits the module made are not visible as policy edits.

`ksu_uid_should_umount()` in [`policy/allowlist.c`](../kernel/policy/allowlist.c) is the shared
decision point for the first two. It excludes the manager and su-granted apps, and it is where
the per-app profile setting from [`kernel/policy/`](../kernel/policy/README.md) is consulted.
The manager is its only hard-wired case: uid 1053, the WebView zygote, was a second one and is
now an ordinary profile row, synthesized by the manager and exempted from the allowlist prune
because no package owns it.

## Where this fork differs from upstream

This tree is a fork of [tiann/KernelSU](https://github.com/tiann/KernelSU). It keeps upstream's
architecture and rebases onto it, so the difference is not a set of commits but a set of
additions. One is identity: the manager is renamed and signed with a private key, and the
matching APK size and signing-block hash are pinned in [`kernel/Kbuild`](../kernel/Kbuild). The
rest are features upstream does not have:

| Feature | Where | What it is |
| --- | --- | --- |
| UTS spoofing | [`supercall/dispatch.c`](../kernel/supercall/dispatch.c) | `strscpy` into `init_uts_ns.name` under `uts_sem`, so [`uname`][uname-2] and `/proc/version` report a chosen release and version |
| CPU spoofing | [`supercall/dispatch.c`](../kernel/supercall/dispatch.c) | Four edits, only the first of them per-CPU - see below |
| Memory spoofing | [`feature/mem_spoof.c`](../kernel/feature/mem_spoof.c) | [kretprobes][kprobes] on `si_meminfo` and friends, armed on demand |
| Mount hiding | [`feature/mount_hide.c`](../kernel/feature/mount_hide.c) | Per-record filtering of the three `/proc` mount files |
| ptctl | [`feature/ptctl.c`](../kernel/feature/ptctl.c) | Memory, registers, signals, kill-guard and hardware breakpoints on another process, without [`ptrace`][ptrace-2] |
| uhook | [`feature/uhook.c`](../kernel/feature/uhook.c) | Userspace instrumentation built on [uprobes][uprobes] |

`do_set_spoof_cpu()` is one ioctl doing four separate things, which matters when only some of
them take effect. It writes `reg_midr` into the per-CPU `cpu_data` slot the command names; then,
once, on cpu index 0 only, it recomputes the global `loops_per_jiffy` from a requested BogoMIPS
figure and overwrites the global `elf_hwcap`/`elf_hwcap2` bitmap. The fourth is conditional on
the MIDR part not being Neoverse-N1, Cortex-A76 or Kryo-4XX-Gold: for a part with no erratum to
explain away it forces the vDSO clock mode to `VDSO_CLOCKMODE_ARCHTIMER` in the kernel-side vDSO
time data, the active clocksource and `vdso_default`, and clears `ARM64_WORKAROUND_1418040`.
Every address in the second, third and fourth group is resolved by name at call time, and each
one is skipped with a warning if the running kernel does not have it.

[`boot-patching.md`](boot-patching.md) documents how the module is installed into a boot image
in the first place, including the out-of-tree crate that handles the format.

[`instrumentation.md`](instrumentation.md) is the caller-facing reference for the last two:
the verb tables, the worked sequences, and what each mechanism leaves visible to the target.

Fork-local ioctl numbers start at `'K', 42` and fork-local feature ids at 16, both deliberately
above the range upstream allocates from, so a rebase does not silently renumber a value that the
manager and `.feature_config` already refer to. The rule and the reason are in
[`uapi/README.md`](../uapi/README.md); ignoring it costs a day of debugging after the next merge.

## Rules that cut across every layer

**An unexported symbol can be reached two ways, and picking the wrong one ships a NULL call.**
Almost nothing this module needs is exported to modules, and the fork answers that in two
different ways. Decide which of them a new call belongs to before writing it;
[`kernel/infra/README.md`](../kernel/infra/README.md) states the rule, and the CFI case where
the linker's answer is the wrong address even for a symbol that is exported.

- *Bound at load, called as an ordinary function.* A symbol that can be promised on every
  kernel this ships against - `path_mount`, `path_umount`, `avc_ss_reset`,
  `ext4_unregister_sysfs` - stays a bare `extern` declaration and a direct call.
  [`ksuinit`](../userspace/ksuinit/README.md) rewrites each [`SHN_UNDEF`][elf-5] entry in
  `kernelsu.ko` to `SHN_ABS` from `/proc/kallsyms` before it calls
  [`init_module`][init-module-2], which is what makes the call link at all.
  [`tools/check_symbol.c`](../kernel/tools/check_symbol.c) is the guardrail: it fails the build
  when the target `vmlinux` does not define one of those names. Without it a typo would ship,
  because `ksuinit` logs `Cannot find symbol` and loads the module anyway.
- *Resolved after load, through a NULL-checked pointer.* Anything version- or
  config-dependent - `uprobe_register`, `elf_hwcap`, `vdso_k_time_data`, `task_call_func`,
  `perf_event_output_forward` - goes through
  [`infra/symbol_resolver.c`](../kernel/infra/symbol_resolver.c) and is checked at every use. A
  name absent on one target would otherwise arrive on a user's device as a zeroed relocation
  and become a NULL call on first use.

**A register without its unregister is a bug, not a degraded mode.** Several features refuse to
arm at all unless *both* halves resolve. Arming a probe that can never be retired means the next
hit after `rmmod` executes freed module text.

**Teardown order is load-bearing.** `kernelsu_exit()` retires hooks in phase one, waits out
[RCU][whatisrcu] readers with `synchronize_rcu()`, and only then frees the structures those
hooks could reach. Adding a feature means adding its teardown call to the right phase; the
phases are listed in
[`kernel/core/README.md`](../kernel/core/README.md#kernelsu_exit-and-the-two-phases).

**The uapi headers are frozen once shipped.** Three consumers compile them: the module, ksud
through `bindgen`, and the manager's C++. A struct that changes layout breaks two of the three
silently.

## Where to start reading

For the kernel side, in order: [`kernel/README.md`](../kernel/README.md) for the map,
[`kernel/core/README.md`](../kernel/core/README.md) for what happens at load time,
[`kernel/supercall/README.md`](../kernel/supercall/README.md) for how userspace talks to it, then
[`kernel/hook/README.md`](../kernel/hook/README.md) for how it attaches itself to a kernel it
does not own. After those four, any feature in
[`kernel/feature/`](../kernel/feature/README.md) reads on its own.

For the userspace side, [`userspace/ksud/README.md`](../userspace/ksud/README.md) covers the
daemon and its several jobs; [`userspace/ksuinit/README.md`](../userspace/ksuinit/README.md) is
short and worth reading first if you care about how the module gets loaded at all.

Build and CI are in [`scripts/README.md`](../scripts/README.md).

<!-- reference links: kernel documentation and man pages -->
[elf-5]: https://man7.org/linux/man-pages/man5/elf.5.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[execveat-2]: https://man7.org/linux/man-pages/man2/execveat.2.html
[faccessat-2]: https://man7.org/linux/man-pages/man2/faccessat.2.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
[ioctl-2]: https://man7.org/linux/man-pages/man2/ioctl.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[lsm]: https://docs.kernel.org/security/lsm.html
[mount-namespaces-7]: https://man7.org/linux/man-pages/man7/mount_namespaces.7.html
[ptrace-2]: https://man7.org/linux/man-pages/man2/ptrace.2.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[setresuid-2]: https://man7.org/linux/man-pages/man2/setresuid.2.html
[stat-2]: https://man7.org/linux/man-pages/man2/stat.2.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
[uname-2]: https://man7.org/linux/man-pages/man2/uname.2.html
[uprobes]: https://docs.kernel.org/trace/uprobetracer.html
[whatisrcu]: https://docs.kernel.org/RCU/whatisRCU.html
