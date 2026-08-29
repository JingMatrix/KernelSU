# Repository automation

The kernel module, ksuinit, ksud and the manager APK are four build products that have to
be produced in that order. ksud embeds the compiled `kernelsu.ko` files and the `ksuinit`
binary in its own executable through `rust-embed`, and the manager APK must have that same
ksud injected *after* Gradle finishes and then be re-signed in a very particular way,
because the kernel module authenticates the manager by hashing its APK signing
certificate. Neither Cargo nor Gradle expresses those dependencies; the scripts and
workflows below enforce the ordering and perform the surgery the build systems cannot.

## setup_cargo_config.py

[`setup_cargo_config.py`](setup_cargo_config.py) generates `.cargo/config.toml` so that a
plain `cargo build --target aarch64-linux-android` works against a locally installed
Android NDK. Run it once per machine:

```sh
python3 scripts/setup_cargo_config.py [--ndk-root DIR] [--host-tag TAG]
                                      [--api-level 26] [--output PATH] [--stdout] [--force]
```

It needs Python 3.10 or newer and an NDK, which `detect_ndk_root()` locates from
`--ndk-root`, `ANDROID_NDK_ROOT`, `ANDROID_NDK_HOME`, or `ndk-bundle` and the
`ndk/<version>` directories under `$ANDROID_HOME` (or `$ANDROID_SDK_ROOT` when
`ANDROID_HOME` is unset), newest version first. It takes the first of those candidates
that actually has a `toolchains/llvm/prebuilt` directory, so a stale environment variable
pointing at a half-deleted NDK does not win over a good SDK-managed one.

The output carries two `[target.*]` linker lines and an `[env]` block with `CC_`, `CXX_`,
`AR_` and `BINDGEN_EXTRA_CLANG_ARGS_` for both Android triples. The file is gitignored and
per developer; the script refuses to overwrite one without `--force`, and `--force`
rewrites it whole, discarding any `rustflags` you had added by hand.
`BINDGEN_EXTRA_CLANG_ARGS_` is the entry that is easy to omit and expensive to debug.
[`userspace/ksud/build.rs`](../userspace/ksud/build.rs) runs bindgen over
`src/ksu_uapi.h` for every Android target, and bindgen drives libclang directly rather
than through the NDK clang wrapper, so it inherits no sysroot and no arch include path
from `CC_*`; without them it cannot find `<linux/types.h>` and the build dies inside
`build.rs`, before any link step. The live `[env]` block of
[`.cargo/config.example.toml`](../.cargo/config.example.toml) sets only
`CC_`/`CXX_`/`AR_`, so renaming that file verbatim, as its own header suggests, gives you
a broken build.

## ksubot.py

[`ksubot.py`](ksubot.py) publishes finished artifacts to a Telegram topic, called from
exactly one place: the `Upload to telegram` step of
[`build-manager.yml`](../.github/workflows/build-manager.yml), as
`python3 $GITHUB_WORKSPACE/scripts/ksubot.py $APK`. It needs `telethon` and takes
everything else from the environment (`BOT_TOKEN`, `CHAT_ID`, `MESSAGE_THREAD_ID`,
`COMMIT_URL`, `COMMIT_MESSAGE`, `RUN_URL`, `TITLE`, `VERSION`, `BRANCH`), and
`check_environ()` exits 1 if any is unset. That absolute invocation path is what makes the
session cache work: `main()` derives `script_dir` from
`os.path.dirname(os.path.abspath(sys.argv[0]))` and hands Telethon `<script_dir>/ksubot`,
to which it appends `.session`, so the login lands at `scripts/ksubot.session` -- exactly
the path the workflow caches under the key `${{ runner.os }}-bot-session` so the bot does
not re-authenticate every run.
`get_caption()` collapses the message to the bare commit URL past 1024 characters,
Telegram's caption limit, because an oversized caption makes `send_file` fail. The
`if __name__ == "__main__"` block wraps `asyncio.run(main())` in a bare `except Exception`
that only prints, so a Telethon failure leaves the step green. The `exit(1)` calls in
`check_environ()` raise `SystemExit`, which does not derive from `Exception` and so is not
caught: a missing environment variable still fails the job.

## allowlist.bt

