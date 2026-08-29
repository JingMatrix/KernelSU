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

The meeting point is a single ioctl channel described in [`uapi/`](../uapi/README.md), called
the *supercall*. If you read nothing else before touching this codebase, read that header
directory and [`kernel/supercall/`](../kernel/supercall/README.md).

## The pieces

| Path | What it is |
| --- | --- |
| [`kernel/`](../kernel/README.md) | The module: build modes, init order, and the layer map |
| [`kernel/core/`](../kernel/core/README.md) | `module_init` / `module_exit` and the order everything comes up in |
| [`kernel/hook/`](../kernel/hook/README.md) | [LSM][lsm] hooks, [kprobes][kprobes], [tracepoints][tracepoints] and syscall-table patching |
| [`kernel/supercall/`](../kernel/supercall/README.md) | The ioctl control plane and its permission classes |
| [`kernel/policy/`](../kernel/policy/README.md) | Allowlist, app profiles, feature flags |
| [`kernel/infra/`](../kernel/infra/README.md) | Symbol resolution, file wrapper, event queue, [seccomp][seccomp-filter] cache |
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
to the magic `reboot`. Every command is an ioctl on that descriptor, dispatched through a table
in [`supercall/dispatch.c`](../kernel/supercall/dispatch.c) where each entry names a handler and
a permission class: `always_allow`, `only_root`, `only_manager`, `manager_or_root`, or
`allowed_for_su`. The classes live in [`supercall/perm.c`](../kernel/supercall/perm.c) and are
three lines each; read them before assuming what a command is protected by.

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
`kernelsu_init()` takes a different branch: it skips the boot-time kprobes entirely, applies
its [SELinux][selinux] rules immediately, escalates the loading process so it keeps working
once SELinux is enforcing again, and calls `track_throne(false)` to find the manager right
away rather than waiting for a boot event that will never arrive.

That single comparison is why the same `.ko` behaves differently depending on who inserts it.

## From power-on to a rooted device

1. `kernelsu_init()` in [`core/init.c`](../kernel/core/init.c) resolves kernel symbols, installs
   the syscall hooks, registers the feature handlers, and arms the `reboot` kprobe.
2. Android's init starts and reads `/system/etc/init/init.rc`. The module is watching:
   [`runtime/ksud_integration.c`](../kernel/runtime/ksud_integration.c) recognises the first
   read of that file by init, and because `file_operations` live in read-only memory, it swaps
   in a *proxy* `file_operations` on the open file rather than patching the original. The proxy
   waits for the read that returns 0 - init reads to EOF - and appends KernelSU's own rc content
   there. Android parses it as if it had always been in the file.
3. That rc content starts `ksud`, which reports `EVENT_POST_FS_DATA` back through the supercall.
   The module loads the allowlist from disk and starts watching for the manager package.
4. ksud mounts modules from `/data/adb/modules/` and reports `EVENT_MODULE_MOUNTED`, which sets
   `ksu_module_mounted`. Until that flag is set, the kernel unmount path does nothing, because
   there is nothing to unmount.
5. At `EVENT_BOOT_COMPLETED`, [`runtime/boot_event.c`](../kernel/runtime/boot_event.c) runs
   `track_throne(true)` to settle the manager's uid.

Safe mode short-circuits this. The module counts volume-down presses through a kprobe on the
input layer; three presses and `ksu_is_safe_mode()` returns true, ksud skips every module, and
the device boots clean. That is the recovery path when a module bootloops the phone.

## What happens when an app asks for root

Two routes end in the same place.

**Through `su`.** [`feature/sucompat.c`](../kernel/feature/sucompat.c) intercepts
[`execve`][execve-2],
[`faccessat`][faccessat-2] and `newfstatat`, compares the path against `/system/bin/su`, and
rewrites the userspace filename pointer to `/data/adb/ksud` before the syscall proceeds.
Nothing is installed at `/system/bin/su`; the file does not need to exist. ksud then finds
`argv[0]` is `su` (or ends in `/su`) and enters `su::root_shell()` in
[`su.rs`](../userspace/ksud/src/su.rs).

**Through the manager.** The app calls the JNI bridge, which issues the same ioctl directly.

Either way the request arrives as `KSU_IOCTL_GRANT_ROOT`, gated by `allowed_for_su()`: the
caller is the manager, or its uid is on the allowlist. The handler calls
`escape_with_root_profile()`, which is where the app profile from
[`policy/app_profile.c`](../kernel/policy/app_profile.c) turns into an actual credential change -
uid, gid, supplementary groups, capability sets and SELinux context. A record goes into the
[sulog](../kernel/sulog/README.md) either way, so a grant that succeeds and a grant that fails
are both visible to the manager.

