# The Rust workspace

KernelSU's Rust userspace lives in two crates here, both members of the Cargo workspace
rooted at the repository top level. [`Cargo.toml`](../Cargo.toml) lists `userspace/ksud`
and `userspace/ksuinit` as both `members` and `default-members`, pins `resolver = "3"` and
`edition = "2024"`, and shares two dependency versions (`anyhow`, `log`). There is no third
crate: [`AGENTS.md`](../AGENTS.md) names `userspace/meta-overlayfs` only to deny that it
exists, so a reference to one anywhere else is stale.

## The two crates and the edge between them

[`ksud`](ksud/README.md) is the daemon, CLI, boot-image patcher and `su` implementation.
[`ksuinit`](ksuinit/README.md) is the ramdisk shim that becomes PID 1 on an LKM-mode boot.
They are coupled twice over, in two entirely different ways.

The first coupling is an ordinary Cargo dependency. [`ksud/Cargo.toml`](ksud/Cargo.toml)
declares `ksuinit = { path = "../ksuinit" }` inside its
`[target.'cfg(target_os = "android")'.dependencies]` block, so ksud links the ksuinit
*library* only when built for Android. [`ksuinit/Cargo.toml`](ksuinit/Cargo.toml) names no
explicit targets, so Cargo infers both a library from `src/lib.rs` and a binary from
`src/main.rs`. The split matters: [`ksuinit/src/main.rs`](ksuinit/src/main.rs) declares `mod
init;`, so [`ksuinit/src/init.rs`](ksuinit/src/init.rs) - the mount dance, the kmsg logger,
the `/init` symlink swap, everything that assumes PID 1 - is compiled into the binary alone,
invisible to ksud. What ksud gets is [`ksuinit/src/lib.rs`](ksuinit/src/lib.rs), whose two
exports of consequence are `load_module()` and `has_kernelsu()`. `load_module()` parses the
`.ko` with goblin and, for each [`SHN_UNDEF`][elf-5] symbol it can find while walking
`/proc/kallsyms`, rewrites that symbol table entry to `SHN_ABS` at the harvested address;
whatever is left unresolved gets a `Cannot find symbol` warning and the
[`init_module(2)`][init-module-2] call goes ahead regardless. That work is as necessary at
late-load time as at boot, which is why it is a library and not a duplicate. On the boot
path [`ksuinit/src/init.rs`](ksuinit/src/init.rs) calls both, once `/proc` is up. On the
late-load path ksud calls them at three more sites: `has_kernelsu()` and `load_module()` in
[`ksud/src/late_load.rs`](ksud/src/late_load.rs), and `load_module()` again behind `ksud
insmod` in [`ksud/src/debug.rs`](ksud/src/debug.rs).

The syscall gets a second attempt. A kernel refuses a module whose `.modinfo` vermagic
string disagrees with its own, and reports the mismatch only in the ring buffer, so
`load_module()` opens `/dev/kmsg` with `O_NONBLOCK` and seeks to the end *before* calling
`init_module(2)` - falling back to `/kmsg`, because ksuinit can run before anything has
populated `/dev`. Should the syscall fail for any reason, it drains the records that
appeared since that seek and looks for the `version magic 'X' should be 'Y'` line. Finding
one, `replace_module_vermagic()` rebuilds `.modinfo` with the kernel's string in place of
the `vermagic=` entry, appends the result at the end of the buffer aligned to
`sh_addralign`, repoints that section's `sh_offset` and `sh_size`, and the syscall runs
again. Appending rather than editing in place is what keeps every other section's file
offset valid when the required string is longer than the one shipped, and it is why a
vendor kernel built from the same KMI but a different local version loads at all.

The second coupling is that ksud carries the ksuinit *binary* as data. It never links it;
it writes it into a ramdisk. In [`ksud/src/boot_patch.rs`](ksud/src/boot_patch.rs) the
patcher fetches the asset named `ksuinit` (or `{arch}/ksuinit` on a host build), renames
the stock `init` cpio entry to `init.real`, and adds the ksuinit bytes back as `init`. Both
crates build for the same two Android triples now, and that binary still cannot come out of
the same `cargo build` as ksud: rust-embed bakes its folder at compile time, so a finished
ksuinit has to be lying in `userspace/ksud/bin/<arch>/` before ksud is compiled at all. It
arrives as a file on disk, and CI orders the two jobs so that it does.

## The release profile

The workspace root sets one profile for both crates:

```text
[profile.release]
strip = true
lto = true
opt-level = "z"
panic = "abort"

[profile.release.package.ksud]
codegen-units = 1
```

