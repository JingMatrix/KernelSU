# Patching the boot image

Installing KernelSU on a GKI device means arranging for `kernelsu.ko` to be loaded by PID 1,
before Android's `init` runs. This document is about how that is arranged, and why each step
is the way it is.

## Why it has to happen that early

The module could be loaded later -- `ksud insmod` does exactly that -- but several things it
does are only possible before userspace starts.

The clearest is `init.rc` injection. The module appends its own content by proxying the
`read()` that init issues on that file, and it recognises the right file by watching init's
very first read of it. Arm that hook after init has already parsed its configuration and there
is nothing left to intercept.

The boot milestones are the same shape. `post-fs-data`, module mounting, the first
`app_process` -- the module's state machine is driven by events that all occur in the first
seconds, and a module that arrives afterwards has to assume they happened.

SELinux is subtler, and the late-load branch of `kernelsu_init()` shows exactly what it costs.
Loaded early, the module installs its own domain into the live policy and everything that
later needs it simply finds it there. Loaded late, onto a system already enforcing, it has to
`apply_kernelsu_rules()`, `cache_sid()`, then grant *itself* the new domain with
`escape_to_root_for_init()` before it can keep touching `/data/app` -- and if it found SELinux
permissive it re-enables enforcement by hand afterwards. All of that is reconstruction work
that the early path never has to do.

A module loaded after boot has missed all of that. The kernel side reflects this directly:
`ksu_late_loaded = (current->pid != 1)` in [`kernel/core/init.c`](../kernel/core/init.c)
splits `kernelsu_init()` into two quite different bring-up paths, and the late one has to
reconstruct by hand what the early one gets for free. See
[`kernel/runtime/README.md`](../kernel/runtime/README.md).

So the question is: how do you get a kernel module loaded by PID 1 on a device whose boot
image you can rewrite but whose kernel you cannot rebuild?

## How Linux actually starts userspace

The **initramfs** is a cpio archive carried in the boot image. Early in boot the kernel
decompresses it and unpacks it into rootfs -- `unpack_to_rootfs()` in `init/initramfs.c` --
creating a real, writable filesystem in memory. That work is scheduled asynchronously, as
`do_populate_rootfs()`.

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
are now in-process Rust. The only trace left is a comment about preserving its vendor-ramdisk
lookup order.

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

`page_size()` reflects that: it reads the field on v0-v2 and vendor headers, and returns 4096
for v3 and v4.

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

Two details of the implementation shape everything downstream. A `TRAILER!!!` record ends an
archive, but the parser then scans forward for the next `070701` and continues if it finds one,
because **Android concatenates archives** -- that is how a v4 fragment table and a generic
ramdisk arrive as one stream. And entries live in a `BTreeMap<String, Box<CpioEntry>>`, so a
repacked archive is sorted by path and the original ordering is lost. Nothing in the format
depends on order, but a byte-for-byte diff against the input will not be empty.

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
`replace_vendor_ramdisk` replaces one indexed fragment, and calling `replace_ramdisk` on a
vendor v4 image is an error, because there is no single ramdisk to replace.
`override_cmdline` writes into the header's command line field and fails if the string exceeds
it.

`patch()` streams a new image out rather than editing in place:

1. Copy the source header verbatim, then apply any cmdline override into it.
2. Write the kernel block, page-aligned, re-encoding if required, and record its size.
3. Write the ramdisk. For a vendor v4 image this loops over the fragment table, writing each
   fragment and updating that entry's offset and size, then emits the rewritten table.
4. Write the remaining blocks.
5. Seek back and patch the size fields in the header now that they are known.
6. If the source had AVB structures, append the tail and the vbmeta block unchanged -- after
   checking there is room, and failing with the measured sizes if not.

The AVB block is copied, never recomputed. The image stays structurally valid and its hashes no
longer describe its contents, which is the honest outcome: the crate does not hold signing keys
and does not pretend to. The consequence for the device is that a bootloader still enforcing
verified boot on that partition will reject the result, so patching presupposes an unlocked
bootloader or verification disabled for it. Preserving the footer is about keeping the image
well-formed, not about passing a check.

