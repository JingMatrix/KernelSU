# ksuinit: the ramdisk init shim

`ksuinit` is a small, statically linked Rust program that ksud writes into the boot
ramdisk as `/init`, displacing the stock init to `/init.real`. The kernel starts it as PID
1; it brings up just enough of a system to insert `/kernelsu.ko` into the running kernel,
then hands control to the real init and disappears. Its interesting half -- an ELF symbol
pre-resolver that makes an out-of-tree module loadable against a GKI kernel's trimmed
export list, plus the `.modinfo` rewrite that gets it past a vendor's edited version magic
-- lives in [`src/lib.rs`](src/lib.rs) and is linked into ksud as a library, so one loader
serves both boot-time and after-boot insertion. [`src/main.rs`](src/main.rs) and
[`src/init.rs`](src/init.rs) are the binary alone.

## What a boot ramdisk is, and why /init is the hook

An Android GKI device boots a kernel image plus a small cpio archive, the *ramdisk*,
carried in the `boot` partition (Android 12) or in a separate `init_boot` partition
(Android 13 and later). The kernel unpacks it into rootfs and execs `/init` as process 1.
Whatever sits at that path owns the machine before any filesystem is mounted, before
[SELinux][selinux] has a policy and before any Android service exists -- and if PID 1 exits,
`do_exit()` reaches `panic("Attempted to kill init! ...")`.

KernelSU has two ways into a kernel. Built into vmlinux, nothing needs to touch the
ramdisk. As a loadable module, the kernel image stays stock and `kernelsu.ko` must be
inserted by somebody holding `CAP_SYS_MODULE`, with `/proc/kallsyms` readable and
`/proc/sys/kernel/kptr_restrict` writable, before Android's SELinux policy loads and
starts denying all three. PID 1 in the initramfs is the one context where all of that
holds at once, which is why the LKM strategy is "become `/init`".

## PID 1 without the std runtime

The binary declares `#![no_main]` and exports a C entry point instead:

```rust
#[unsafe(no_mangle)]
pub unsafe extern "C" fn main(_argc: i32, argv: *const *const u8, envp: *const *const u8) -> i32 {
    let _ = init::init();
    unsafe {
        execve(cstr!("/init"), argv, envp);
    }
    0
}
```

The C runtime's startup code calls this symbol directly -- bionic's, since CI builds the
crate for the `*-linux-android` triples and links it statically -- so `std::rt::lang_start`
never runs. The comment above it gives the reason: std's start-up aborts when file
descriptors 0, 1 and 2 are not all open, and PID 1 from an initramfs has none of them. The
result of `init()` is discarded on purpose -- whatever failed inside, control must still
reach the [`execve`][execve-2], which forwards the raw `argv`/`envp` the kernel handed PID
1.

## Bringing up just enough system