[`allowlist.bt`](allowlist.bt) is an 010 Editor binary template for reading
`/data/adb/ksu/.allowlist` by hand, the file
[`kernel/policy/allowlist.c`](../kernel/policy/allowlist.c) writes from `task_work` on PID
1. Nothing in the build invokes it; it is a debugging aid, and it is stale. It models the
776-byte version 3 record, whose `root_profile` ends at `namespaces` plus padding.
[`uapi/app_profile.h`](../uapi/app_profile.h) is now `KSU_APP_PROFILE_VER 4`,
`struct root_profile` ends with a `__u64 flags` field, and `ksu_load_allow_list()` keeps
`kAppProfileSizePreV4 = 776` only to read legacy files, so the template desynchronises
after the first record of a current file. Its header handling still matches: magic
`0x7f4b5355`, then `FSeek(8)` past the `{magic, version}` pair.

## repack_apk.py

[`repack_apk.py`](../repack_apk.py) is the post-Gradle APK surgeon, at the repository root
rather than in `scripts/`. One subcommand:

```sh
python3 repack_apk.py repack -b release -t release -a arm64-v8a -a x86_64 \
    -K key.jks -A alias -P storepass -S keypass [-n BASENAME] [--strip] [-o dist]
```

Configuration merges built-in defaults, `repack-config.json` at the repository root (JSONC
only with the optional `json-with-comments` package; see
[`repack-config.example.json`](../repack-config.example.json)), then CLI flags.
`find_latest_apk()` takes the newest `*.apk` under
`manager/app/build/outputs/apk/<app_build_type>/`, and `find_ksud_binaries_by_arch()` maps
each requested ABI through `ARCH_TO_TRIPLE` to `target/<triple>/<ksud_build_type>/ksud`.
`-a` is an appending option, so one invocation carries as many ABIs as you hand it and
emits a single APK holding all of them; that is how CI produces one manager APK with both
`lib/arm64-v8a/libksud.so` and `lib/x86_64/libksud.so` inside it.
`repack_apk()` copies the input zip entry by entry into a new one, preserving each entry's
compression and attributes, dropping `lib/<abi>/` entries for unselected ABIs and any
`libksud.so` it is about to replace, and appending the fresh ksud deflated. The APK must
end up containing it, which `assert_required_libs()` checks: `getKsuDaemonPath()` in
[`manager/.../KsuCli.kt`](../manager/app/src/main/java/me/weishu/kernelsu/ui/util/KsuCli.kt)
returns `<nativeLibraryDir>/libksud.so`, and every root shell and ksud subcommand goes
through that path. Gradle never produces it, since
[`manager/app/src/main/jniLibs/.gitignore`](../manager/app/src/main/jniLibs/.gitignore)
ignores `libksud.so`, so a plain `./gradlew assembleRelease` APK is inert until repacked.

Rewriting the zip invalidates the signature, and the re-signing shape is dictated by the
kernel rather than by taste. `check_v2_signature()` in
[`kernel/manager/apk_sign.c`](../kernel/manager/apk_sign.c) locates the zip's central
directory from the end of the file, refuses the archive outright if a ZIP64 locator
(`0x07064b50`) sits in front of that record, checks that the `APK Sig Block 42` magic and
the block's two length fields agree, and then walks the block's id-value pairs. It
requires the count of v2 blocks (id `0x7109871a`) to be exactly one and returns false if a
v3 (`0xf05368c0`) or v3.1 (`0x1b93ad61`) block is present. Every read goes through
`read_exact()` and
`read_length_prefixed_end()`, which bound every length against the container enclosing it,
so a truncated or hostile block fails the walk instead of running off the end of the file.
The parser is deliberately shaped like AOSP's, on the principle that the kernel and the
framework should never disagree about which certificate signed an APK. Hence the pinned
`--v2-signing-enabled true` and `--v3-signing-enabled false`: flip either and
`check_v2_signature()` rejects the APK, so the app is never crowned. The other two flags
no longer decide the outcome. Upstream deleted the `META-INF/MANIFEST.MF` scan that used
to reject a v1-signed APK, and a v4 signature lives in a detached `.apk.idsig` file the
kernel never opens, so `--v1-signing-enabled false` and `--v4-signing-enabled false`
survive as hygiene rather than as requirements.

