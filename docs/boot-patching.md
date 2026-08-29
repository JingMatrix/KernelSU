# Patching the boot image

Installing KernelSU on a GKI device means arranging for `kernelsu.ko` to be loaded by PID 1,
before Android's `init` runs. This document is about how that is arranged, and why each step
is the way it is.

Two subcommands of [`ksud`](../userspace/ksud/README.md) do the arranging, and they are
independent of one another. `ksud boot-patch`
([`boot_patch.rs`](../userspace/ksud/src/boot_patch.rs)) rewrites the ramdisk so that
[`ksuinit`](../userspace/ksuinit/README.md) becomes `/init`. `ksud boot-patch-v2`
([`lkm_image.rs`](../userspace/ksud/src/lkm_image.rs)) rewrites the arm64 kernel `Image`
itself, for images with no ramdisk `/init` to take over. `ksud boot-restore` undoes the first.
Image *format* handling belongs to none of them; it is an out-of-tree crate.

- [Why it has to happen that early](#why-it-has-to-happen-that-early)
- [How Linux actually starts userspace](#how-linux-actually-starts-userspace)
- [Where the code lives](#where-the-code-lives)
- [The format library](#the-format-library) - `android-bootimg`, header parsing through cpio
- [Strategy 1: become `/init`](#strategy-1-become-init) - `ksud boot-patch`
- [Strategy 2: inject into the kernel Image](#strategy-2-inject-into-the-kernel-image)
- [Restoring](#restoring) - `ksud boot-restore`

The first two sections are the motivation and the mechanism. After those, either strategy
section stands on its own.

## Why it has to happen that early

The module could be loaded later -- `ksud insmod` does exactly that -- but several things it
does are only possible before userspace starts.

The clearest is `init.rc` injection. The module appends its own content by proxying the
[`read()`][read-2] that init issues on that file, and it recognises the right file by
watching init's very first read of it. Arm that hook after init has already parsed its
configuration and there is nothing left to intercept.

The boot milestones are the same shape. `post-fs-data`, module mounting, the first
`app_process` -- the module's state machine is driven by events that all occur in the first
seconds, and a module that arrives afterwards has to assume they happened.

[SELinux][selinux] is subtler, and the late-load branch of `kernelsu_init()` shows exactly
what it costs. Loaded early, the module installs its own domain into the live policy and
everything that later needs it simply finds it there. Loaded late, onto a system already
enforcing, it has to `apply_kernelsu_rules()`, `cache_sid()`, then grant *itself* the new
domain with `escape_to_root_for_init()` before it can keep touching `/data/app` -- and if it
found SELinux permissive it re-enables enforcement by hand afterwards. All of that is
reconstruction work that the early path never has to do.

A module loaded after boot has missed all of that. The kernel side reflects this directly:
`ksu_late_loaded = (current->pid != 1)` in [`kernel/core/init.c`](../kernel/core/init.c)
splits `kernelsu_init()` into two quite different bring-up paths, and the late one has to
reconstruct by hand what the early one gets for free. See
[`kernel/runtime/README.md`](../kernel/runtime/README.md).

So the question is: how do you get a kernel module loaded by PID 1 on a device whose boot
image you can rewrite but whose kernel you cannot rebuild?

## How Linux actually starts userspace

The [**initramfs**][initramfs] is a cpio archive carried in the boot image. Early in boot the
kernel decompresses it and unpacks it into rootfs -- `unpack_to_rootfs()` in
`init/initramfs.c` -- creating a real, writable filesystem in memory. That work is scheduled
asynchronously, as `do_populate_rootfs()`.

Then PID 1 runs `kernel_init()` in `init/main.c`, and the order of what follows is the whole
story:

```c
static int __ref kernel_init(void *unused)
{
        wait_for_completion(&kthreadd_done);
        kernel_init_freeable();
        /* need to finish all async __init code before freeing the memory */
        async_synchronize_full();          /* <-- rootfs is now fully unpacked      */

        system_state = SYSTEM_FREEING_INITMEM;
        kprobe_free_init_mem();
        ftrace_free_init_mem();
        free_initmem();                    /* <-- .init.text ceases to exist        */
        mark_readonly();                   /* <-- kernel text becomes read-only     */

        system_state = SYSTEM_RUNNING;
        ...
        if (ramdisk_execute_command) {
                ret = run_init_process(ramdisk_execute_command);   /* execs "/init" */
```

`ramdisk_execute_command` is initialised to the string `"/init"` and is overridden only by an
`rdinit=` command line argument. So PID 1 execs whatever file happens to sit at `/init` in the
archive that was just unpacked.

Two independent openings follow, and they are exactly the two strategies:

- **Whatever file is at `/init` becomes PID 1.** Put a different program there and it runs
  with full kernel privilege before anything else in userspace exists.
- **There is a moment inside `kernel_init()`** -- after `async_synchronize_full()` returns,
  before `run_init_process()` -- when rootfs is populated, the allocator and scheduler are up,
  and userspace has not started. Kernel code executing there can load a module itself.

| | `ksud boot-patch` | `ksud boot-patch-v2` |
| --- | --- | --- |
| Exploits | `/init` is whatever the archive says | the gap inside `kernel_init()` |
| Modifies | the **ramdisk** | the **kernel Image** |
| Needs a ramdisk | yes | no |
| Architecture | arm64, x86_64 | arm64 only |
| Source | [`boot_patch.rs`](../userspace/ksud/src/boot_patch.rs) | [`lkm_image.rs`](../userspace/ksud/src/lkm_image.rs) |

## Where the code lives

Image *format* handling is out of tree. `ksud` depends on the `android-bootimg` crate, pinned
by commit in [`userspace/ksud/Cargo.toml`](../userspace/ksud/Cargo.toml):

```toml
android-bootimg = { git = "https://github.com/5ec1cff/android_bootimg",
                    rev = "150425b027c76ea104c82e408571651f2181b2c2" }
```

That crate owns header parsing ([`parser.rs`][bi-parser], [`layouts.rs`][bi-layouts]),
compression ([`compress.rs`][bi-compress]), the cpio archive ([`cpio.rs`][bi-cpio]) and
repacking ([`patcher.rs`][bi-patcher]). Policy -- which partition, which module, what to put in
the archive -- is `ksud`. The crate supplies verbs; `boot_patch.rs` decides what to say.

`magiskboot` is gone. KernelSU used to ship that binary and shell out to it; unpack and repack
are now in-process Rust. The only trace left in `ksud` is a comment about preserving its
vendor-ramdisk lookup order. The installation guides under
[`website/docs/`](../website/docs/guide/installation.md) are a separate matter: they still
document a `--magiskboot` flag that `BootPatchArgs` does not have, which
[`website/README.md`](../website/README.md) records as a known-stale block.

## The format library

`android-bootimg` is small -- about 2,400 lines across six modules -- and this section covers
all of it, so that reading `ksud` does not require reading the crate as well.

```
parser.rs     BootImage, BootHeader, block extraction, AVB detection
layouts.rs    field offsets for every header version, computed at compile time
compress.rs   format sniffing and codecs
cpio.rs       the ramdisk archive
patcher.rs    rebuilding an image
utils.rs      byte-slice accessors, alignment, a chunker
```

### The data model

Parsing is zero-copy. `BootImage` borrows the mapped file and adds a parsed header, the
located blocks, and any AVB structures it found:

```rust
pub struct BootImage<'a> {
    data: &'a [u8],
    header: BootHeader<'a>,          // a slice plus a layout descriptor
    blocks: BootImageBlocks<'a>,     // kernel, ramdisk, vendor fragments
    avb_info: Option<BootImageAVBInfo<'a>>,
}
```

`BootHeader` does not deserialise into a struct with named fields. It keeps the raw slice and a
`&'static BootHeaderLayout` describing where each field sits, and every accessor is a read at
`layout.offset_*`. That is what lets one type serve seven different header versions without a
variant per version.

### Header layouts

`layouts.rs` is a macro DSL. Each version declares its field sequence, and the macro accumulates
offsets at compile time -- `offset_of(field n+1) = offset_of(field n) + sizeof(field n)` --
emitting a `BootHeaderLayout` const with an `offset_*` (and for byte arrays, `size_*`) member per
field. A later version can inherit an earlier one and append.

Reading the declarations in order tells the GKI story:

| Layout | Fields it declares |
| --- | --- |
| `BOOT_HEADER_V0` | `kernel_size/addr`, `ramdisk_size/addr`, `second_size/addr`, `tags_addr`, `page_size`, `header_version`, `os_version`, `name[16]`, `cmdline[512]`, `id[32]`, `extra_cmdline[1024]` |
| `BOOT_HEADER_V1` | inherits v0, adds `recovery_dtbo_size`, `recovery_dtbo_offset`, `header_size` |
| `BOOT_HEADER_V2` | inherits v1, adds `dtb_size`, `dtb_addr` |
| `BOOT_HEADER_V3` | **a fresh layout**: `kernel_size`, `ramdisk_size`, `os_version`, `header_size`, `reserved[16]`, `header_version`, `cmdline[1536]` |
| `BOOT_HEADER_V4` | inherits v3, adds `signature_size` |
| `VENDOR_BOOT_HEADER_V3` | `header_version`, `page_size`, `kernel_addr`, `ramdisk_addr`, `ramdisk_size`, `cmdline[2048]`, `tags_addr`, `name[16]`, `header_size`, `dtb_size`, `dtb_addr` |
| `VENDOR_BOOT_HEADER_V4` | inherits vendor v3, adds `vendor_ramdisk_table_size`, `_entry_num`, `_entry_size`, `bootconfig_size` |

v3 is not an extension of v2, it is a replacement, and what it *drops* is the point. Every load
address is gone -- `kernel_addr`, `ramdisk_addr`, `second_addr`, `tags_addr` -- along with
`second_size`, the board `name` and the `id` hash. GKI fixed those values, so the header no
longer has to carry them, and the page size became a constant 4096 rather than a field. The two
command line fields merged into one 1536-byte field. Everything device-specific moved to
`vendor_boot`.

`page_size()` reflects that: it reads the field on v0-v2 and vendor headers, returns 4096 for
v3 and v4, and returns 1 for a raw ramdisk, where there are no pages to align to.

Only four of the seven layouts are reachable through `ksud`, which is stricter than the crate:
`enforce_bootimage_version()` in [`boot_patch.rs`](../userspace/ksud/src/boot_patch.rs) bails
with `bootimage version {ver} is not supported!` for any `ANDROID!` header below v3. Boot v3
and v4 and vendor v3 and v4 pass; v0-v2 parse and are then refused.

### Parsing

`BootHeader::parse` dispatches on the leading magic -- `ANDROID!` selects the boot layouts,
`VNDRBOOT` the vendor ones -- then reads `header_version` and picks the layout, rejecting a
version it does not know. Anything else is `invalid boot image`.

`BootImageBlocks::parse` then walks the blocks. Each is at a page-aligned offset, and its size
comes from the header, so the walk is arithmetic rather than search. For a vendor v4 image it
additionally parses the ramdisk table: `vendor_ramdisk_table_entry_num` entries of
`vendor_ramdisk_table_entry_size` bytes, each naming a fragment (`VENDOR_RAMDISK_NAME_SIZE` is
32) with its offset, size and type. Those become `VendorRamdiskEntry` values, which is what lets
`ksud` ask for the fragment called `init_boot`.

`BootImage::parse` finishes by looking for AVB. If the last `AvbFooter::SIZE` bytes start with
`AVBf` it reads the vbmeta offset and size from the footer, checks that the structure there
starts with `AVB0`, and records both plus any tail bytes between the payload and the vbmeta
block. A footer whose vbmeta magic is wrong is an error rather than a shrug.

`parse_raw_ramdisk` is the escape hatch, added by the pinned revision. It fabricates a header
with the all-zero `DEFAULT_LAYOUT` and version `RawRamdisk`, so a bare cpio file with no boot
header at all flows through the same code. That is what `ksud --ramdisk` uses.

### Compression

There is no field recording which codec a block used, so the format is sniffed from magic bytes:

| Format | Magic |
| --- | --- |
| gzip | `1f 8b` or `1f 9e` |
| lzop | `89 4c 5a 4f` (`\x89LZO`) |
| xz | `fd 37 7a 58 5a` |
| bzip2 | `BZh` |
| lz4 frame | `03 21 4c 18` or `04 22 4d 18` |
| lz4 legacy | `02 21 4c 18` |

LZMA has no magic, so it is *guessed*: first byte `0x5d`, a dictionary size that is a non-zero
power of two, and an uncompressed-size field of eight `0xff` bytes meaning unknown. Three
independent conditions, which is enough to make a false positive unlikely.

Decoding covers gzip (also used for zopfli, since zopfli emits a gzip stream), xz, lzma, bzip2
and both lz4 flavours. Encoding covers the same set, plus zopfli proper. **Lzop is detected but
has no codec** -- it falls into an `unreachable!()` arm -- so an lzop-compressed block would
panic rather than fail cleanly. Android ramdisks in practice are gzip or lz4.

Re-encoding uses the format the original block used, at maximum effort: xz and lzma at preset 9,
bzip2 and gzip at best compression, lz4 frame at level 9 with 4 MB independent blocks and
checksums on. Zopfli is configured with a single iteration and one block split, which the source
notes is already better than `gzip -9`.

### The cpio archive

Ramdisks are cpio in **newc** format. Each record is a 110-byte header -- the six-byte magic
`070701` followed by thirteen 8-digit ASCII hexadecimal fields -- then the NUL-terminated path,
then the file data, with the name and the data each padded to a 4-byte boundary:

| Field | Meaning |
| --- | --- |
| `c_magic` | always `070701` |
| `c_ino`, `c_nlink`, `c_mtime`, `c_dev*`, `c_check` | parsed and discarded |
| `c_mode` | type and permission bits |
| `c_uid`, `c_gid` | ownership |
| `c_filesize` | data length |
| `c_rdev*` | device numbers, kept for character nodes |
| `c_namesize` | path length including the NUL |

Everything being ASCII hexadecimal is why a ramdisk is greppable without tooling.

Two details of the implementation shape everything downstream. The first is that a
`TRAILER!!!` record ends an archive, but the parser then scans forward for the next `070701`
and continues if it finds one, because **Android concatenates archives** -- that is how a
`vendor_boot` ramdisk fragment and the generic ramdisk reach the kernel as one stream.

The second is that entries live in a `BTreeMap<String, Box<CpioEntry>>`, so a repacked archive
is sorted by path and the original ordering is lost. Nothing in the format depends on order,
but a byte-for-byte diff against the input will not be empty.

`Cpio` exposes what a patcher needs and nothing more: `exists`, `add`, `mv`, `rm`, `ls`,
`entry_by_name`, `entries`. `CpioEntry` has constructors for the types that occur in a ramdisk --
`regular`, `dir`, `symlink`, `char` -- with `uid`/`gid` builders. Writing back out renumbers
inodes from 300000 and re-emits a trailer.

`is_magisk_patched()` lives here too, and is simply a check for `.backup/.magisk`,
`init.magisk.rc` or `overlay/init.magisk.rc`.

### Repacking

`BootImagePatchOption` is a builder over a parsed source image:

```rust
let mut patcher = BootImagePatchOption::new(&boot_image);
patcher.replace_ramdisk(Box::new(cursor), false);   // false = not already compressed
patcher.patch(&mut output_file)?;
```

`replace_kernel` and `replace_ramdisk` take a reader plus a flag saying whether the bytes are
already compressed; when they are not, the block is re-encoded in the source block's format.
Where the source image had no such block to copy a format from, the two disagree: a new
ramdisk is encoded lz4 legacy, following Magisk, while a new kernel is
`Could not determine compression format of kernel`. The first of those is the path
[strategy 1](#strategy-1-become-init) takes for an image with no ramdisk at all.

`replace_vendor_ramdisk` replaces one indexed fragment, and calling `replace_ramdisk` on a
vendor v4 image is an error, because there is no single ramdisk to replace.
`override_cmdline` writes into the header's command line field and fails if the string exceeds
it.

`patch()` streams a new image out rather than editing in place:

1. Copy the source header verbatim, then apply any cmdline override into it.
2. Write the kernel block, page-aligned, re-encoding if required, and record its size.
3. Write the ramdisk. For a vendor v4 image this loops over the fragment table, writing each
   fragment and recording its new offset and size.
4. Write `second`, `recovery_dtbo`, `dtb` and `signature`, then the rewritten fragment table,
   then `bootconfig`. On a real `vendor_boot` v4 image the `dtb` block therefore lands between
   the fragments and the table that describes them.
5. If the source had AVB structures, append the tail and the vbmeta block unchanged -- after
   checking there is room, and failing with the measured sizes if not -- then zero-fill from
   the end of the vbmeta block up to the footer, and rewrite the footer with the new payload
   size and vbmeta offset.
6. Seek back and patch every size field in the header now that they are known.

The vbmeta block is copied, never recomputed; the footer is rewritten only so that
`original_image_size` and `vbmeta_offset` still point where they should. The image therefore
stays structurally valid while its hashes no longer describe its contents, which is the honest
outcome: the crate does not hold signing keys and does not pretend to. The consequence for the
device is that a bootloader still enforcing verified boot on that partition will reject the
result, so patching presupposes an unlocked bootloader or verification disabled for it.
Preserving the footer is about keeping the image well-formed, not about passing a check.

### The CLI

The workspace also builds [`android-bootimg-cli`][bi-cli], a standalone unpack and repack tool
over the same library. It is not used by `ksud`, but it is the quickest way to look inside an
image by hand when a patch has gone wrong.

## Strategy 1: become `/init`

`ksud boot-patch`, in [`boot_patch.rs`](../userspace/ksud/src/boot_patch.rs).

There is nothing special about `init` in a cpio archive. It is a regular file at the root,
mode `0755`, and the kernel execs whatever it finds there. That is the entire reason this
strategy works, and why the substitution itself is three archive operations: rename `init` out
of the way, add a new `init`, add the module. The config and backup entries below are
everything else.

### Choosing the partition

With no explicit `--boot`, `choose_boot_partition()` decides. An explicit
`--partition boot | init_boot | vendor_boot` wins; any other value is not an error but
silently becomes `boot`. Otherwise, if an `init_boot` partition exists and the kernel is not
being replaced, that is the target -- **except** when the KMI begins `android12-`, because the
`init_boot` split arrived after Android 12 and on those devices the generic ramdisk is still
inside `boot`. Failing both, `boot`.

The slot suffix comes from `ro.boot.slot_suffix` on A/B devices. `--ota` deliberately picks
the *other* slot, so an image can be staged for the half about to become active.

### Choosing the module

A GKI kernel exports a frozen ABI called the **KMI** (Kernel Module Interface). A module built
against one KMI carries a vermagic string and symbol CRCs from that build; load it on a kernel
with a different KMI and `init_module` refuses it, which is the good outcome. The bad outcome
would be a module that loads while disagreeing about a struct layout.

The KMI is written `android14-6.1`, and it selects an embedded asset named
`{kmi}_kernelsu.ko` -- which is why `userspace/ksud/bin/aarch64/` ships
`android14-6.1_kernelsu.ko`. `--module` and `--kmi` override the two halves of that.

Where the string comes from is decided in order, because each source is authoritative about a
different kernel:

| | Source of the KMI |
| --- | --- |
| 1 | `--kmi`, taken verbatim. |
| 2 | `--module`: the KMI is left empty, since no embedded asset has to be named. |
| 3 | `--ota`: `parse_kmi_from_boot()` on the *other* slot's `boot<suffix>` block device. |
| 4 | On a device, `get_current_kmi()`: the `uname` release, else a `modinfo` `vermagic` line from the first `.ko` under `/vendor/lib/modules`. |
| 5 | The free function `parse_kmi()`, over the decompressed kernel of the `--boot` image or of a `--kernel`. |

Row 4 is the normal path on a device and row 5 only its fallback; row 5 is also the whole of
it on a host, where row 4 is not compiled in. Two sources rather than one because many OEM
kernels drop the `androidNN` token from `uname` while their vendor modules still carry it in
`vermagic`. `parse_kmi()` scans for the version banner with the regex
`(\d+\.\d+)(?:\S+)?(android\d+)` and swaps the captures; it scans because there is no field to
read -- the banner is simply a string in the kernel's rodata. `--ramdisk` reaches neither
row 5 nor an asset: with no kernel anywhere in the input it fails with
`please specify kmi manually`.

### Rewriting the archive

The ramdisk is decompressed into a `Cpio`. For a v4 `vendor_boot` with a fragment table,
`extract_ramdisk()` prefers the fragment named `init_boot` and falls back to the unnamed one,
matching magiskboot's order so images patched by either tool agree. An image with no ramdisk
block at all is not an error here: ksud prints `- No ramdisk, create by default` and starts
from an empty `Cpio`, which the crate then encodes lz4 legacy for want of a source format
(see [Repacking](#repacking)).

Then:

```rust
ensure!(!cpio.is_magisk_patched(), "Cannot work with Magisk patched image");

let is_kernelsu_patched = cpio.exists("kernelsu.ko");
if !is_kernelsu_patched && cpio.exists("init") {
    cpio.mv("init", "init.real")?;                                  // keep the stock init
}
cpio.add("init",        CpioEntry::regular(0o755, ksu_init))?;      // ksuinit becomes PID 1
cpio.add("kernelsu.ko", CpioEntry::regular(0o755, kernelsu_ko))?;   // the module rides along
```

The rename is guarded on `is_kernelsu_patched` -- the archive already carrying a
`kernelsu.ko` -- so the operation is idempotent: re-patching an already-patched image must not
move *ksuinit* to `init.real` and lose the real init.

`is_magisk_patched()` looks for `.backup/.magisk`, `init.magisk.rc` and
`overlay/init.magisk.rc`. Stacking on Magisk is refused rather than attempted, because both
projects claim `/init` and whichever ran second would orphan the other's saved copy. The check
sits inside the install branch, so `--no-install` -- which only rewrites the config entries --
skips it along with the rename and the two adds.

At boot, [`ksuinit`](../userspace/ksuinit/README.md) runs as PID 1 and loads the module with
[`init_module`][init-module-2], reading its parameters from `/ksu_config`. It then unlinks
`/init` and recreates it as a symlink to `init.real` -- or to `/system/bin/init` when the
archive had no `init.real` -- before exec'ing `/init`. Android never observes the difference.

### Getting parameters to the module

`insmod` takes parameters on a command line; here there is no command line. The patcher writes
a `ksu_config` entry into the same archive instead, a space-separated list that ksuinit reads
and passes as the module argument string. That is how `allow_shell`, `norc` and `bundled`
(see [`kernel/core/README.md`](../kernel/core/README.md)) reach a module nobody typed a
command for.

Only three of the flags become tokens in it, and the third is not a flag at all. The adb
ones write separate entries, and are easy to misread as more of the same:

| Flag | What it does to the archive |
| --- | --- |
| `--allow-shell` | adds the token `allow_shell=1` to `ksu_config` |
| `--no-custom-rc` | adds the token `norc=1` to `ksu_config` |
| either omitted on a re-patch | removes that token again; a `ksu_config` left empty is deleted outright |
| (no flag) `--kmod` absent | adds `bundled=1`, recording that the LKM being installed is the one ksud carries; supplying `--kmod` omits the token, and `--no-install` leaves whatever the image already had |
| `--enable-adbd` | adds an empty `force_debuggable` marker plus an `adb_debug.prop` entry holding `ro.debuggable=1`, `ro.force.debuggable=1` and `ro.adb.secure=0` |
| `--adb-debug-prop <text>` | adds the same marker, and appends the text to `adb_debug.prop` |
| both adb flags omitted | removes `force_debuggable` and `adb_debug.prop` |

Every patch also deletes a legacy `allow_shell` entry -- the bare file that predates
`ksu_config` -- whether or not the flag was given.

### Backing up the stock image

The untouched image is preserved so that [restoring](#restoring) does not need the factory
image. It is kept as a file on the data partition rather than inside the archive, and the
names come from [`defs.rs`](../userspace/ksud/src/defs.rs):

1. `do_backup()` computes the SHA-1 of the source image.
2. It copies the raw bytes to `/data/adb/ksu/ksu_backup_<sha1>`, with `std::io::copy` rather
   than `fs::copy` so the source may be a block device. If `/data/adb` is not writable yet the
   copy goes to `/data/user_de/<user>/<package>/boot_backup/` instead, and if neither is
   accessible the backup fails.
3. The archive receives only the hash, as a `stock_image.sha1` entry naming that copy.

The backup runs when `--backup` is given, and on the first `--flash` of an image that is not
already patched -- `is_kernelsu_patched` again. It sits in the install branch, so
`--no-install` skips it, and it is best-effort: a failure prints
`- Backup stock image failed` and the patch continues.

## Strategy 2: inject into the kernel Image

`ksud boot-patch-v2`, in [`lkm_image.rs`](../userspace/ksud/src/lkm_image.rs). This exists for
images with no ramdisk `/init` to take over. It never touches the ramdisk; it edits the arm64
`Image` and appends to it.

### What the image has to be

Five conditions decide whether an image is a candidate at all. Each of them refuses the patch
outright rather than degrading, and each error names what was looked for:

| Requirement | Enforced by | Refusal |
| --- | --- | --- |
| an uncompressed arm64 `Image`: the magic `ARM\x64` at 0x38, a non-zero `image_size` at 0x10 | `parse_arm64_image_size()` | `kernel input is not an uncompressed ARM64 Image` |
| a 5.10, 5.15, 6.1, 6.6 or 6.12 kernel, read out of `linux_banner` | `recover_gki_abi()` | `unsupported GKI kernel series {major}.{minor}` |
| one unambiguous kallsyms table, which in practice means `CONFIG_KALLSYMS_ALL` | `recover_arm64_kernel_metadata()` | `cannot uniquely recover GKI kallsyms from ARM64 Image` |
| sixteen symbols that resolve to exactly one address each | `RequiredSymbols::resolve()` | `symbol "..." was not found`, or `is not unique` |
| enough proven zero padding before `_etext`, within `BL` range of every patch site | `find_text_tail_cave()`, `encode_bl()` | `cannot find 0x... bytes of proven zero padding before _etext` |

The rest of this section is what happens once all five hold.

### Recovering a symbol table

A raw `Image` has no ELF symbol table. The injector recovers one from **kallsyms**, the
kernel's own compressed symbol table, compiled in so that oops traces can print names. Its
encoding is a token table -- a fixed set of short byte strings -- plus symbol names stored as
sequences of token indices, which is what makes it compact.

`find_kallsyms_token_tables()` scans for something with that shape, and there can be more than
one plausible candidate. The tie is broken with the kernel's embedded [**BTF**][btf]
([`lkm_image_btf.rs`](../userspace/ksud/src/lkm_image_btf.rs)), the type information the kernel
carries for BPF, whose layout pins which candidate is real. If that still does not yield a
unique answer the patch is **refused**, with an error naming how many candidates were found.

Sixteen symbols must resolve, among them `_text`, `_stext`, `_etext`, `_end`, `linux_banner`,
`arm64_memblock_init`, `memblock_reserve`, `memstart_addr`, `kimage_voffset`, `kernel_init`,
`async_synchronize_full`, `load_module`, `strndup_user`, `vmalloc` (or `vmalloc_noprof` on
newer kernels), `memcpy` and `kstrdup`.

### Finding somewhere to put code

New code goes into a **code cave**: a run of unused bytes already inside the image.
`find_text_tail_cave()` walks backwards from `_etext` over trailing zero padding, then requires
that the candidate range is entirely zero and holds no symbol except the section-boundary
markers (`_etext`, `__stop_*`, `*_text_end`). It tries a 4096-aligned start first and falls
back to 16-aligned.

The constraint deciding *where* is `free_initmem()`. Look again at `kernel_init()`: it frees
`.init.text` immediately after the hook site. The `strndup_user` adapter below is entered from
`load_module()` on every later `insmod`, so it has to keep working for the life of the system;
the cave therefore has to be in **permanent** text -- between `_stext` and `_etext` -- not in
the much larger init sections about to be returned to the allocator. The other two patches
both run before `free_initmem()` and would survive anywhere.

Calls into the cave are ordinary `BL` instructions rewritten in place. `encode_bl()` enforces
the architectural limit: a 26-bit signed immediate scaled by 4, so a `BL` reaches +-128 MiB.
The cave must be within that of every site being patched.

### Three patches, not one

All three live in [`lkm_image_bootstrap.S`](../userspace/ksud/src/lkm_image_bootstrap.S). Each
replaces exactly one `BL`, and each solves a different problem.

**1. The bootstrap, at the `async_synchronize_full()` call in `kernel_init()`.** The moment
identified earlier: rootfs unpacked, init not yet exec'd. The replacement calls the original
function first -- the kernel still needs that synchronisation -- then does its own work.
Patching a *call site* rather than the function means only this caller is affected;
`async_synchronize_full` stays itself for everyone else.

**2. The memblock wrapper, at a `memblock_reserve()` call inside `arm64_memblock_init()`.**
The capsule is appended *after* `_end`, and the kernel reserves only `[kernel_start, _end)` as
in-use. Everything past `_end` is free as far as the page allocator is concerned, so between
early boot and `kernel_init()` the capsule could be handed out and overwritten. The wrapper
widens the reservation:

```asm
ksu_memblock_reserve_wrapper:
    ldr x8, =ksu_reserve_extension
    add x1, x1, x8                  /* extend the size argument */
    b   ksu_ext_memblock_reserve    /* tail-call the real thing */
```

Locating that call is harder than the others, because `arm64_memblock_init()` calls
`memblock_reserve()` several times and the right one cannot be identified by target alone. The
injector matches it **semantically**: it decodes the two instructions preceding each candidate
and looks for the `sub` shifted-register pair computing `_end - kernel_start`, the signature of
the reservation covering the kernel image. Pattern-matching on decoded instructions rather than
on names is what makes that survive recompilation.

**3. The `strndup_user()` adapter, inside `load_module()`.** `load_module()` copies the module
argument string from *user* memory. PID 1 here has not exec'd anything and has no usable
userspace mapping, so that copy would fail. The adapter compares the incoming pointer against
the injector's own empty-string symbol; on a match it duplicates from kernel memory with
`kstrdup`, and otherwise **tail-calls the real `strndup_user`**. That conditional is what keeps
every later `insmod` from userspace working normally.

### The capsule

The module and its relocation fixups are appended after `_end` as a capsule: it starts at the
16-byte-aligned end of the Image and is padded so that the new `image_size` lands on a 4096
boundary, behind a 96-byte header beginning `KSULKM1\0`.

The header carries ten fields, of which the bootstrap re-checks eight -- magic, version,
header size, capsule size, module offset and size, fixup offset and count -- against constants
baked in at injection time, returning quietly if any disagree. A truncated or mismatched
capsule does nothing rather than loading garbage. The remaining two, a flags word set when the
fixup table is non-empty and the module's SHA-256, are written at injection time and never
read back at boot.

Reaching it takes arithmetic, because the capsule lies beyond the link-time end of the image
and is not covered by the kernel image mapping. The bootstrap goes through the
[**linear map**][arm64-memory]:

```
capsule_phys = __pa_symbol(_text) + capsule_image_offset
capsule_virt = (capsule_phys - memstart_addr) | PAGE_OFFSET
```

which is why `memstart_addr` and `kimage_voffset` are in the required-symbol list. Every branch
inside the bootstrap is PC-relative, so the whole thing survives [KASLR][kaslr].

Then the fixups. A `.ko` refers to kernel symbols it does not define, and normally the module
loader resolves them. Here the injector resolved them offline, so each fixup record rewrites
one `Elf64_Sym` in the copied module: `st_shndx` set to `SHN_ABS` (`0xfff1`) and `st_value` to
the absolute address, which makes the loader treat the symbol as already resolved. This is the
same manoeuvre [`ksuinit`](../userspace/ksuinit/README.md) performs from userspace, done here
in a dozen instructions. A `struct load_info` is then built on the stack -- oversized and
zeroed, with only `hdr` and `len` seeded -- and `load_module()` is called.

## Restoring

`ksud boot-restore` reverses strategy 1 by one of two paths. It refuses an image with no
`kernelsu.ko` entry -- `boot image is not patched by KernelSU` -- and, unlike `boot-patch`, it
refuses an image with no ramdisk instead of creating one.

**Write the saved image back, if it is still there.** The `stock_image.sha1` entry left by
[the backup](#backing-up-the-stock-image) names a copy; when
`/data/adb/ksu/ksu_backup_<sha1>` is a file, restore reads it whole and flashes or writes
those bytes back unchanged. Only that one directory is consulted, so a backup that landed in
the `/data/user_de` fallback is not found again and the rebuild takes over. Either way,
`clean_backup()` then deletes every other `ksu_backup_*` file in the directory.

**Rebuild the image, otherwise.** `rebuild_without_ksu()` removes `kernelsu.ko` from the
archive and, when `init.real` exists, renames it back over the `init` that ksuinit installed --
one rename, not a delete followed by a move. An archive with no `init.real` keeps the ksuinit
`init`, which is the correct outcome for an image that never had one. The result is repacked
through the same `BootImagePatchOption` path as a patch.

The rebuild is also the only path on a host, where the backup lookup is not compiled in.

## See also

- [`userspace/ksud/README.md`](../userspace/ksud/README.md) - the daemon's other jobs
- [`userspace/ksuinit/README.md`](../userspace/ksuinit/README.md) - what runs as PID 1 afterwards
- [`kernel/runtime/README.md`](../kernel/runtime/README.md) - the boot pipeline the module joins
- [`kernel/core/README.md`](../kernel/core/README.md) - the two bring-up paths
  `ksu_late_loaded` selects
- [`docs/architecture.md`](architecture.md) - the repository-wide hub
- [`android-bootimg`][bi-repo] at the pinned revision - the format library

<!-- Links below point at the out-of-tree android-bootimg crate, pinned to the exact revision
     userspace/ksud/Cargo.toml depends on, so they never drift from the compiled source.
     Maintained by hand: update them together with the `rev =` in that manifest. -->
[bi-repo]: https://github.com/5ec1cff/android_bootimg/tree/150425b027c76ea104c82e408571651f2181b2c2
[bi-parser]: https://github.com/5ec1cff/android_bootimg/blob/150425b027c76ea104c82e408571651f2181b2c2/android-bootimg/src/parser.rs
[bi-layouts]: https://github.com/5ec1cff/android_bootimg/blob/150425b027c76ea104c82e408571651f2181b2c2/android-bootimg/src/layouts.rs
[bi-cpio]: https://github.com/5ec1cff/android_bootimg/blob/150425b027c76ea104c82e408571651f2181b2c2/android-bootimg/src/cpio.rs
[bi-compress]: https://github.com/5ec1cff/android_bootimg/blob/150425b027c76ea104c82e408571651f2181b2c2/android-bootimg/src/compress.rs
[bi-patcher]: https://github.com/5ec1cff/android_bootimg/blob/150425b027c76ea104c82e408571651f2181b2c2/android-bootimg/src/patcher.rs
[bi-cli]: https://github.com/5ec1cff/android_bootimg/blob/150425b027c76ea104c82e408571651f2181b2c2/android-bootimg-cli/src/main.rs
<!-- reference links: kernel documentation and man pages -->
[arm64-memory]: https://docs.kernel.org/arch/arm64/memory.html
[btf]: https://docs.kernel.org/bpf/btf.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
[initramfs]: https://docs.kernel.org/filesystems/ramfs-rootfs-initramfs.html
[kaslr]: https://docs.kernel.org/security/self-protection.html
[read-2]: https://man7.org/linux/man-pages/man2/read.2.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