### The CLI

The workspace also builds [`android-bootimg-cli`][bi-cli], a standalone unpack and repack
tool over the
same library. It is not used by `ksud`, but it is the quickest way to look inside an image by
hand when a patch has gone wrong.

## Strategy 1: become `/init`

`ksud boot-patch`, in [`boot_patch.rs`](../userspace/ksud/src/boot_patch.rs).

There is nothing special about `init` in a cpio archive. It is a regular file at the root,
mode `0755`, and the kernel execs whatever it finds there. That is the entire reason this
strategy works, and why it amounts to four archive operations.

### Choosing the partition

With no explicit `--boot`, `choose_boot_partition()` decides. An explicit
`--partition boot | init_boot | vendor_boot` wins. Otherwise, if an `init_boot` partition
exists and the kernel is not being replaced, that is the target -- **except** when the KMI
begins `android12-`, because the `init_boot` split arrived after Android 12 and on those
devices the generic ramdisk is still inside `boot`. Failing both, `boot`.

The slot suffix comes from `ro.boot.slot_suffix` on A/B devices. `--ota` deliberately picks
the *other* slot, so an image can be staged for the half about to become active.

### Choosing the module

A GKI kernel exports a frozen ABI called the **KMI** (Kernel Module Interface). A module built
against one KMI carries a vermagic string and symbol CRCs from that build; load it on a kernel
with a different KMI and `init_module` refuses it, which is the good outcome. The bad outcome
would be a module that loads while disagreeing about a struct layout.

The KMI is written `android14-6.1`. `parse_kmi()` finds it by scanning the *decompressed
kernel* for the version banner with the regex `(\d+\.\d+)(?:\S+)?(android\d+)` and swapping
the captures. It scans because there is no field to read -- the banner is simply a string in
the kernel's rodata. That string selects an embedded asset named `{kmi}_kernelsu.ko`, which is
why `userspace/ksud/bin/aarch64/` ships `android14-6.1_kernelsu.ko`. `--module` and `--kmi`
override the two halves of that.

### Rewriting the archive

The ramdisk is decompressed into a `Cpio`. For a v4 `vendor_boot` with a fragment table,
`extract_ramdisk()` prefers the fragment named `init_boot` and falls back to the unnamed one,
matching magiskboot's order so images patched by either tool agree.

Then:

```rust
ensure!(!cpio.is_magisk_patched(), "Cannot work with Magisk patched image");

if !is_kernelsu_patched && cpio.exists("init") {
    cpio.mv("init", "init.real")?;                                  // keep the stock init
}
cpio.add("init",        CpioEntry::regular(0o755, ksu_init))?;      // ksuinit becomes PID 1
cpio.add("kernelsu.ko", CpioEntry::regular(0o755, kernelsu_ko))?;   // the module rides along
```

The rename is guarded on `is_kernelsu_patched` so the operation is idempotent: re-patching an
already-patched image must not move *ksuinit* to `init.real` and lose the real init.

`is_magisk_patched()` looks for `.backup/.magisk`, `init.magisk.rc` and
`overlay/init.magisk.rc`. Stacking on Magisk is refused rather than attempted because both
projects claim `/init`, and whichever ran second would orphan the other's saved copy.

At boot, [`ksuinit`](../userspace/ksuinit/README.md) runs as PID 1, loads the module with
[`init_module`][init-module-2], and execs `init.real`. Android never observes the difference.

### Getting parameters to the module

`insmod` takes parameters on a command line; here there is no command line. The patcher writes
a `ksu_config` entry into the same archive instead, a space-separated list that ksuinit reads
and passes as the module argument string. `--allow-shell`, `--enable-adbd` and
`--no-custom-rc` add or remove entries. That is how `allow_shell` and `norc` (see
[`kernel/core/README.md`](../kernel/core/README.md)) reach a module nobody typed a command for.

Before flashing, the untouched image is stored inside the archive, so `ksud boot-restore` can
undo the patch without the factory image.

## Strategy 2: inject into the kernel Image