What that costs is identity, not access to the driver. The `[ksu_driver]` fd still installs,
because `reboot_handler_pre()` in
[`kernel/supercall/supercall.c`](../kernel/supercall/supercall.c) checks nothing but the
`KSU_INSTALL_MAGIC1`/`KSU_INSTALL_MAGIC2` argument pair before queueing `ksu_install_fd()`
as a `task_work`, which is how the [kprobe][kprobes] defers the fd installation out of its
atomic pre-handler and onto the calling task's return to userspace. Everything the fd is for
breaks instead. `is_manager()` stays false, so `only_manager()` and -- for a caller that is
not uid 0 -- `manager_or_root()` in [`kernel/supercall/perm.c`](../kernel/supercall/perm.c)
both return false, and the dispatch loop in
[`kernel/supercall/dispatch.c`](../kernel/supercall/dispatch.c) returns `-EPERM` before
reaching the handler. The app gets its fd and is refused every command it sends through it.

The preceding `zipalign -P 16 -f 4` page-aligns the shared libraries for 16 KB-page
devices. `--strip` strips the embedded ksud with the NDK `llvm-strip`,
close to a no-op for a release build since [`Cargo.toml`](../Cargo.toml) already sets
`strip = true`. Note that `-n` supplies a *base* name and the tool appends `.apk`, so a
value that already ends in `.apk` yields a doubled extension. CI passes no `-n` at all,
which leaves `output_name` empty and falls back to the input APK's stem, so `dist/` ends
up holding one file named after whatever Gradle produced.

## The justfile

[`../justfile`](../justfile) is the local-developer shortcut set: three recipes and two
aliases, `bk` for `build_ksud` and `bm` for `build_manager`.

| Recipe | What it runs |
| --- | --- |
| `build_ksud` | `cross build --target aarch64-linux-android --release` |
| `build_manager` | depends on `build_ksud`, copies ksud into `jniLibs`, runs `./gradlew aDebug` |
| `clippy` | `cargo fmt` (formats in place, not `--check`), then `cross clippy --target aarch64-linux-android --release` |

The copy lands at `manager/app/src/main/jniLibs/arm64-v8a/libksud.so`, the local
equivalent of what `repack_apk.py` does in CI, which is why a locally built debug APK
works without repacking. Every recipe goes through `cross`, which brings its own container
toolchain, so the justfile needs no NDK and no `.cargo/config.toml`.

It does need the rest of the toolchain, though: `just` itself, a container runtime for
`cross` to drive, and a JDK plus the Android SDK for the Gradle half. It also needs the
destination directory to exist. Git tracks only
[`manager/app/src/main/jniLibs/.gitignore`](../manager/app/src/main/jniLibs/.gitignore),
so a fresh clone has no `arm64-v8a/` under it and `just bm` dies at the `cp` with a
no-such-file error after an otherwise successful ksud build. Create that directory once
with `mkdir -p` and it stays out of the way.

## setup-rust-build.sh

[`../.github/scripts/setup-rust-build.sh`](../.github/scripts/setup-rust-build.sh) is the
CI counterpart of `setup_cargo_config.py`. It is *sourced*, not executed, so its exports
survive into the `cargo` invocation in the same step; it takes the target triple and the
Android API level as `$1` and `$2`:

```sh
export BINDGEN_EXTRA_CLANG_ARGS_$UTRIPLE="--sysroot=$LLVM_PATH/sysroot -I$LLVM_PATH/sysroot/usr/include/$TRIPLE"
```

It also leaves `TRIPLE` set for the caller, so the build step reads
`cargo build --target $TRIPLE`; the host tag `linux-x86_64` is hardcoded. Composing
variable names at runtime is why the script sits on the `ignore_paths` list of
[`shellcheck.yml`](../.github/workflows/shellcheck.yml).

## The CI graph

[`build-manager.yml`](../.github/workflows/build-manager.yml) is the only workflow that
runs the whole pipeline. It fires on pushes to `main`, `dev` and `ci` and on pull requests
to `main` and `dev`, both halves behind the same twelve-entry path filter, and on
`workflow_call` from [`release.yml`](../.github/workflows/release.yml). Seven jobs:

| Job | Needs | Produces |
| --- | --- | --- |
| `generate-key` | none | PR-only throwaway keystore, plus its certificate size and SHA-256 |
| `build-lkm` | `generate-key` | `aarch64-<kmi>-lkm` and `x86_64-<kmi>-lkm`, 16 artifacts |
| `build-ksuinit` | none | `ksuinit-aarch64`, `ksuinit-x86_64` |
| `build-ksud` | `build-lkm`, `build-ksuinit` | `ksud-aarch64-linux-android`, `ksud-x86_64-linux-android` |
| `build-ksud-extra` | `build-lkm`, `build-ksuinit` | `ksud-<target triple>` for five desktop targets |
| `build-manager` | `generate-key` | `manager-gradle` (an APK with no ksud in it), `mappings` |
| `repack-manager` | `build-manager`, `build-ksud`, `generate-key` | `manager`, i.e. the single `dist/*.apk` carrying both ABIs |