`panic = "abort"` is about ksuinit before it is about size. Its entry point is `#![no_main]`
plus a hand-written `pub unsafe extern "C" fn main(...)`, because Rust's normal start-up
aborts when stdin, stdout and stderr are not all open and PID 1 spawned from an initramfs
has none of them. A panic unwinding out of that `extern "C"` boundary never reaches a
caller anyway - rustc plants an abort shim at a plain `extern "C"` frontier, and an abort in
PID 1 panics the kernel - so `panic = "abort"` only makes the single possible outcome
explicit, and drops the unwind tables that would otherwise be dead weight in both binaries.
Both crates carry their errors in `anyhow::Result`, so a panic is always a bug.

`opt-level = "z"`, `lto` and `strip` are there because neither binary ships on its own.
ksud ends up as `lib/<abi>/libksud.so` inside the manager APK while itself embedding every
per-KMI `kernelsu.ko` plus ksuinit, so its size is paid for twice; ksuinit ends up inside
a boot or `init_boot` ramdisk, where the budget is a partition. `codegen-units = 1` is
scoped to ksud alone.

## Cross-compiling for Android

Both binaries are built for `aarch64-linux-android` and `x86_64-linux-android`, linked with
the NDK's API-26 clang; ksuinit used to be built for `aarch64-unknown-linux-musl` alone and
no longer is. Nothing in this tree uses `cargo ndk`, despite what
[`AGENTS.md`](../AGENTS.md) says: no manifest, workflow or justfile recipe invokes it, and
no CI step installs `cargo-ndk`.

Two floors sit under all of it. The workspace needs rustc 1.91 or newer, because ksud's
Android-only `adb_client` dependency declares `rust-version = "1.91.0"`; a `stable`
toolchain still at 1.89 refuses to resolve the graph at all rather than failing somewhere
informative, and `rustup run nightly` is the usual way past that. And a `cargo check` aimed
at an Android triple needs a real NDK on the machine even though it never links, because
[`ksud/build.rs`](ksud/build.rs) drives an assembler and libclang long before rustc reaches
codegen. A plain host `cargo check --workspace` does compile, but nearly all of ksud sits
behind `#[cfg(target_os = "android")]`, so it proves very little.

A linker alone is not enough for ksud. [`ksud/build.rs`](ksud/build.rs) reaches for a
toolchain twice before rustc runs, and only one of the two is about the Android target.

`assemble_bootstrap()` runs first and runs unconditionally, host builds included. It
assembles [`ksud/src/lkm_image_bootstrap.S`](ksud/src/lkm_image_bootstrap.S) into
`$OUT_DIR/lkm_image_bootstrap.o`, which [`ksud/src/lkm_image.rs`](ksud/src/lkm_image.rs)
pulls back in with `include_bytes!`. That object is AArch64 kernel code destined for a cave
in someone else's `Image`, so it has to be assembled for AArch64 whatever ksud itself is
being built for - a Darwin or Windows ksud carries the identical bytes. Every candidate is
validated before use: at least 64 bytes, a `\x7fELF` magic, class 2, little-endian, `e_type`
of 1 (`ET_REL`) and `e_machine` of 183 (`EM_AARCH64`). A wrong-architecture assembler
therefore fails the build instead of producing a boot image that hangs on the device. The
search order is `KSU_LKM_BOOTSTRAP_OBJECT` naming a ready-made object, then the gitignored
`userspace/ksud/.lkm_image_bootstrap.o` that a cross build leaves on the host before
entering the container, then `KSU_LKM_BOOTSTRAP_CC`, `aarch64-linux-gnu-gcc`, the first
clang found under `ANDROID_NDK_HOME` or `ANDROID_NDK_ROOT`, a bare `clang`, and last
`llvm-mc -triple=aarch64-linux-gnu -filetype=obj`. Any driver whose file name contains
`clang` gets `--target=aarch64-linux-gnu` prepended. Exhaust the list and build.rs panics
with every error it collected along the way.