`init()` calls `setup_kmsg()` before any mount. It probes `/dev/kmsg`, and because the
ramdisk's `/dev` is normally empty at this point, falls back to `mknodat(CWD, "/kmsg",
FileType::CharacterDevice, 0o666, makedev(1, 11))` -- major 1, minor 11 is the kmsg
device. `kernlog::init_with_device` then installs a `log::Log` implementation writing each
record as `<N>target[pid]: message`. With no console and no writable filesystem, the ring
buffer is the only place a post-mortem can come from.

`prepare_mount()` brings up exactly one filesystem now. Its single call to
`mount_filesystem("proc", "/proc")` creates the mountpoint with `mkdir`, swallowing
`ErrorKind::AlreadyExists` so a ramdisk that already ships an empty `/proc` is not a
failure, and then walks the new mount API: `fsopen`, `fsconfig_create`, `fsmount`,
`move_mount`. Sysfs used to go up beside it and no longer does: everything this shim reads
or writes -- `/proc/kallsyms`, `/proc/sys/kernel/kptr_restrict`,
`/proc/sys/kernel/printk_devkmsg` -- lives in procfs, so the second mount only added a
filesystem that had to be torn down again. A mountpoint that came up is pushed onto an
`AutoUmount` guard whose `Drop` detaches it with `UnmountFlags::DETACH` -- Android's first
stage mounts `/proc` itself, and a lazy detach cannot fail because something still holds a
reference. The guard is bound as `let _dontdrop = ...`, which is load-bearing: binding to
`_` drops it at once and unmounts `/proc` before a single symbol is read. `unlimit_kmsg()`
then writes `on` to `/proc/sys/kernel/printk_devkmsg`, whose default `ratelimit` would drop
a burst of warnings exactly when it matters.

## Is KernelSU already in this kernel?

Nothing should insert a second copy of KernelSU into a kernel that already has one built in,
and `has_kernelsu()` is the check that prevents it. Its preferred half, `has_kernelsu_v2()`,
issues a raw `reboot(0xDEADBEEF, 0xCAFEBABE, 0, &mut fd)`. On a KernelSU kernel
`reboot_handler_pre` in [`kernel/supercall/supercall.c`](../../kernel/supercall/supercall.c)
-- a [kprobe][kprobes] on `REBOOT_SYMBOL`, which
[`kernel/include/arch.h`](../../kernel/include/arch.h) defines as `__arm64_sys_reboot` or
`__x64_sys_reboot` -- recognises the magic pair and queues a `task_work_add(current, ...,
TWA_RESUME)` that installs the driver fd and writes its number into the fourth syscall
argument. The deferral is forced: a kprobe pre-handler runs in atomic context, where
`fd_install` and `copy_to_user` are illegal, while task_work runs on the return-to-user path
of the same syscall.

The probe then issues `KSU_IOCTL_GET_INFO`, falls back to `KSU_IOCTL_GET_INFO_LEGACY`,
closes the fd and reports success when `version != 0`; both carry `perm_check =
always_allow` in [`kernel/supercall/dispatch.c`](../../kernel/supercall/dispatch.c), so
unlike `GRANT_ROOT` (`allowed_for_su`) or `REPORT_EVENT` (`only_root`) they answer any
caller at all. That callback is KernelSU's own uid-and-manager test rather than an SELinux
decision, and PID 1 running as uid 0 would clear `only_root` too; what `always_allow` buys
is that any caller, including an unrecognised manager, can ask. Nothing reboots by
accident, since `0xDEADBEEF` is not `LINUX_REBOOT_MAGIC1` (`0xfee1dead`): without KernelSU
the call returns `-EINVAL` and `fd` stays `-1`.

Both ioctl numbers are hardcoded (`0x80104b02`, `0x80004b02`) because ksuinit runs no
bindgen and never includes [`uapi/supercall.h`](../../uapi/supercall.h). `_IOR` folds
`sizeof` into the command number, so growing `struct ksu_get_info_cmd` would make the
first ioctl match no entry of the dispatch table and fall out of the search loop as
`-ENOTTY`. Detection would survive that, but only because the fallback is spelled
`_IOC(_IOC_READ, 'K', 2, 0)` and encodes no size at all; what the probe loses is the
`uapi_version` field that only the newer struct carries. `has_kernelsu_legacy()` probes
`prctl(0xDEADBEEF, 2, &version)`; this fork's kernel has no prctl hook, so it only detects
a foreign, upstream-style KernelSU.

## Loading kernelsu.ko past the GKI export list

A GKI kernel exports a small curated `__ksymtab`, and the module loader resolves an
[`SHN_UNDEF`][elf-5] symbol only against that table, so a plain
[`init_module(2)`][init-module-2] on `kernelsu.ko` would fail with "Unknown symbol" -- or
`-EACCES`, "Protected symbol", on a protected ABI
-- for nearly everything the module calls. `load_module()` in [`src/lib.rs`](src/lib.rs)
sidesteps the resolver by binding those symbols itself before the kernel sees them.

### Harvesting /proc/kallsyms

`kernel_symbols_iter()` first constructs a `Kptr` guard that reads
`/proc/sys/kernel/kptr_restrict`, writes `1`, and restores the old value on drop. Android's
init.rc raises that sysctl to 2, for which `kallsyms_show_value()` returns false
unconditionally while `s_show` still prints the address with `%px` -- a suppressed value
prints as sixteen ASCII zeros, which parse cleanly as address 0, so a scan under that
setting binds every symbol to NULL and the module calls into a table of them. Value 1 is
the smallest weakening that avoids it: `kallsyms_show_value()` falls through from case 1
to a `security_capable(cred, ..., CAP_SYSLOG)` test, and PID 1 passes that.

Which path actually needs the guard is worth being exact about. `kptr_restrict` is a plain
`int` in `lib/vsprintf.c` with no build-time initialiser, so it is still 0 while ksuinit
runs, and case 0 falls through to the same `CAP_SYSLOG` test -- from the ramdisk the
addresses come out real whether or not the sysctl is touched. What the guard buys is that
the identical function still works from ksud on a booted system, where init.rc has already
set the value to 2. The kernel latches the decision at open time from `file->f_cred`, so
`KptrOwnedIter` carries the guard inside the iterator to keep it alive across the open.

The scan is a `map_while`: field 0 parses as hex, `nth(1)` skips the type letter and
yields the name, and `take_if(|_| splits.next().is_none())` rejects any line with a fourth
field. That rejection is the stop condition, not a filter -- `s_show` prints core-kernel
symbols as three fields and module symbols as `%px %c %s\t[%s]`, and every vmlinux symbol
precedes the first module symbol, so a four-field line means the rest of the file need not
be read. Each name is truncated at the first `$` or `.llvm.`, folding clang's LTO-renamed
file-local symbols -- `foo.llvm.1234567` and the like -- onto the plain name an undefined
reference uses. Only the `.llvm.` half of that test has work to do in practice: the kernel
builds `/proc/kallsyms` from the output of `scripts/mksysmap`, which greps away every line
whose symbol begins with `$`, so aarch64's `$x` and `$d` mapping symbols never reach the
file.

### Rewriting the symbol table

`load_module()` parses the image with goblin and collects every `SHN_UNDEF` entry of
`.symtab` -- skipping index 0, matching the kernel's own `for (i = 1; ...)` in
`simplify_symbols()` -- into a map from name to `(Sym, byte offset)`. It then streams
kallsyms, and on each hit:

```rust
if let Some((mut sym, offset)) = unresolved_symbols.remove(symbol) {
    sym.st_shndx = section_header::SHN_ABS as usize;
    sym.st_value = *addr;
    buffer.pwrite_with(sym, offset, ctx)?;
}
```

Both stores are needed. `simplify_symbols()` handles `SHN_ABS` with the comment `/* Don't
need to do anything */` and takes `st_value` as final, so `SHN_ABS` with a zero value
passes every check and produces a NULL call. The callback returns
`!unresolved_symbols.is_empty()`, ending the scan the moment the last symbol binds.
Anything still unresolved gets only a `log::warn!("Cannot find symbol: {}", name)` and the
load proceeds -- so the evidence worth reading after a failed boot is the `Cannot find
symbol:` lines preceding the kernel's own complaint.

[`init_module`][init-module-2] rather than [`finit_module`][init-module-2] is used because
the patched image exists only in memory, and rewriting `.symtab` invalidates any appended
module signature, so this works only where unsigned modules load. That every undefined
symbol really exists in the target vmlinux is checked at build time, not at boot, by
[`kernel/tools/check_symbol.c`](../../kernel/tools/check_symbol.c) run from
[`kernel/Makefile`](../../kernel/Makefile), which also insists `__versions` be present with
size 0 -- nothing here supplies MODVERSIONS CRCs.

### When the kernel refuses the version magic

Every module carries a `vermagic=` string in `.modinfo` -- kernel release, SMP, preemption
model, a few ABI details -- and `check_modinfo()` compares it against the running kernel's
own early in `load_module()`, before a single relocation is applied. Vendors edit that
string; some vivo kernels splice their own name into it. A module built for the right KMI
is then refused with `-ENOEXEC` even though every symbol the previous section bound is
correct. The kernel prints what it wanted before returning: `%s: version magic '%s' should
be '%s'`. That log line is the only place the expected string is exposed to userspace, so
`load_module()` reads it back out of the ring buffer and tries once more.

Capture has to be armed before the load, not after it. `open_kmsg_at_end()` opens
`/dev/kmsg` -- falling back to the `/kmsg` node `setup_kmsg()` created, since in an
initramfs the first path does not exist -- with `O_NONBLOCK`, then seeks to
`SeekFrom::End(0)`. Every open of `/dev/kmsg` gets its own read cursor into the shared ring
buffer, so seeking to the end first means whatever is read afterwards belongs to this
`init_module` call rather than to the boot that preceded it. `O_NONBLOCK` is what lets the
drain terminate at all: `read_new_kmsg()` takes one record per `read` into an 8 KiB buffer
and treats `ErrorKind::WouldBlock` as end of data, where a blocking descriptor would sit
waiting for the next kernel message. A kmsg that will not open costs the retry, not the
load; the failure is a warning and `init_module` runs regardless.

`extract_required_vermagic()` walks the captured lines newest first, drops the
`level,seq,timestamp,flags;` prefix every `/dev/kmsg` record carries by splitting once on
`;`, then locates `version magic '` and takes what follows the `' should be '` separator up
to the next quote. What it keeps is the kernel's own string, not the module's rejected one,
which is the half of that message the retry has any use for. Nothing is matched against the
module's own name, so the parser works whatever the `.ko` is called. When no line matches,
the first `init_module` error is returned with the context `init_module failed without
vermagic mismatch` and the whole captured log goes out through `log::error!` -- that dump
is what to read when a load fails for any other reason.