`build-ksud` waits on the two producers for a reason no compiler reports: `struct Asset`
in [`userspace/ksud/src/assets.rs`](../userspace/ksud/src/assets.rs) is a
`#[derive(RustEmbed)]` over `bin/aarch64` or `bin/x86_64`, and
[`ksud.yml`](../.github/workflows/ksud.yml) copies the downloaded `.ko` files and
`ksuinit` there before `cargo build`. Reorder the jobs and you get a ksud that compiles
cleanly and whose `list_supported_kmi()` is empty. `build-ksud-extra` needs the same two
producers for a slightly different reason: on a target that is not Android the `cfg` picks
the variant rooted at `bin` itself, so a desktop ksud embeds both architectures' modules
at once and can flash a device of either kind.

### Building the module

[`build-lkm.yml`](../.github/workflows/build-lkm.yml) is a matrix over eight KMI strings
(`android12-5.10` through `android17-6.18`), each calling
[`ddk-lkm.yml`](../.github/workflows/ddk-lkm.yml) inside
`ghcr.io/ylarod/ddk-min:<kmi>-<ddk_release>` with `--privileged`. A KMI, Kernel Module
Interface, is the ABI Android's Generic Kernel Image freezes per release and version pair,
and a module loads only on a kernel with a matching one, so the matrix produces sixteen
modules: eight for arm64 and eight for x86_64. `build-lkm.yml` no longer pins
`ddk_release`; the default lives in one place, `ddk-lkm.yml`'s input declaration, currently
`20260828`, so bumping the container image moves every KMI at once instead of leaving the
matrix and the reusable workflow disagreeing about which image they meant.

The aarch64 leg uses the container's prepared `KDIR` and is just
`CONFIG_KSU=m CC=clang make` in `kernel/`, which runs the default target of
[`../kernel/Makefile`](../kernel/Makefile):

```make
all: check_symbol
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) modules compile_commands.json -j$(shell nproc)
	./check_symbol $(ODIR)/kernelsu.ko $(KDIR)/vmlinux
```

[`kernel/tools/check_symbol.c`](../kernel/tools/check_symbol.c) keeps an unloadable module
from shipping. The module deliberately calls unexported kernel internals, so the tool walks
every [`SHN_UNDEF`][elf-5] symbol in the `.ko` and demands a defined match in the target
`vmlinux` symtab, and requires `__versions` to exist with size 0, i.e. built without
MODVERSIONS, whose CRCs would not match a kernel it was not compiled against. A missing
symbol fails the build instead of failing `insmod` on a device.

The x86_64 leg opens by undoing the first one. `make clean` in `kernel/` comes before
anything else, because `kernel/Makefile` sets `ODIR := $(MDIR)` and the aarch64 build
therefore left its objects and its `kernelsu.ko` in the source directory the second build
is about to hand to `M=`. The step then clears `ARCH` and `CROSS_COMPILE` before exporting
`ARCH=x86_64`, so the arm64 values the ddk-min image presets for its prepared `KDIR` cannot
leak into the x86 configure. Past that it has no prepared `KDIR` waiting for it, so it
builds one by hand out of `/opt/ddk/src/<kmi>` into `/opt/ddk/kdir-x64/<kmi>`:
`make O=$KDIR gki_defconfig`, then `scripts/config` to disable `LTO_CLANG`,
`LTO_CLANG_THIN`, `LTO_CLANG_FULL` and `THINLTO` and enable `LTO_NONE`, then
`make O=$KDIR modules_prepare`. An out-of-tree module built against an LTO objtree needs
the whole plugin chain, so forcing `LTO_NONE` is what makes a bare `modules_prepare`
sufficient. `android16-6.12` and `android17-6.18` additionally get
`CONFIG_CFI_ICALL_NORMALIZE_INTEGERS`, so the type signatures Clang hashes for
indirect-call checks match how those kernels were built; get that wrong and the module
loads but every call through a function pointer traps.