The second reach is bindgen, which runs only when `CARGO_CFG_TARGET_OS == "android"`,
pointed at [`ksud/src/ksu_uapi.h`](ksud/src/ksu_uapi.h) - a one-line file that `#include`s
[`uapi/ksu.h`](../uapi/ksu.h) - with `clang_args(["-x", "c++", "-I../../"])`. The
`-I../../` resolves the repository-root-relative include style the headers use, and
`-x c++` makes bindgen fold the `static const __u32 KSU_IOCTL_*` definitions into Rust
`const`s instead of unusable `extern` statics. The result is written to
`$OUT_DIR/bindings.rs` and re-enters the crate through
[`ksud/src/ksu_uapi.rs`](ksud/src/ksu_uapi.rs), three lines whose blanket `allow` of
`nonstandard_style`, `unused`, `unsafe_op_in_unsafe_fn` and all three clippy groups is the
only reason machine-generated bindings do not trip the lint settings described below.
Since bindgen drives libclang directly rather than through the linker wrapper, it needs its
own `--sysroot` and per-arch include path via `BINDGEN_EXTRA_CLANG_ARGS_<triple>`; miss
that and the build dies inside `build.rs` looking for `<linux/types.h>`, not at link time.
Three routes satisfy this.

A generated `.cargo/config.toml` is the first.
[`scripts/setup_cargo_config.py`](../scripts/setup_cargo_config.py) finds an NDK from
`--ndk-root`, `ANDROID_NDK_ROOT`, `ANDROID_NDK_HOME` or the SDK's `ndk/` directory,
derives a host tag, and writes `[target.*].linker` plus an `[env]` block carrying `CC_`,
`CXX_`, `AR_` and `BINDGEN_EXTRA_CLANG_ARGS_` for both triples; it refuses to overwrite
without `--force`, and its output is gitignored. Do not hand-copy the tracked template
[`.cargo/config.example.toml`](../.cargo/config.example.toml) instead: its live `[env]`
block sets only `CC_`/`CXX_`/`AR_` and strands the `BINDGEN_EXTRA_CLANG_ARGS_` lines in a
commented preamble, so the result fails in bindgen.

Second is `cross`, which supplies its own container image and so needs neither an NDK nor
a `.cargo/config.toml`. That is what [`justfile`](../justfile) uses, without
`--manifest-path`, so from the repository root it builds both default members for the
Android triple and discards the ksuinit one. The output still lands at
`target/aarch64-linux-android/release/ksud`: a workspace member's artefacts go to the
workspace target directory whichever manifest was named. The bootstrap step in build.rs runs
inside that container like anywhere else, so the image has to carry an assembler it can find
or `.lkm_image_bootstrap.o` has to be prepared before `cross` is invoked.

Third is CI, a `cargo build --manifest-path ./userspace/ksud/Cargo.toml` that skips the
container and takes its environment from
[`.github/scripts/setup-rust-build.sh`](../.github/scripts/setup-rust-build.sh), which
takes a triple and an API level and builds the variable names by hand:

```sh
export CARGO_TARGET_${UUTRIPLE}_LINKER="$CLANG_PATH"
export BINDGEN_EXTRA_CLANG_ARGS_$UTRIPLE="--sysroot=$LLVM_PATH/sysroot -I$LLVM_PATH/sysroot/usr/include/$TRIPLE"
```

It is `source`d, not executed: [`ksud.yml`](../.github/workflows/ksud.yml),
[`clippy.yml`](../.github/workflows/clippy.yml) and
[`ksuinit.yml`](../.github/workflows/ksuinit.yml) all source it and then run cargo in the
same `run:` block, so the exports survive. It leaves `TRIPLE` set for `--target` and
`CLANG_PATH` pointing at the driver, and it hardcodes `prebuilt/linux-x86_64`, so it needs a
Linux runner.

The ksuinit binary has its own recipe in
[`ksuinit.yml`](../.github/workflows/ksuinit.yml). It pulls NDK r29 in through
`nttld/setup-ndk`, then sources that same script once per Android triple and runs
`cargo build --package ksuinit` for each, uploading the results as separate
`ksuinit-aarch64` and `ksuinit-x86_64` artefacts. Four `RUSTFLAGS` carry the build, and
both `run:` blocks set all four. `-C target-feature=+crt-static` is not a size choice: this
binary is PID 1 inside an initramfs that contains nothing but itself, so there is no
`/system/lib64/libc.so` yet for a dynamic loader to find. `-C link-arg=$BUILTINS` puts
`libclang_rt.builtins-<arch>-android.a` on the link line by absolute path, so that the
compiler-rt intrinsics rustc emits calls to resolve; the step derives that path from
`$CLANG_PATH --print-resource-dir` and runs `test -f` on it first, which turns an NDK whose
layout moved into a clear failure rather than a pile of undefined symbols.
`-C link-arg=-Wl,-z,max-page-size=16384` aligns the segments for the 16 KB page devices
Android now ships, which will not map a binary laid out for 4 KB. The fourth,
`-C link-arg=-Wno-unused-command-line-argument`, only quiets the clang driver, which
otherwise complains about link-only arguments handed to invocations that do not link.
[`ksuinit/build.rs`](ksuinit/build.rs) is a leftover from the musl era: it re-emits `-lc` at
the end of the link line so that `compiler_builtins`' reference to `getauxval` resolves, but
only for `aarch64-unknown-linux-musl` and `x86_64-unknown-linux-musl`, so nothing CI builds
reaches it any more.