Editing `.modinfo` where it lies is not an option, because the replacement can be longer
than the string it replaces and the section is packed between its neighbours.
`replace_module_vermagic()` rebuilds it elsewhere instead: it splits the old contents on
NUL into `key=value` entries, copies each one except `vermagic=`, writes the new value in
that entry's place -- appending it if the module carried none -- then parks the result at
an `align_up()`-ed offset past the end of the buffer and retargets the section header,
writing `sh_offset` at `+0x18` and `sh_size` at `+0x20` of its `Elf64_Shdr`. The kernel
reaches `.modinfo` through the section table like every other section, so where the bytes
physically sit does not matter. ELF32 is refused outright: `elf.is_64` false is a `bail!`,
and both stores are hardcoded to 64-bit field offsets.

Forging the version magic switches off the one check that keeps a module built for a
different kernel out, and what that check prevents is an oops inside module init rather
than a tidy `-ENOEXEC`. Two things make it defensible here. The value is not invented --
the kernel named it, so the module is being moved toward this kernel rather than away from
it -- and the load has no other guarantees left to lose: `.symtab` has already been
rewritten so nothing resolves through the export table, the appended signature is void,
and `check_symbol.c` has forced `__versions` to size 0.

## Module parameters from /ksu_config

`load_module_from_path()` asserts `rustix::process::getpid().is_init()` -- the ramdisk
path is unreachable from anywhere else -- reads `/kernelsu.ko`, then reads `/ksu_config`
with `unwrap_or_default()` and passes the bytes straight to `init_module` as the parameter
string. The tokens are module parameters declared in
[`kernel/core/init.c`](../../kernel/core/init.c): `module_param(allow_shell, bool, 0)` and
`module_param_named(norc, ksu_no_custom_rc, bool, 0)`, the second read in exactly one
place: `load_module_rc_once()` in
[`kernel/runtime/ksud_integration.c`](../../kernel/runtime/ksud_integration.c), where a
true value returns early and skips the `modules.rc` the function would otherwise read out
of `/metadata`. It does not disable the init.rc injection. KernelSU's own `KERNEL_SU_RC`
block is a compile-time string whose length is `sizeof(KERNEL_SU_RC) - 1` unconditionally,
and the `read`/`read_iter` proxies append it to what init sees whatever `norc` says; only
the extra module rc goes away.