Four `sed` edits land on `scripts/mod/modpost.c` before any of that. The ddk-min image
ships a patched modpost inside its prebuilt aarch64 `kdir`, but the x86_64 tree is
configured from the untouched source tarball and builds its own host modpost, which would
then apply rules the aarch64 build never sees. Two of the edits comment out
`check_exports(mod)` and the `s->module = exp->module` assignment that follows from it:
this module calls kernel internals nobody exports, and modpost's job is to fail exactly
that. One marks `check_exports()` `__attribute__((unused))` so the now-uncalled function
does not trip `-Werror`. The last gives the generated `__version_ext_names` array an empty
string initialiser, since with no symbol versions to name the emitted `.mod.c` would
otherwise carry a bare `=` and fail to compile. Each edit is written to be idempotent, so
an image whose tarball already carries some of the fixes still builds.

Generating one pair of headers is the other thing this leg has to do for itself.
[`kernel/Kbuild`](../kernel/Kbuild) compiles the policy editor with
`-I$(srctree)/security/selinux` and its `include` subdirectory, because that code reaches
into headers the kernel does not export: `ss/policydb.h` in
[`kernel/selinux/sepolicy.c`](../kernel/selinux/sepolicy.c), `security.h` in
[`kernel/selinux/rules.c`](../kernel/selinux/rules.c), `objsec.h` in
[`kernel/selinux/selinux.c`](../kernel/selinux/selinux.c). Those headers in turn include
two that exist nowhere in the source tree. `security.h` pulls in `flask.h`, and `objsec.h`
pulls in `avc.h` which pulls in `av_permissions.h`; `scripts/selinux/genheaders` generates
both, into the objtree, while `security/selinux` itself is compiled. Hence the second
include line, `-I$(objtree)/security/selinux`. `modules_prepare` lays down the generated
configuration headers but stops well before compiling the [LSM][lsm], so nothing writes that
pair. The leg therefore asks for the LSM explicitly with
`make O=$KDIR security/selinux/built-in.a`, letting Kbuild run `genheaders` as a
prerequisite in this output tree. Earlier revisions copied the two headers out of the
container's aarch64 objtree, which worked only as long as both configurations agreed on
the class and permission tables; generating them where they will be included drops that
assumption.

The module is then built with `CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER=y`, an x86_64-only
option ([`kernel/Kconfig`](../kernel/Kconfig)) that replaces a kernel source patch with a
runtime patch of the hardened syscall dispatcher. The leg drives `make -C $KDIR` directly
instead of going through `kernel/Makefile`, which is why it never runs `check_symbol`.
`android17-6.18` gets a different command line from the rest: `M=$MDIR MO=$MDIR` where the
older branches pass `M=$MDIR src=$MDIR`, because 6.18 reworked external module builds to
take the output directory in `MO` and to export the module's source root as `srcroot`.
`kernel/Kbuild` reads that variable when it is set and falls back to `src` or
`$(srctree)/$(src)` otherwise, which is how `KSU_KERNEL_DIR` -- the root of the module's
own `-I` lines -- comes out the same on all eight branches.

PR builds thread a second accepted manager certificate through here. `generate-key` mints
an RSA-2048 keystore valid for one day, exports the DER certificate, and publishes its
byte length as `printf '0x%04x'` alongside its SHA-256; `ddk-lkm.yml` turns those into
`KSU_EXPECTED_SIZE2` and `KSU_EXPECTED_HASH2`, refusing a run where only one is set.
`kernel/Kbuild` catches half of that on its own, with a hard `$(error)` when
`KSU_EXPECTED_SIZE2` arrives without `KSU_EXPECTED_HASH2`; a lone `KSU_EXPECTED_HASH2` it
ignores silently, which is why the workflow tests both directions in shell first. The same
`-DEXPECTED_SIZE2` also trips a second pair of `#ifdef`s, in
[`kernel/supercall/dispatch.c`](../kernel/supercall/dispatch.c): `do_get_info()` and
`do_get_info_legacy()` both OR `KSU_GET_INFO_FLAG_PR_BUILD` from
[`uapi/supercall.h`](../uapi/supercall.h) into the flags they return, which the manager
reads back as `Natives.isPrBuild` and turns into an on-screen warning.

### ksuinit, ksud and the repack