Host ksud has a workflow of its own, and it is where the desktop binaries actually come
from. [`ksud-extra.yml`](../.github/workflows/ksud-extra.yml) installs `cross` from git at
rev `66845c1`, then runs it with `--manifest-path ./userspace/ksud/Cargo.toml` for
`x86_64-pc-windows-gnu`, both Apple targets and both musl targets. The Apple ones need a
`macos-latest` runner and the rest build on `ubuntu-latest`, which is why the caller's
`build-ksud-extra` matrix in [`build-manager.yml`](../.github/workflows/build-manager.yml)
carries an `os` column alongside the target. It performs the same "Prepare LKM files" and
"Prepare ksuinit" steps before building, because a host ksud embeds those assets too -
they are what `boot-patch` on a desktop writes into the ramdisk. One step exists only here:
"Assemble LKM bootstrap" runs `clang --target=aarch64-linux-gnu -c -nostdlib` on the runner
and drops the object at `userspace/ksud/.lkm_image_bootstrap.o`, the prepared-object path
build.rs checks second. A cross container aimed at Windows or Darwin has no AArch64
assembler for build.rs to find, so handing the object in from outside is the only way that
build gets one.

## Where the artefacts go

Two copies, in two directions, and getting either wrong fails quietly at build time. Into
ksud: [`ksud/src/assets.rs`](ksud/src/assets.rs) declares a `rust-embed` `Asset` type whose
folder is chosen by cfg - `bin/x86_64` and `bin/aarch64` for the two Android targets, the
whole of `bin` for any host build, so host asset names there are arch-prefixed. rust-embed
bakes whatever is on disk at *compile* time, so the per-KMI `kernelsu.ko` files and the
ksuinit binary must sit in `userspace/ksud/bin/<arch>/` before `cargo build` runs.
[`ksud.yml`](../.github/workflows/ksud.yml) is a reusable `workflow_call` job that stages
them with its "Prepare LKM files" and "Prepare ksuinit" steps, both gated on the `pack_lkm`
and `pack_ksuinit` inputs and both filling `bin/aarch64` and `bin/x86_64`, since upstream
began building x86_64 LKMs and an x86_64 ksuinit alongside the arm64 pair; the ordering
lives in the caller, where the `build-ksud` job in
[`build-manager.yml`](../.github/workflows/build-manager.yml) declares
`needs: [build-lkm, build-ksuinit]` and passes both flags as true. Build ksud before those
artefacts land and nothing complains: `cargo build` succeeds, `list_supported_kmi()` finds
nothing whose name ends in `_kernelsu.ko` and returns an empty list, and
`ksud boot-info supported-kmis` on device, or `ksud supported-kmis` off it, prints an empty
listing. The error only surfaces much later, when someone runs `boot-patch` and
`assets::get_asset` fails with `Failed to load <kmi>_kernelsu.ko`.
[`ksud/bin/.gitignore`](ksud/bin/.gitignore) excludes `**/*.ko` and `**/ksuinit`, leaving
`aarch64/busybox`, `aarch64/bootctl` and `x86_64/busybox` as the only tracked contents, so a
fresh clone cannot `boot-patch` until the rest are fetched.

Out of ksud: the manager needs the binary as a native library, because
`getKsuDaemonPath()` in
[`KsuCli.kt`](../manager/app/src/main/java/me/weishu/kernelsu/ui/util/KsuCli.kt) returns
`ksuApp.applicationInfo.nativeLibraryDir` joined with `libksud.so`. Locally,
`just build_manager` copies `target/aarch64-linux-android/release/ksud` to
`manager/app/src/main/jniLibs/arm64-v8a/libksud.so` before invoking Gradle; that path is
gitignored, so a plain `./gradlew assembleRelease` yields an APK with no ksud in it. CI
closes the gap afterwards instead: [`repack_apk.py`](../repack_apk.py) maps each ABI to a
triple, reads `target/<triple>/<build-type>/ksud`, rewrites the APK with a fresh
`lib/<abi>/libksud.so` and re-signs. The `abiFilters` in
[`build.gradle.kts`](../manager/app/build.gradle.kts) are `arm64-v8a` and `x86_64`.