Parameters can only be set at load time, and when ksuinit calls `init_module` the
initramfs is the only thing mounted. `/metadata` and `/data`, the two places the module
later goes looking for files of its own, do not exist yet, so a cpio entry beside the `.ko`
is the only channel through which a boot-time choice can reach `kernelsu_init()`.

## Handing control to the real init

```rust
unlink("/init")?;

let real_init = match access("/init.real", Access::EXISTS) {
    Ok(_) => "init.real",
    Err(_) => "/system/bin/init",
};

log::info!("init is {}", real_init);
symlink(real_init, "/init")?;
```

Overwriting `/init` in place is impossible -- it is the running executable, so a write
returns `ETXTBSY` -- but unlinking it is legal, because the running image holds the inode
open. Hence the order. Making `/init` a symlink rather than `execve`ing `/init.real`
directly is what makes the handover durable, because Android's init re-executes `/init`
for its later stages.

## What ksud writes into the ramdisk

The producer is `patch()` in [`../ksud/src/boot_patch.rs`](../ksud/src/boot_patch.rs).
After unpacking the boot image and extracting the cpio it does:

```rust
let is_kernelsu_patched = cpio.exists("kernelsu.ko");

if !is_kernelsu_patched && cpio.exists("init") {
    cpio.mv("init", "init.real")?;
}

cpio.add("init", CpioEntry::regular(0o755, ksu_init))?;
cpio.add("kernelsu.ko", CpioEntry::regular(0o755, kernelsu_ko))?;
```