[`ksuinit.yml`](../.github/workflows/ksuinit.yml) cross-builds the ramdisk init shim for
`aarch64-linux-android` and `x86_64-linux-android`, one `run:` step each, both sourcing
`setup-rust-build.sh <triple> 26` against an NDK r29 that `nttld/setup-ndk` installs into
the job. The Android triples matter: ksuinit is what the patched boot image runs as PID 1
-- `init.rs` asserts `getpid().is_init()` before it does anything -- and building it
against the NDK sysroot keeps its header and syscall surface identical to the rest of the
Android side. Four `RUSTFLAGS` entries go with that.
`-C target-feature=+crt-static` links it static, because nothing has mounted the
partitions that hold a dynamic loader at the point ksuinit runs.
`-C link-arg=-Wl,-z,max-page-size=16384` keeps the segments aligned for 16 KB-page
devices, the same constraint `zipalign -P 16` enforces on the APK.
`-Wno-unused-command-line-argument` comes third and only quiets the clang driver about the
compile-time flags rustc hands it on a link-only invocation. The last entry hands the
linker `libclang_rt.builtins-<arch>-android.a`, located through
`$CLANG_PATH --print-resource-dir`, so the compiler helper routines a static link would
otherwise leave undefined are present; the step runs `test -f` on that path first, turning
an NDK layout change into an obvious failure rather than a wall of undefined symbols. Its
position at the end is not cosmetic: a static archive only contributes the members that
resolve symbols the linker has already seen undefined, so the builtins have to come after
the objects that call them.

Each step uploads a single file, `ksuinit-aarch64` and `ksuinit-x86_64`. Naming the
artifacts by architecture rather than uploading one `target/**/release/ksuinit` tree is
what lets the consumers write `mv ksuinit-aarch64/ksuinit` and know exactly which binary
they moved. [`ksud.yml`](../.github/workflows/ksud.yml) downloads every artifact of the
run and stages both sets: `aarch64-android*-lkm/*_kernelsu.ko` into
`userspace/ksud/bin/aarch64/` and `x86_64-android*-lkm/*_kernelsu.ko` into
`userspace/ksud/bin/x86_64/`. The architecture prefix `ddk-lkm.yml` puts on its artifact
names is load-bearing here, since both legs name their file `<kmi>_kernelsu.ko` and a
single glob would collide sixteen modules into one directory. It then sources
`setup-rust-build.sh <target> 26` and builds. For pull requests it overrides
`KSU_PACKAGE_NAME` to `me.weishu.kernelsu.pr`; the compile-time default in `build.rs` is
`me.weishu.kernelsu`, which is not this fork's manager applicationId `org.matrix.su` (see
[`manager/app/build.gradle.kts`](../manager/app/build.gradle.kts)).
[`ksud-extra.yml`](../.github/workflows/ksud-extra.yml) stages the same way but builds
five desktop targets with `cross` pinned to rev `66845c1`. Its artifacts are named after
the target, not the runner: `x86_64-pc-windows-gnu`, `aarch64-unknown-linux-musl` and
`x86_64-unknown-linux-musl` all come off `ubuntu-latest`, while the two Darwin targets
need a `macos-latest` runner and an extra `rustup target add`. The upload path is
`target/<triple>/release/ksud*`, the trailing glob being what catches `ksud.exe` on the
Windows target.

One step in that workflow exists purely because of where `cross` runs the compiler.
[`userspace/ksud/build.rs`](../userspace/ksud/build.rs) assembles
`src/lkm_image_bootstrap.S` into `$OUT_DIR/lkm_image_bootstrap.o`, which `lkm_image.rs`
pulls in with `include_bytes!` and splices into a boot image during LKM injection. That
object is AArch64 machine code whatever host ksud itself runs on, because it executes on
the phone, and `validate_bootstrap_object()` enforces exactly that -- little-endian ELF64,
`ET_REL`, `e_machine` 183 -- before it will be embedded. `build.rs` looks for an assembler
in order: `KSU_LKM_BOOTSTRAP_OBJECT`, a prepared `.lkm_image_bootstrap.o` beside the
manifest, `KSU_LKM_BOOTSTRAP_CC`, `aarch64-linux-gnu-gcc`, the NDK clang found through
`ANDROID_NDK_HOME`, plain `clang`, and finally `llvm-mc`. None of those is guaranteed
inside the `cross` image that ends up compiling a mingw or Darwin target, and by then it
is too late to install one, so the workflow assembles the object on the runner itself with
`clang --target=aarch64-linux-gnu` and drops it at
`userspace/ksud/.lkm_image_bootstrap.o` -- the prepared path `build.rs` checks second, and
the reason that fallback exists at all. `ksud.yml` needs no such step: `setup-ndk` has
already put an NDK clang where `ndk_clang()` will find it.