`ksud boot-patch-v2`, in [`lkm_image.rs`](../userspace/ksud/src/lkm_image.rs). This exists for
images with no ramdisk `/init` to take over. It never touches the ramdisk; it edits the arm64
`Image` and appends to it.

### Recovering a symbol table

A raw `Image` has no ELF symbol table. The injector recovers one from **kallsyms**, the
kernel's own compressed symbol table, compiled in so that oops traces can print names. Its
encoding is a token table -- a fixed set of short byte strings -- plus symbol names stored as
sequences of token indices, which is what makes it compact.

`find_kallsyms_token_tables()` scans for something with that shape, and there can be more than
one plausible candidate. The tie is broken with the kernel's embedded **BTF**
([`lkm_image_btf.rs`](../userspace/ksud/src/lkm_image_btf.rs)), the type information the kernel
carries for BPF, whose layout pins which candidate is real. If that still does not yield a
unique answer the patch is **refused**, with an error naming how many candidates were found.
`CONFIG_KALLSYMS_ALL` is required.

Sixteen symbols must resolve, among them `_text`, `_stext`, `_etext`, `_end`, `linux_banner`,
`arm64_memblock_init`, `memblock_reserve`, `memstart_addr`, `kimage_voffset`, `kernel_init`,
`async_synchronize_full`, `load_module`, `strndup_user`, `vmalloc` (or `vmalloc_noprof` on
newer kernels), `memcpy` and `kstrdup`.

### Finding somewhere to put code

New code goes into a **code cave**: a run of unused bytes already inside the image.
`find_text_tail_cave()` walks backwards from `_etext` over trailing zero padding, then requires
that the candidate range contains no symbol and is entirely zero.

The constraint deciding *where* is `free_initmem()`. Look again at `kernel_init()`: it frees
`.init.text` immediately after the hook site. Two of the three patches below must keep working
for the life of the system, so the cave has to be in **permanent** text -- between `_stext` and
`_etext` -- not in the much larger init sections about to be returned to the allocator.

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
The capsule is appended *after* `_end`, and the kernel
reserves only `[kernel_start, _end)` as in-use. Everything past `_end` is free as far as the
page allocator is concerned, so between early boot and `kernel_init()` the capsule could be
handed out and overwritten. The wrapper widens the reservation:

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

The module and its relocation fixups are appended after `_end` as a capsule, 4096-aligned,
behind a 96-byte header beginning `KSULKM1\0`. The bootstrap re-checks all eight header fields
-- magic, version, header size, capsule size, module offset and size, fixup offset and count --
against constants baked in at injection time, and returns quietly if any disagree. A truncated
or mismatched capsule does nothing rather than loading garbage.

Reaching it takes arithmetic, because the capsule lies beyond the link-time end of the image
and is not covered by the kernel image mapping. The bootstrap goes through the **linear map**:

```
capsule_phys = __pa_symbol(_text) + capsule_image_offset
capsule_virt = (capsule_phys - memstart_addr) | PAGE_OFFSET
```

which is why `memstart_addr` and `kimage_voffset` are in the required-symbol list. Every branch
inside the bootstrap is PC-relative, so the whole thing survives KASLR.

Then the fixups. A `.ko` refers to kernel symbols it does not define, and normally the module
loader resolves them. Here the injector resolved them offline, so each fixup record rewrites
one `Elf64_Sym` in the copied module: `st_shndx` set to `SHN_ABS` (`0xfff1`) and `st_value` to
the absolute address, which makes the loader treat the symbol as already resolved. This is the
same manoeuvre [`ksuinit`](../userspace/ksuinit/README.md) performs from userspace, done here
in a dozen instructions. A `struct load_info` is then built on the stack -- oversized and
zeroed, with only `hdr` and `len` seeded -- and `load_module()` is called.

## Restoring

`ksud boot-restore` reverses strategy 1: it recovers the backup stored in the archive, or
failing that removes `kernelsu.ko`, deletes the `init` that ksuinit installed, and moves
`init.real` back. It refuses to touch an image that was never patched.

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
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