The `is_kernelsu_patched` guard makes re-patching idempotent: without it a second
`boot-patch` run would move ksuinit itself onto `init.real` and the stock init would be
lost for good. `ksu_init` is the `ksuinit` asset embedded in ksud, or a file given with
`--init`; `kernelsu_ko` is the `{kmi}_kernelsu.ko` asset for the detected KMI, or
`--module`. The same function rewrites the `ksu_config` entry as a space-separated token
list, adding or removing `norc=1` and `allow_shell=1`; `rebuild_without_ksu()` is the
inverse used by `ksud boot-restore`. Four names are a contract between patcher and shim --
`init`, `init.real`, `kernelsu.ko`, `ksu_config` -- and renaming one means editing both
sides.

Where those two assets come from differs on and off the device. An on-device ksud embeds
one architecture's `bin/` subdirectory and asks for `ksuinit` and `{kmi}_kernelsu.ko` by
bare name; a host build embeds `bin/` whole, both subdirectories at once, and prefixes each
request with `--arch` (`aarch64` unless told otherwise), so `patch()` fetches
`x86_64/ksuinit` when pointed at an emulator image. `--ramdisk` belongs to the same
host-side workflow: it makes `patch()` read its input through
`BootImage::parse_raw_ramdisk()` instead of `BootImage::parse()`, so a bare cpio such as
the AVD's `ramdisk.img` can be patched directly. No kernel is present in such a file to
sniff a KMI out of, which is why `--ramdisk` insists on an explicit `--kmi` and refuses
both `--kernel` and the on-device `--flash`.

## The same loader after boot: late-load and insmod

`run()` in [`../ksud/src/late_load.rs`](../ksud/src/late_load.rs) calls
`ksuinit::has_kernelsu()` and then `ksuinit::load_module(&ko_data, params)` with the `.ko`
taken from ksud's embedded assets instead of a ramdisk -- the path that gets root onto a
device whose boot image cannot be replaced. `insmod()` in
[`../ksud/src/debug.rs`](../ksud/src/debug.rs) exposes the same function for any module.
The kernel tells the two apart by one line in `kernelsu_init()`:

```c
#ifdef MODULE
    ksu_late_loaded = (current->pid != 1);
#else
    ksu_late_loaded = false;
#endif
```

Loaded by ksuinit, `current->pid == 1` and the module takes the early-boot branch, whose
`ksu_ksud_init()` hooks `__NR_read` and `__NR_fstat` so the injected init.rc block appears
in what init reads, and registers the `input_event` kprobe behind the volume-down
safe-mode check. A late load skips that entirely: it applies the SELinux rules itself,
escalates the calling ksud into the `ksu` domain, sets `ksu_boot_completed` and re-enables
enforcing. One difference matters when reading logs: `setup_kmsg()` lives in the binary,
so from ksud the library's `Cannot find symbol` goes to ksud's logger, not to `dmesg`. The
descriptor the vermagic retry reads from is the library's own and works for either caller,
since on a booted system `/dev/kmsg` exists and ksud holds the `CAP_SYSLOG` that
`dmesg_restrict` demands of a reader.

## The route that bypasses this shim