`repack-manager` downloads `manager-gradle`, `ksud-aarch64-linux-android` and
`ksud-x86_64-linux-android` into `artifacts/`, then reconstructs the paths
`repack_apk.py` expects, `manager/app/build/outputs/apk/release/` and
`target/<triple>/release/ksud` for both triples, picks the PR or production keystore, and
runs the tool once with `-a arm64-v8a -a x86_64`. The result is one APK carrying a ksud
for each ABI, which is what a user installing on either kind of device needs, since
`getKsuDaemonPath()` resolves `libksud.so` inside whichever `nativeLibraryDir` the
platform picked at install time.

### Version stamping

Three build systems independently compute `30000 + git rev-list --count HEAD`:
`kernel/Kbuild`, `userspace/ksud/build.rs` and
[`manager/build.gradle.kts`](../manager/build.gradle.kts). A shallow clone makes that
count 1 and stamps everything 30001, and Android's `versionCode` is monotonic, so a bad
number poisons update flows. Every checkout in the build pipeline therefore sets
`fetch-depth: 0` -- the `build-manager` and `repack-manager` jobs, `ksuinit.yml`,
`ksud.yml`, `ksud-extra.yml`, and `deploy-website.yml` for its own reasons -- except
`ddk-lkm.yml`, where `Kbuild` compensates with its own `git fetch --unshallow` when it
finds a `.git/shallow` file. The four lint workflows use a bare `actions/checkout@v7` and
clone shallow. `clippy.yml` still runs `build.rs` and still stamps 30001 into what it
compiles, but none of those four jobs upload an artifact, so the wrong number never leaves
the runner.

That `Kbuild` block is also why the container's first command is
`git config --global --add safe.directory`: without it git refuses the differently-owned
checkout, `KSU_GIT_VERSION_VALID` is never set, and the module silently falls back to
`-DKSU_VERSION=16`.

### What gates a pull request

Five workflows run on `pull_request` against `main` and `dev`, all path-filtered:

| Workflow | Fires on | Checks |
| --- | --- | --- |
| [`clang-format.yml`](../.github/workflows/clang-format.yml) | `kernel/**/*.[ch]`, `kernel/.clang-format` | `make check-format` in `kernel/`, a `clang-format --dry-run --Werror` sweep |
| [`clippy.yml`](../.github/workflows/clippy.yml) | `userspace/**` | `cargo clippy` on both Android triples with `RUSTFLAGS: -Dwarnings` |
| [`rustfmt.yml`](../.github/workflows/rustfmt.yml) | `userspace/**` | `cargo fmt --all --check` on the nightly toolchain |
| [`shellcheck.yml`](../.github/workflows/shellcheck.yml) | `**/*.sh` | shellcheck, minus `gradlew`, `installer.sh` and `setup-rust-build.sh` |
| [`build-manager.yml`](../.github/workflows/build-manager.yml) | `manager/**`, `kernel/**`, `userspace/**`, `repack_apk.py`, `Cargo.toml`, `Cargo.lock`, plus the build scripts and workflows | the whole pipeline, repacked APKs included |

The last filter is worth reading in full before you conclude that a change does not build.
Alongside the three source trees it lists `repack_apk.py`, `Cargo.toml`, `Cargo.lock`,
`.github/scripts/setup-rust-build.sh`, and `build-manager.yml`, `build-lkm.yml`,
`ddk-lkm.yml`, `ksud.yml` and `ksud-extra.yml` themselves. `ksuinit.yml` is missing from
it, so a pull request that only edits the ksuinit workflow never exercises the pipeline it
belongs to.

Neither [`release.yml`](../.github/workflows/release.yml) nor
[`deploy-website.yml`](../.github/workflows/deploy-website.yml) ever runs on a pull
request. `release.yml` fires on `v*` tags and on `workflow_dispatch`, re-runs
`build-manager.yml` with `secrets: inherit`, and attaches artifacts to a GitHub Release,
though its `files:` globs do not match the directory names `download-artifact` creates, so
in practice only `manager/*.apk` is attached. `deploy-website.yml` builds VitePress
against the committed `website/yarn.lock` and publishes to GitHub Pages, but triggers only
on `workflow_dispatch` and on pushes to `main` or `website`, so website edits landed on
`dev` never deploy on their own. Dependency bumps come from
[`dependabot.yml`](../.github/dependabot.yml).

