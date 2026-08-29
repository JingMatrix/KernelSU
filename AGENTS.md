# KernelSU Agent Guide

## Before you start

- Read [`docs/architecture.md`](docs/architecture.md) first. It maps the whole tree and links to a
  README in every subproject; those READMEs are the reference, and this file is only the
  quick-start.
- This is a **fork** of [tiann/KernelSU](https://github.com/tiann/KernelSU) with its own commits on
  top of upstream `dev`. Upstream rewrites its `dev` history, so `git fetch upstream` can
  force-update
  the remote-tracking ref. Check `git log --oneline upstream/dev..dev` before assuming a
  commit is ours.
- Upstream documentation, the website, and your own memory of KernelSU are all unreliable here.
  The working tree is the authority.
- Default to `rg` for searching. Keep edits ASCII unless the file already uses non-ASCII.
- Run the component checks below before handing work off. Do not skip a failing step.
- Documentation convention: a kernel concept or userspace API gets a reference-style link to
  background documentation on its first mention in a file, and only the first -- kernel
  Documentation for concepts, man pages for syscalls. Targets live in one definition block at
  the end of each file. Do not link symbols to a source cross-referencer: a reader clicking a
  term wants an explanation, not a list of use sites. Files in this repository keep relative
  links; prose wraps near 90 columns.

## Repository structure

```
kernel/           Kernel module (C), split into layers - see kernel/README.md
uapi/             Shared kernel/userspace ABI headers. Frozen once shipped.
userspace/ksud/   Userspace daemon, module manager, su implementation, CLI (Rust)
userspace/ksuinit/  Ramdisk init shim that loads the LKM as pid 1 (Rust)
manager/          Android manager app (Kotlin/Compose + JNI bridge)
website/          VitePress documentation site
js/               npm package backing module WebUI
scripts/          Build and packaging automation
docs/             Architecture hub, design studies, translated project READMEs
.github/workflows/  CI
```

There is no `userspace/meta-overlayfs` crate in this tree. The Cargo workspace is `ksud` and
`ksuinit`, and nothing else.

## Kernel layout

The kernel module has no `.c` files at the top level. Everything lives under a layer directory,
each with its own README:

| Directory | Contents |
| --- | --- |
| `kernel/core/` | `init.c` - `kernelsu_init()` / `kernelsu_exit()` |
| `kernel/hook/` | [LSM][lsm] hooks, [kprobes][kprobes], [tracepoints][tracepoints], arm64/x86_64 syscall patching |
| `kernel/supercall/` | `supercall.c` (anon inode + reboot [kprobe][kprobes]), `dispatch.c` (command table), `perm.c` |
| `kernel/policy/` | `allowlist.c`, `app_profile.c`, `feature.c` |
| `kernel/infra/` | `symbol_resolver.c`, `file_wrapper.c`, `event_queue.c`, `seccomp_cache.c`, `su_mount_ns.c` |
| `kernel/manager/` | `apk_sign.c`, `throne_tracker.c`, `pkg_observer.c` |
| `kernel/runtime/` | `boot_event.c`, `ksud_integration.c` |
| `kernel/selinux/` | `selinux.c`, `rules.c`, `sepolicy.c` |
| `kernel/sulog/` | `event.c`, `fd.c` |
| `kernel/feature/` | `sucompat.c`, `kernel_umount.c`, `mount_hide.c`, `mem_spoof.c`, `selinux_hide.c`, `adb_root.c`, `sulog.c`, `ptctl.c`, `uhook.c` |

## Core concepts

- **supercall** - the ioctl control plane. Userspace obtains the `[ksu_driver]` anon-inode fd by
  calling `reboot(KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd)`, which a kprobe in
  `kernel/supercall/supercall.c` intercepts and completes from a `task_work`. The command table and
  every `do_*` handler are in `kernel/supercall/dispatch.c`; the five permission predicates
  (`always_allow`, `only_root`, `only_manager`, `manager_or_root`, `allowed_for_su`) are in
  `kernel/supercall/perm.c`. The ABI is `uapi/supercall.h`. ksud's client is
  `userspace/ksud/src/ksucalls.rs`.
- **allowlist** - which uids may become root. `kernel/policy/allowlist.c`, a kref-counted hashtable
  persisted to `/data/adb/ksu/.allowlist`, initialised from `kernel/core/init.c`.
- **app profile** - what root means for a given app. The struct is `uapi/app_profile.h`; kernel-side
  code is `kernel/policy/app_profile.c`; persistence is in `allowlist.c`.
- **feature flags** - runtime toggles registered through `kernel/policy/feature.c` and enumerated in
  `uapi/feature.h`. Upstream allocates ids from 0 upwards; **fork-local features start at 16**,
  because the id is a wire value the manager and `/data/adb/ksu/.feature_config` refer to and cannot
  be renumbered by a rebase.
- **manager identity** - the kernel authenticates the manager APK by the size and SHA-256 of its
  signing block (`kernel/manager/apk_sign.c`, pinned via `KSU_EXPECTED_SIZE`/`KSU_EXPECTED_HASH` in
  `kernel/Kbuild`). `throne_tracker.c` resolves its uid, `pkg_observer.c` watches for changes. This
  fork pins its own key and package name. `CONFIG_KSU_DISABLE_MANAGER` removes all of it.
- **sucompat** - reroutes `/system/bin/su` to `/data/adb/ksud` by rewriting the userspace filename
  pointer in [`execve`][execve-2]/[`faccessat`][faccessat-2]/`newfstatat`. The handlers are
  in `kernel/feature/sucompat.c`;
  registration is `ksu_register_syscall_hook()` in `kernel/hook/syscall_hook_manager.c`.
- **sulog** - audit records for root grants, exposed as a second anon-inode fd
  (`KSU_IOCTL_GET_SULOG_FD`) and persisted by a `sulogd` child of ksud. Emitters are in
  `sucompat.c`, `hook/syscall_event_bridge.c` and `supercall/dispatch.c`.
- **module** - a flashable zip unpacked by `userspace/ksud/src/module.rs` into `/data/adb/modules/`,
  with lifecycle scripts run from `init_event.rs`. Paths are in `defs.rs`.
- **metamodule** - a module marked `metamodule=1` in `module.prop`. ksud allows one, symlinks
  `/data/adb/metamodule`, and runs its hooks before regular modules
  (`userspace/ksud/src/metamodule.rs`). There is no in-repo metamodule implementation.

## Component workflows

### Kernel (`kernel/`)

C only. There is no kernel objtree in this checkout, so the module cannot be compiled here; CI
(`.github/workflows/ddk-lkm.yml`) builds it against the GKI DDK. What you can run locally:

```bash
cd kernel && make check-format     # clang-format; note your local clang-format version may
                                   # disagree with CI's even on unmodified upstream files
```

If you change an ioctl, a profile field or a feature id, update `uapi/`, then
`userspace/ksud/src/ksucalls.rs`, then `manager/app/src/main/cpp/ksu.cc` if the manager uses that
command.

### Userspace Rust (`userspace/ksud`, `userspace/ksuinit`)

```bash
cargo ndk -t arm64-v8a check
cargo ndk -t arm64-v8a clippy
cargo fmt
```

Notes for this machine: the workspace needs rustc >= 1.91 (`adb_client`), so the `stable` toolchain
at 1.89 will refuse; use `rustup run nightly`. Checking the Android target also needs an NDK on
`PATH` for `cc-rs` - there are several under `~/Archives/Android/ndk/`. A plain host
`cargo check --workspace` compiles, but most of ksud is behind `#[cfg(target_os =
"android")]`, so it
proves very little.

`main.rs` gates nearly every module on `target_os = "android"`. If you add a module, add the same
gate or the host build breaks.

### Android Manager App (`manager/`)

```bash
cd manager
mkdir -p app/src/main/jniLibs/arm64-v8a
cp ../target/aarch64-linux-android/release/ksud app/src/main/jniLibs/arm64-v8a/libksud.so
./gradlew assembleRelease
```

The build fails without `libksud.so` in `jniLibs`. `just build_manager` (alias `just bm`) chains
`cross build --target aarch64-linux-android --release`, the copy, and `./gradlew aDebug` - note it
produces a debug APK and does not create the `jniLibs` directory, so make it once by hand.

The JNI bridge mirrors a **subset** of the supercall surface: `ksu.cc` issues `GET_INFO`,
`GET_INFO_LEGACY`, `CHECK_SAFEMODE`, `NEW_GET_ALLOW_LIST`, `UID_SHOULD_UMOUNT`, `GET_APP_PROFILE`,
`SET_APP_PROFILE`, `GET_FEATURE` and `SET_FEATURE`. Everything else the app needs it gets by
shelling out to `ksud`. Do not assume a new ioctl belongs in the bridge; check how the app actually
reaches the feature.

### Website (`website/`)

```bash
cd website && bun install && bun run docs:build
```

## Common pitfalls

- Only one metamodule can be active; keep the ksud hooks in sync.
- A `register_*` symbol resolved without its `unregister_*` counterpart is a bug, not a degraded
  mode: it arms something that cannot be retired, and the next hit after `rmmod` runs freed text.
- Most kernel symbols this module needs are unexported. Resolve them through
  `kernel/infra/symbol_resolver.c` rather than linking against them.
- Teardown order in `kernelsu_exit()` is load-bearing: hooks first, `synchronize_rcu()`, then free.
  A new feature needs its exit call in the right phase.
- `uapi/` structs are compiled by the module, by ksud through `bindgen`, and by the manager's C++.
  Changing a layout breaks two of the three silently.

## Git

- Upstream-facing changes follow upstream's style: `<scope>: <summary>` or
  `<type>(<scope>): <summary>`, lowercase scope (`kernel`, `ksud`, `manager`, `docs`, `scripts`).
- The fork's own feature commits use a sentence-case imperative summary with a long explanatory
  body (`Implement uhook: general userspace instrumentation via uprobes`). Match whichever
  convention the surrounding history uses.
- Keep subjects under about 72 characters. Reference a PR as `(#1234)` at the end.
- Check `git log --oneline` before committing.
- Never push without being asked.

<!-- reference links: kernel documentation and man pages -->
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[faccessat-2]: https://man7.org/linux/man-pages/man2/faccessat.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[lsm]: https://docs.kernel.org/security/lsm.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