`ksud boot-patch-v2`, in [`../ksud/src/lkm_image.rs`](../ksud/src/lkm_image.rs), arrives at
the same destination -- `kernelsu.ko` running inside a kernel that was not built with it --
without a ramdisk shim at all. It decompresses the kernel out of the boot image, recovers
the symbol table from the compiled-in kallsyms blob and, when the image
carries a usable one, `struct load_info`'s layout from `.BTF`
(via [`../ksud/src/lkm_image_btf.rs`](../ksud/src/lkm_image_btf.rs)), performs offline the
same `SHN_ABS` binding `load_module()` performs at runtime, appends the relocated module
past the image's `_end` as a `KSULKM1` capsule, and redirects the `async_synchronize_full()`
call inside `kernel_init()` to a bootstrap
([`../ksud/src/lkm_image_bootstrap.S`](../ksud/src/lkm_image_bootstrap.S)) that validates
the capsule and hands it to the kernel's own internal `load_module()`. `/init` is never
touched, so none of this crate runs.

The two are alternatives, not stages of one flow, and they trade different things away.
`boot-patch-v2` rewrites the kernel image, is arm64-only, works on a `boot` image and never
on `init_boot` or `vendor_boot`, and has to recover that particular image's compiled-in
kallsyms table before it can bind a single symbol; when that recovery fails there is
nothing to fall back on. `.BTF` is the softer of the two dependencies -- all it supplies is
the layout of `struct load_info`, so an image without a usable one drops to the built-in
`GKI_ABI` constants and says so with `BTF: unavailable; using built-in GKI ABI`.
`boot-patch` leaves the kernel byte-exact and only edits the ramdisk, needs nothing from
the image beyond a KMI string, covers both architectures, and pays for it with a shim that
must survive as PID 1 on every boot. Which one a device gets is a choice made at patch time
by the subcommand name.

## Building and shipping

[`Cargo.toml`](Cargo.toml) keeps the dependency list to what a PID 1 with no libc of its own
can justify. goblin and scroll parse and rewrite the ELF; `syscalls` issues the
[`reboot`][reboot-2], [`prctl`][prctl-2] and [`ioctl`][ioctl-2] of the KernelSU probe as
bare `syscall!` invocations, with no typed wrapper between the magic constants and the
kernel; kernlog sits behind the `log` facade and writes to whichever kmsg device
`setup_kmsg()` managed to open; anyhow carries the errors across the `?` operators. Nothing
here is a `-sys` crate and nothing runs bindgen, which is why the ioctl numbers a few
sections up are literals rather than generated constants.

rustix is the one dependency pinned to a git revision:
`https://github.com/Kernel-SU/rustix.git` at rev `4a53fbc`, with the `mount`, `fs`,
`runtime`, `system` and `process` features. That checkout is what supplies
`rustix::system::init_module`, the new-mount-API trio `fsopen`/`fsconfig_create`/`fsmount`
and `rustix::runtime::execve` in the shapes [`src/lib.rs`](src/lib.rs),
[`src/init.rs`](src/init.rs) and [`src/main.rs`](src/main.rs) call them. A bump to a
crates.io rustix is therefore not a version bump but a port.

[`.github/workflows/ksuinit.yml`](../../.github/workflows/ksuinit.yml) builds the crate
twice, once for `aarch64-linux-android` and once for `x86_64-linux-android`, sourcing
[`.github/scripts/setup-rust-build.sh`](../../.github/scripts/setup-rust-build.sh) to aim
`CC_*`, `AR_*` and `CARGO_TARGET_*_LINKER` at the API-26 clang of NDK r29 for the triple in
hand. What makes an Android triple usable for a PID 1 is `-C target-feature=+crt-static` in
`RUSTFLAGS`: an initramfs has no dynamic loader and no `libc.so`, so the binary has to
carry static bionic rather than expect anything to be resolved at exec time. The same flags
put the NDK's `libclang_rt.builtins-<arch>-android.a` on the link line by absolute path,
behind a `test -f` so that an NDK layout change fails the job loudly instead of yielding a
binary short of compiler-support routines, and add `-Wl,-z,max-page-size=16384` for kernels
with 16 KiB pages. [`build.rs`](build.rs) is left over from the musl era: it appends `-lc`
for `aarch64-unknown-linux-musl` and `x86_64-unknown-linux-musl`, where the default link
order places libc ahead of the `compiler_builtins` reference to `getauxval` and only a
second copy at the end of the command line resolves it. CI builds neither of those triples,
so nothing shipped passes through that hook; a hand-built musl binary still does.