## Keeping modules out of sight

Three mechanisms, three different layers, and they are not interchangeable:

- **Unmounting** ([`feature/kernel_umount.c`](../kernel/feature/kernel_umount.c)) removes module
  mounts from a process's mount namespace as it drops privilege in
  [`setresuid`][setresuid-2]. It only works
  where the process has a private namespace to modify.
- **Filtering** ([`feature/mount_hide.c`](../kernel/feature/mount_hide.c)) leaves the mounts
  alone and edits what `/proc/<pid>/{mountinfo,mounts,mountstats}` *prints*, keyed on the reader.
  It works in the global namespace, where unmounting would be catastrophic.
- **Spoofing** ([`feature/selinux_hide.c`](../kernel/feature/selinux_hide.c)) sanitises what
  `/sys/fs/selinux` reports, so the policy edits the module made are not visible as policy edits.

`ksu_uid_should_umount()` in [`policy/allowlist.c`](../kernel/policy/allowlist.c) is the shared
decision point for the first two. It excludes the manager and su-granted apps, and it is where
the per-app profile setting is consulted.

## Where this fork differs from upstream

This tree is a fork of [tiann/KernelSU](https://github.com/tiann/KernelSU) and carries seven
commits of its own on top. Two of them change identity: the manager is renamed and signed with a
private key, with the matching size and hash pinned in [`kernel/Kbuild`](../kernel/Kbuild). The
other five add features upstream does not have:

| Feature | Where | What it is |
| --- | --- | --- |
| UTS spoofing | [`supercall/dispatch.c`](../kernel/supercall/dispatch.c) | Rewrites `init_uts_ns` so `uname(2)` and `/proc/version` report a chosen kernel |
| CPU spoofing | [`supercall/dispatch.c`](../kernel/supercall/dispatch.c) | Rewrites per-CPU `reg_midr`, `elf_hwcap`, and the vDSO clock mode |
| Memory spoofing | [`feature/mem_spoof.c`](../kernel/feature/mem_spoof.c) | [kretprobes][kprobes] on `si_meminfo` and friends, armed on demand |
| Mount hiding | [`feature/mount_hide.c`](../kernel/feature/mount_hide.c) | Per-record filtering of the three `/proc` mount files |
| ptctl and uhook | [`feature/ptctl.c`](../kernel/feature/ptctl.c), [`feature/uhook.c`](../kernel/feature/uhook.c) | Process control without ptrace, and uprobe-based userspace instrumentation |

[`boot-patching.md`](boot-patching.md) documents how the module is installed into a boot image
in the first place, including the out-of-tree crate that handles the format.

[`instrumentation.md`](instrumentation.md) is the caller-facing reference for the last two:
the verb tables, the worked sequences, and what each mechanism leaves visible to the target.

Fork-local ioctl numbers start at `'K', 42` and fork-local feature ids at 16, both deliberately
above the range upstream allocates from, so a rebase does not silently renumber a value that the
manager and `.feature_config` already refer to. The rule and the reason are in
[`uapi/README.md`](../uapi/README.md); ignoring it costs a day of debugging after the next merge.

## Rules that cut across every layer

**Resolve symbols, do not link against them.** Most of what this module needs -
`find_task_by_vpid`, `path_mount`, `avc_ss_reset`, `uprobe_register` - is not exported. They are
looked up through kallsyms by [`infra/symbol_resolver.c`](../kernel/infra/symbol_resolver.c). A
build that links a bare unexported symbol produces a `.ko` that loads nowhere.

**A register without its unregister is a bug, not a degraded mode.** Several features refuse to
arm at all unless *both* halves resolve. Arming a probe that can never be retired means the next
hit after `rmmod` executes freed module text.

**Teardown order is load-bearing.** `kernelsu_exit()` retires hooks in phase one, waits out
[RCU][whatisrcu] readers with `synchronize_rcu()`, and only then frees the structures those
hooks could reach. Adding a feature means adding its teardown call to the right phase.

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
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[faccessat-2]: https://man7.org/linux/man-pages/man2/faccessat.2.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[lsm]: https://docs.kernel.org/security/lsm.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[setresuid-2]: https://man7.org/linux/man-pages/man2/setresuid.2.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
[whatisrcu]: https://docs.kernel.org/RCU/whatisRCU.html