## Reproducing the pipeline locally

Build the module in the same `ddk-min` container CI uses, or with
[`kernel/build-all.sh`](../kernel/build-all.sh), which loops KMIs through
`ddk build <kmi> ODIR=$(realpath .)/out/<kmi> -e CONFIG_KSU=m`, then copies each result
out as a stripped `kernelsu-<kmi>.ko`. Its built-in list still ends at `android16-6.12`,
seven of the eight the CI matrix covers, so `android17-6.18` has to be named as the
script's one argument until the default catches up. `ODIR` has to be absolute:
`kernel/Makefile` hands it straight to `make -C $(KDIR) M=$(ODIR)`, and by then `-C` has
moved the build into the kernel tree, so a relative `out/<kmi>` would be created there
instead. Every KMI needs its own output directory, which is the whole reason the variable
is passed at all.

The x86_64 side has no `ddk build` equivalent, so it is split into a preparation script
and a build script that mirror the two halves of `ddk-lkm.yml`'s second step.
[`prepare-ddk-x64.sh`](prepare-ddk-x64.sh) does the expensive part once for all eight
KMIs: the four idempotent `modpost.c` edits, `gki_defconfig` into
`/opt/ddk/kdir-x64/<kmi>`, the LTO knobs, `CONFIG_CFI_ICALL_NORMALIZE_INTEGERS` for the
two newest branches, `modules_prepare`, and `security/selinux/built-in.a` to generate
`flask.h` and `av_permissions.h`. Its `KMIS`, `CLANGS` and `RUSTS` arrays share one index,
so each branch gets the toolchain it was built with -- `clang-r416183b` for
`android12-5.10`, `clang-r584948c` for `android17-6.18` -- prepended to `PATH` from
`/opt/ddk/clang/<version>`, with `/opt/ddk/rust/<version>` added for `android16-6.12` and
`android17-6.18`, the only two entries whose `RUSTS` slot is not `none`. Trusting whatever
compiler the container's `PATH` offers first is how you get an objtree that no longer
matches the KMI it claims.

[`kernel/build-all-x64.sh`](../kernel/build-all-x64.sh) then builds against those prepared
trees, taking the same optional space-separated KMI list and writing each module to
`out-x64/<kmi>/kernelsu.ko`. It carries its own copy of the three arrays and repeats the
same per-KMI `PATH` switch, under `ARCH=x86_64 LLVM=1 LLVM_IAS=1`, because the compiler has
to match on the compile pass as well as on the configure pass: an objtree prepared with
`clang-r584948c` and a module built with whatever clang `PATH` offered first is the same
mismatch one step later. It repeats the workflow's `MO=`/`src=` split, passing
`M=$MDIR MO=$ODIR` for `android17-6.18` and `M=$ODIR src=$MDIR` for the rest, and gives
`CONFIG_KSU=m CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER=y` on the command line rather than
through a config file. Nothing here strips or renames the output the way `build-all.sh`
does; the modules stay where they land, one directory per KMI, which is what makes a
second run of a single KMI cheap.

Build ksuinit and then ksud, staging the outputs into `userspace/ksud/bin/<arch>/` by
hand, since that directory's `.gitignore` excludes `**/*.ko` and `**/ksuinit`. Build the
APK with Gradle, then run `repack_apk.py` with your own keystore. Remember that the module
you just built pins a signing certificate: unless you also pass `KSU_EXPECTED_SIZE2` and
`KSU_EXPECTED_HASH2` for your key, a locally signed manager is never crowned and the app
reports no root at all. For a fast inner loop, use `just bm` instead.

## See also

- [`../docs/architecture.md`](../docs/architecture.md), how the layers fit together
- [`../kernel/README.md`](../kernel/README.md), build modes and the module's init order
- [`../uapi/README.md`](../uapi/README.md), the ABI these builds must keep in step
- [`../userspace/README.md`](../userspace/README.md), the Rust workspace and its targets
- [`../manager/README.md`](../manager/README.md), the app `repack_apk.py` completes
- [`../website/README.md`](../website/README.md), the documentation site and its build

<!-- reference links: kernel documentation and man pages -->
[elf-5]: https://man7.org/linux/man-pages/man5/elf.5.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[lsm]: https://docs.kernel.org/security/lsm.html