That workflow does nothing further than upload the two binaries as the artifacts
`ksuinit-aarch64` and `ksuinit-x86_64`. The `Prepare ksuinit` step of
[`.github/workflows/ksud.yml`](../../.github/workflows/ksud.yml) moves them into
`userspace/ksud/bin/{aarch64,x86_64}/` before ksud is compiled, because rust-embed bakes in
whatever is on disk at compile time -- and with the `debug-embed` feature enabled, in debug
builds too, not only in release. Those directories are gitignored down to `**/ksuinit` and
`**/*.ko`, so a from-source `ksud boot-patch` with no CI artifacts staged fails inside
`assets::get_asset()`, which asks for `ksuinit` on Android and `{arch}/ksuinit` on a host
build.

## When it fails

A failed module load is not fatal: `init()` logs `Cannot load kernelsu.ko`, carries on to
the handover, and the device boots stock without root. The dangerous failures break the
handover instead. If `unlink("/init")` succeeds but `symlink` fails, `init()` returns
`Err`, `main` ignores it, `execve("/init")` finds nothing, PID 1 returns 0 and the kernel
panics. If `unlink` fails, `/init` still resolves to ksuinit and the exec re-enters the
shim in a loop. A ksuinit for the wrong architecture, or a ramdisk the bootloader rejects,
means the kernel cannot exec `/init` at all; a `kernelsu.ko` built against a different
kernel can load and then panic inside `kernelsu_init()`.

None of these is fixable from inside the device, because nothing after `/init` gets a
chance to run. Recovery means flashing a stock boot image, which is why `boot-patch` keeps
one when it can. The call to `do_backup()` sits behind `#[cfg(target_os = "android")]` and
`if (backup || (!is_kernelsu_patched && flash))`, so it runs for an on-device `--flash` of
an image that is not already patched, or whenever `--backup` is passed explicitly; a
host-side patch, and a re-patch of an already-patched image without `--backup`, keep
nothing. When it does run it copies the source partition to
`/data/adb/ksu/ksu_backup_<sha1>` -- or, if `find_backup_location()` cannot open a file
there, to `ksu_backup_<sha1>` inside a `/data/user_de/<user>/<pkg>/boot_backup` it wipes
and recreates, so that fallback holds one image and no more -- and records that SHA-1 as a
`stock_image.sha1` cpio entry. `restore()` uses the pair to reproduce a byte-exact stock
image, falling back to `rebuild_without_ksu()` when the backup is gone. From a working
system or a recovery shell that is `ksud boot-restore --flash`; with no system at all, a
`fastboot flash`. A device that boots the kernel but hangs later is a different problem,
covered in [`website/docs/guide/rescue-from-bootloop.md`](../../website/docs/guide/rescue-from-bootloop.md).

## See also

- [`../README.md`](../README.md) --
  the Rust workspace and how the two crates relate
- [`../ksud/README.md`](../ksud/README.md) --
  the daemon that builds the ramdisk and reuses this loader
- [`../../docs/boot-patching.md`](../../docs/boot-patching.md) --
  how this binary is installed as `/init`, and the alternative that skips the ramdisk
- [`../../kernel/core/README.md`](../../kernel/core/README.md) --
  `kernelsu_init()` and the module parameters
- [`../../kernel/runtime/README.md`](../../kernel/runtime/README.md) --
  the init.rc injection and the `modules.rc` append that `norc=1` disables
- [`../../kernel/supercall/README.md`](../../kernel/supercall/README.md) --
  the `[ksu_driver]` fd this crate probes
- [`../../uapi/README.md`](../../uapi/README.md) --
  the ioctl numbers hardcoded in `src/lib.rs`
- [`../../docs/architecture.md`](../../docs/architecture.md) --
  where this step sits in the boot flow

<!-- reference links: kernel documentation and man pages -->
[elf-5]: https://man7.org/linux/man-pages/man5/elf.5.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
[ioctl-2]: https://man7.org/linux/man-pages/man2/ioctl.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[prctl-2]: https://man7.org/linux/man-pages/man2/prctl.2.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