## Host builds, versions and lints

Off Android, the `#[cfg(target_os = "android")]` attribute on most of the `mod` lines in
[`ksud/src/main.rs`](ksud/src/main.rs) leaves `apk_sign`, `assets`, `boot_patch`, `defs`,
`lkm_image` and `lkm_image_btf` compiled; the last two carry no cfg at all, because offline
image injection is precisely the work a desktop is meant to do. The inverse attribute brings
in [`ksud/src/cli_non_android.rs`](ksud/src/cli_non_android.rs), which exposes `boot-patch`,
`boot-restore`, `boot-patch-v2`, `get-sign` and `supported-kmis`. `boot-patch-v2` is the new
one and it is different in kind: rather than swapping the ramdisk's `init`, `patch_boot()`
decompresses the kernel `Image`, recovers kallsyms and BTF out of it, appends the module in
a `KSULKM1` capsule past the link-time `_end`, and links the AArch64 bootstrap object into a
text cave so the kernel loads the capsule itself. Its whole surface is `--boot`, `--module`,
`--output` and `--force`, and its help text says outright that it only ever touches a boot
image, never `init_boot` or `vendor_boot`. The older `boot-patch` grew two flags of its own:
a host-only `--arch`, defaulting to `aarch64`, which decides whether the embedded `.ko` and
ksuinit are read out of `bin/aarch64` or `bin/x86_64`, and `--ramdisk`, which parses the
input as a bare cpio through `BootImage::parse_raw_ramdisk()` for AVD work and refuses to
combine with `--kernel` or with the Android-only `--flash`. A naked ramdisk carries no
kernel to detect a KMI from either, so `--ramdisk` bails with `please specify kmi manually`
unless `--kmi` or `--module` is given, or, on Android, `get_current_kmi()` already answered
from the running kernel. Bindgen does not run on a host build, because a host ksud never
issues an ioctl; `assemble_bootstrap()` does run, which is why an AArch64 assembler is a
prerequisite even for a Windows build. Whatever the target, [`ksud/build.rs`](ksud/build.rs)
stamps `VERSION_CODE` as `30000 + git rev-list --count HEAD`. That formula is duplicated
twice over: [`kernel/Kbuild`](../kernel/Kbuild) computes
`$(shell expr 30000 + $(KSU_GIT_VERSION))`, and `getVersionCode()` in
[`manager/build.gradle.kts`](../manager/build.gradle.kts) returns `30000 + commitCount` for
the app-level [`build.gradle.kts`](../manager/app/build.gradle.kts) to read back out of
`extra`. That is why [`ksud.yml`](../.github/workflows/ksud.yml) checks out with
`fetch-depth: 0`. A shallow clone counts one commit and stamps 30001 everywhere.

Lints are enforced twice, once in the source and once in CI.
[`ksud/src/main.rs`](ksud/src/main.rs) opens with
`#![deny(clippy::all, clippy::pedantic)]` and `#![warn(clippy::nursery)]`, then buys seven
pedantic lints back: `cast_possible_truncation`, `cast_sign_loss`, `cast_precision_loss`
and `cast_possible_wrap`, plus `module_name_repetitions`, `doc_markdown` and
`too_many_lines`. The four `cast_*` exemptions are the interesting half, because a crate
marshalling a C ABI narrows and re-signs integers on nearly every ioctl.
[`clippy.yml`](../.github/workflows/clippy.yml) sets `RUSTFLAGS: '-Dwarnings'`
workflow-wide and runs clippy for both Android triples from the workspace root, covering
both members. [`rustfmt.yml`](../.github/workflows/rustfmt.yml) pulls the nightly toolchain
through `dtolnay/rust-toolchain@nightly` and runs `cargo fmt --all --check` at the
workspace root, so both members are checked; there is no `rustfmt.toml` anywhere in the
tree, so what is enforced is nightly rustfmt's own defaults.

## See also

- [`ksud/README.md`](ksud/README.md) - the userspace daemon and CLI
- [`ksuinit/README.md`](ksuinit/README.md) - the ramdisk init shim
- [`uapi/README.md`](../uapi/README.md) - the ABI that bindgen turns into Rust
- [`manager/README.md`](../manager/README.md) - the Android app that ships `libksud.so`
- [`scripts/README.md`](../scripts/README.md) - build, packaging and repo automation
- [`docs/architecture.md`](../docs/architecture.md) - repository-wide hub

<!-- reference links: kernel documentation and man pages -->
[elf-5]: https://man7.org/linux/man-pages/man5/elf.5.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
