# Manager identification and tracking

Every privilege decision in this module eventually asks one question: is the caller the
KernelSU manager app? This directory is the only thing that answers it. The answer lives
in a single module-global `uid_t ksu_manager_appid`, defined once in
[`throne_tracker.c`](throne_tracker.c) and declared in
[`manager_identity.h`](manager_identity.h) under the comment `// DO NOT DIRECT USE`. Three
mechanisms feed it: [`apk_sign.c`](apk_sign.c), whose interface is
[`apk_sign.h`](apk_sign.h), decides whether an APK was signed with the key this kernel
trusts; [`throne_tracker.c`](throne_tracker.c) finds candidate APKs under `/data/app` and
resolves the winner to a uid; [`pkg_observer.c`](pkg_observer.c) re-runs the tracker
whenever Android rewrites its package database.

Everything else reaches the answer through six inline accessors: `is_manager()` and
`is_uid_manager()` for the two comparisons, `ksu_get_manager_appid()` and
`ksu_set_manager_appid()` for access, `ksu_invalidate_manager_uid()` to reset the global to
`KSU_INVALID_APPID` (-1), and `ksu_is_manager_appid_valid()` to test whether anyone has been
crowned. Both comparisons reduce modulo `KSU_PER_USER_RANGE` (100000), because Android
encodes a uid as `user_id * 100000 + appid`: a manager installed for secondary user 10 runs
as uid 1010234 and must still match the appid recorded at crowning time. Both are wrapped in
`unlikely()`, since they sit on the [`setresuid`][setresuid-2] path, in three of the five
supercall permission predicates and inside the allowlist lookup.

## Reading an APK from kernel context

`is_manager_apk()` has no PackageManager, no ZIP library and no way to ask userspace for a
parsed certificate. It works on the file directly, so the layout it walks is worth having
in front of you.

Every read the parser makes goes through `read_exact()`, which takes an `end` offset,
refuses any request that would run past it, and treats a short `kernel_read()` as failure.
No offset in the walk below is ever dereferenced without first naming the structure it has
to stay inside. That is the whole defence against an APK whose lengths are chosen by
someone else: a bad number fails a bound instead of pulling in a neighbouring structure or
walking off the end of the file.

A ZIP archive ends with an End Of Central Directory record: signature `0x06054b50`,
sixteen fixed bytes, a two-byte comment length, then the comment. The comment is
variable-length, so there is no fixed offset to seek to and `check_v2_signature()` finds
the record backwards. It takes the length once with `generic_file_llseek(fp, 0, SEEK_END)`
and computes every later offset arithmetically from it, reading the halfword at
`file_size - i - 2` for increasing `i`; when that equals `i` the candidate comment length
is self-consistent, so it rewinds 22 bytes and confirms the signature:

```c
        if (comment_size == i) {
            pos -= 22;
            if (!read_exact(fp, &magic, sizeof(magic), &pos, file_size))
                goto clean;
            if (magic == 0x06054b50) {
                eocd_offset = pos - sizeof(magic);
                break;
            }
        }
```

The loop gives up at `i == 0xffff`, the largest representable comment length, and
`eocd_offset` becomes the anchor for everything after it. Before trusting that record the
code rules out ZIP64. A ZIP64 archive places an end-of-central-directory locator,
signature `0x07064b50`, in the twenty bytes immediately preceding the EOCD, and the EOCD's
own 32-bit size and offset fields then carry `0xffffffff` placeholders while the real
values live in the ZIP64 record. Reading them anyway would mean following numbers that
describe nothing, so finding the locator abandons the file outright.

Twelve and sixteen bytes into the record sit the size and the offset of the central
directory, and the pair has to be consistent with the anchor: `cd_offset` no larger than
`eocd_offset`, and `cd_size` exactly `eocd_offset - cd_offset`, which holds only when
nothing is wedged between the central directory and the EOCD. A third bound demands
`cd_offset >= 0x20`: every offset the signing block needs is computed by stepping
backwards from that anchor, and a central directory beginning in the first 32 bytes of the
file has nothing behind it to step into. A v2-signed APK keeps its signing block
immediately before the central directory. The block carries its own size at both ends: a
`u64` size, the id-value pairs, the same `u64` size again, and the 16-byte magic
`APK Sig Block 42`. The code positions itself 24 bytes before the central directory --
that offset becomes `pairs_end`, the end of the pair region -- reads the trailing size and
the magic, bounds the size against `cd_offset` so that stepping back cannot land at a
negative offset, then seeks back by that size plus eight, reads the leading size, and
refuses to continue unless the two agree.

The body is a sequence of `[u64 pair-length]u32 id`. Three ids matter:
`0x7109871a` is v2, `0xf05368c0` is v3, `0x1b93ad61` is v3.1. The walk is a plain
`while (pos < pairs_end)`, and what terminates it is the data rather than a counter: a
pair must declare a length of at least four bytes -- enough for the id it is required to
contain -- and no more than the bytes still left before `pairs_end`, after which
`pos = pair_end` moves strictly forward. Neither a zero-length pair nor a length large
enough to wrap can make the loop repeat or step outside the block, so a truncated or
hostile signing block is rejected at the first pair that does not fit rather than stepping
forward until a `kernel_read()` comes up short past the end of the file, all of it inside
a `system_server` syscall. A pair that fails either bound branches to the `invalid` label,
which clears `v2_signing_valid` before the shared close path.

Reaching every pair is not an optimisation; it is the premise of the v3 rejection below.
The walk used to stop after ten pairs and to break early on a pair whose declared length
happened to equal the block size, so an attacker could keep one genuine v2 block, pad the
region with filler pairs and park the v3 id behind them: the kernel counted one v2 block,
never saw a v3 id, and crowned an APK that Android was installing under a different v3
signer. That is CAN-2026-2035133, closed by making the loop consume the region exactly,
with no iteration cap and no sentinel length, so an id can only be missed by lying outside
the block whose trailing and leading sizes already agreed.

`check_block()` extracts the certificate, entered just past the v2 id with `pair_end` as
its outer bound. The v2 block nests length-prefixed structures, and
`read_length_prefixed_end()` is the single primitive for descending them: it reads a `u32`
length, checks it against the end of the enclosing structure, and hands back the offset at
which the value ends. Four calls walk signers sequence, first signer, signed data, digests
sequence, each bounded by the one above it, so a length claiming more than its parent
holds is caught at the level that knows the parent's extent. Skipping the digests is then
the assignment `*pos = digests_end` rather than an addition that could overflow. A fifth
call reads the certificates-sequence length, and the certificate length is read inside
that. The comparison against the build-time constant happens *before* the certificate is
read:

```c
    if (certificate_size != expected_size)
        return false;

    if (certificate_size > CERT_MAX_LENGTH) {
        pr_info("cert length overlimit\n");
        return false;
    }
```

A wrong length is rejected without a 1 KiB read and without a SHA-256, which matters on a
device holding hundreds of APKs. On a match the DER bytes go through the synchronous shash
API -- `crypto_alloc_shash("sha256")` plus a `struct sdesc` sized as a `struct shash_desc`
followed by `crypto_shash_descsize(alg)` trailing bytes, the idiom for carrying
per-algorithm context inline -- and the digest is rendered with `bin2hex()`. Both the
computed and the expected hash go to `pr_info` before the `strcmp` decides anything, but
the length mismatch has already returned by then, so an APK signed with a key of a
different DER length never reaches that line. When a self-built manager is not recognised,
look in dmesg first for the tracker's `Found new base.apk at path: <path>, is_manager: 0`,
and only then for the `sha256:` pair; its absence means the certificate size, not the
hash, is what disagrees.

Where `check_block()` leaves `pos` is of no consequence to its caller. The pair walk sets
`pos = pair_end` on the way out, so a field added or removed inside the v2 descent cannot
desynchronise the outer loop; the only thing that ever advances it is the length the pair
declared for itself.

## Why one v2 block and nothing else

Passing the certificate check is not sufficient. `check_v2_signature()` also requires that
exactly one v2 block was seen and that neither the v3 nor the v3.1 id turned up anywhere
in the pair walk.

Android verifies an APK with the *highest* scheme present, preferring v3.1, then v3, then
v2, then v1, and the KernelSU release certificate is public -- it ships inside every
release APK. An attacker could therefore carry the genuine certificate in a v2 block while
Android installs and runs the app under a completely different v3 signer. Forcing v2 to be
the only signature makes the certificate this code hashed the same certificate Android
used.

Nothing looks for a v1 signature, and nothing needs to. v1 is the bottom of that
preference order, so a v2 block already takes it out of contention: the framework does not
fall back to the JAR manifest when a signing block it understands is present. A stray
`META-INF/MANIFEST.MF` therefore attests to nothing Android will act on. The scan that
used to reject one walked ZIP local file headers from offset zero through a packed
`zip_entry_header`, trusting each entry's own `compressed_size` to locate the next: a
second parser over the same attacker-supplied structure, for no decision it could change.
It is gone, and nothing in the tree replaces it.

The two rejections announce themselves differently. A v2 count other than one clears
`v2_signing_valid` and logs `Unexpected v2 signature count` only under `CONFIG_KSU_DEBUG`.
A v3 or v3.1 id logs `Unexpected v3 signature scheme found!` unconditionally, but the test
runs after the file is closed and is guarded by `v2_signing_valid`, so only an APK that
would otherwise have been crowned produces the line; the ordinary v3-signed apps that fill
`/data/app` pass through the sweep silently.

The constraint reaches into packaging. [`repack_apk.py`](../../repack_apk.py) re-signs the
manager after injecting `libksud.so`, and its `apksigner` invocation passes
`--v2-signing-enabled true` with v1, v3 and v4 all explicitly disabled. Any tool that
rewrites the APK must keep v2 on and v3 off, or the kernel silently stops recognising it.
The v1 switch is the one that has stopped mattering here; leaving it off still costs
nothing, since a JAR manifest only adds entries no verifier on the device will consult.

Reading the APK leaves no inotify trace. Right after `filp_open()`, `check_v2_signature()`
sets `fp->f_mode |= FMODE_NONOTIFY`; `kernel_read()` reaches `__kernel_read()`, which calls
`fsnotify_access(file)` on every successful read, and `fsnotify_file()` returns early for
that mode bit, so an app watching its own `base.apk` sees nothing. The directory walk is
not covered. `search_manager()` opens each directory with plain `O_RDONLY | O_NOFOLLOW` and
never sets that bit, and `iterate_dir()` calls `fsnotify_access()` on the directory file
unconditionally, so an app watching its own `/data/app` package directory does see an
`IN_ACCESS` on every sweep.

## Pinning the signature at build time

[`Kbuild`](../Kbuild) is the only place the trusted certificate is described.
`KSU_EXPECTED_SIZE` defaults to `0x0385` (901) and `KSU_EXPECTED_HASH` to
`b10b03393e1f7df49a6dcd97cdb6478fb19cb5efd2024841be1129fd807697ed`; both become
`-DEXPECTED_SIZE` and `-DEXPECTED_HASH` and fold into the code as constants, which is why
`check_v2_signature()` is `__always_inline`.

A second certificate slot exists. Define `KSU_EXPECTED_SIZE2` and `KSU_EXPECTED_HASH2` and
`is_manager_apk()` retries with the second pair:

```c
    if (check_v2_signature(path, EXPECTED_SIZE, EXPECTED_HASH)) {
        return true;
    }
#ifdef EXPECTED_SIZE2
    return check_v2_signature(path, EXPECTED_SIZE2, EXPECTED_HASH2);
```

Continuous integration is what the slot is for: the `generate-key` job in
[`build-manager.yml`](../../.github/workflows/build-manager.yml) mints an ephemeral
keystore and hands its DER certificate size and SHA-256 to
[`build-lkm.yml`](../../.github/workflows/build-lkm.yml), which fans them across the KMI
matrix into [`ddk-lkm.yml`](../../.github/workflows/ddk-lkm.yml), so a kernel built from a
pull request accepts the release manager and that PR's throwaway-signed one alike. Each
KMI is built twice inside that single job: once for aarch64 against the DDK image's own
kernel directory, then once for x86_64 against a second output tree prepared from the same
source tarball, and both `make` invocations take the pair as
`KSU_EXPECTED_SIZE2=`/`KSU_EXPECTED_HASH2=`. The guard on the pair is only half there:
Kbuild raises `$(error)` for a size given without a hash, but silently ignores a hash given
without a size. `ddk-lkm.yml` rejects both orderings, in a shell check written out once
before each of the two architecture builds, so the pinning rules for a PR kernel live in
three places that have to agree. Defining the slot also sets `KSU_GET_INFO_FLAG_PR_BUILD`
in `do_get_info()` and `do_get_info_legacy()` ([`dispatch.c`](../supercall/dispatch.c)),
which `is_pr_build()` in [`ksu.cc`](../../manager/app/src/main/cpp/ksu.cc) reads back.

`KSU_MANAGER_PACKAGE` narrows the check further. With it defined, `is_manager_apk()`
derives the package name from the APK path and demands exact equality before spending
anything on the signature:

```c
    if (strncmp(pkg, KSU_MANAGER_PACKAGE, sizeof(KSU_MANAGER_PACKAGE))) {
        return false;
    }
```

`sizeof()` of a string literal includes the terminating NUL and `pkg` is NUL-terminated,
so this is equality, not a prefix test. The macro is undefined by default, so a stock
build accepts any package name carrying the right certificate.

Renaming and self-signing the manager therefore takes three coordinated changes.
[`build.gradle.kts`](../../manager/app/build.gradle.kts) sets `applicationId` to
`org.matrix.su` (or `org.matrix.su.pr` for a PR build), overridable with
`-PKSU_PACKAGE_NAME`. The kernel must be built with `KSU_EXPECTED_SIZE=` and
`KSU_EXPECTED_HASH=` matching the new certificate; no workflow here overrides slot 1, so a
fork signing with its own key passes them on the `make` command line. And a pinned
`KSU_MANAGER_PACKAGE` must equal the new `applicationId` exactly -- pinning it to
`org.matrix.su` rejects the PR manager regardless of the second slot.

For development there is an escape hatch. Under `CONFIG_KSU_DEBUG` only,
[`apk_sign.c`](apk_sign.c) registers `ksu_debug_manager_appid` as a writable module
parameter whose setter calls `ksu_set_manager_appid()` directly, bypassing the signature
check; `ksud debug set-manager <pkg>` in [`debug.rs`](../../userspace/ksud/src/debug.rs)
writes `st_uid % 100000` of `/data/data/<pkg>` into it and force-stops the app, since the
driver fd is only planted at zygote specialization time. A release build never compiles the
parameter at all, and a release LKM build goes further: [`init.c`](../core/init.c) calls
`kobject_del(&THIS_MODULE->mkobj.kobj)`, unlinking the whole `/sys/module/kernelsu`
directory and every trace of the module underneath it. That missing directory is what
`ksud debug set-manager` reports as "CONFIG_KSU_DEBUG is not enabled".

`get_pkg_from_apk_path()` supplies the package name for both the pin and
`crown_manager()`. It scans backwards for the last two `/`, takes the first `-` after the
second-to-last, and copies what lies between, so
`/data/app/~~AbC==/org.matrix.su-XyZ==/base.apk` yields `org.matrix.su`, and so does the
older one-level layout. It rejects any path at or beyond `KSU_MAX_PACKAGE_NAME` (256, from
[`app_profile.h`](../../uapi/app_profile.h)) while the walker's buffers are 384 bytes, so a
`base.apk` between those two lengths is signature-checked and then fails to be crowned.

## Finding the manager: track_throne and the /data/app walk

`track_throne()` always begins by parsing `/data/system/packages.list`, the
space-separated file PackageManagerService maintains whose first two fields are the
package name and its uid. It is the only package-to-uid mapping the kernel can read
without an Android API. The parser reads one byte at a time looking for a newline, then
bulk-reads from the start of the line into a 256-byte buffer and `strsep`s off the first
two fields, so nothing buffers the whole file.

With that list in hand the function branches. If `prune_only` is set it skips discovery.
Otherwise it looks for an entry whose uid equals `ksu_get_manager_appid()`; if one exists
the throne is untouched. If none exists and the appid was valid, it logs "manager is
uninstalled, invalidate it!", calls `ksu_invalidate_manager_uid()` and goes straight to
pruning *without* rescanning -- the next `packages.list` write will find an invalid appid
and start the search then, which bounds the work done inside any one `system_server`
syscall. Only when the appid is already invalid does this call run
`search_manager("/data/app", 2, &uid_list)`.

`search_manager()` is a breadth-first walk driven by `iterate_dir()`, the in-kernel
readdir interface: embed a `struct dir_context` in a larger struct, point its `actor` at a
`filldir_t` callback, recover your own struct with `container_of()`. That callback
protocol changed in Linux 6.1 -- it returns `bool` now, `true` meaning keep going, where
it used to return `int` with 0 meaning keep going -- so the file defines
`FILLDIR_RETURN_TYPE`, `FILLDIR_ACTOR_CONTINUE` and `FILLDIR_ACTOR_STOP` against
`LINUX_VERSION_CODE`. The actor,
`my_actor()`, skips `.`, `..` and the installer's `vmdl*.tmp` staging directories, queues
any directory at a depth above zero for the next round, and treats a file named exactly
`base.apk` as a candidate at every level, which covers both the modern
`/data/app/~~<b64>/<pkg>-<b64>/base.apk` layout and the older one-level one.

A negative cache keeps the walk affordable. Each candidate path is reduced to
`full_name_hash(NULL, dirpath, strlen(dirpath))` and looked up in the module-global
`apk_path_hash_list`; a hit only marks the entry live, while a miss runs the signature
check and, on failure, appends a new hash. Entries not seen during a walk are freed
afterwards, so uninstalled apps drop out. Without the cache, every `packages.list` write
made while no manager is crowned would re-open every APK on the device and re-walk its
EOCD, signing block and pair list. The cache stores only the hash, never the path, and
never re-compares on a hit, so a collision would make a genuine manager APK invisible.

A superblock check keeps the walk safe. The `s_magic` of the first directory opened,
`/data/app` itself, is recorded, and any later directory whose magic differs is closed and
skipped:

```c
                if (file->f_inode->i_sb->s_magic != data_app_magic) {
                    pr_info("%s: skip: %s magic: 0x%lx expected: 0x%lx\n", __func__, pos->dirpath,
                            file->f_inode->i_sb->s_magic, data_app_magic);
```

Play Store incremental installs mount `incfs` under `/data/app`, and reading a `base.apk`
there can block waiting for blocks to arrive over the network. This walk frequently runs
inside `system_server`'s `rename`, where a stall is a visible system hang. Comparing
`s_magic` costs nothing, needs no path lookup and no reference counting. Directories are
opened `O_RDONLY | O_NOFOLLOW`, so a planted symlink cannot redirect the walk.

On a match, `crown_manager()` re-derives the package name from the winning path, walks the
uid list for a name match and calls `ksu_set_manager_appid()`; the scan stops and the hash
cache is dropped. Whether or not discovery ran, `track_throne()` ends by calling
`ksu_prune_allowlist()` in [`allowlist.c`](../policy/allowlist.c) with a local
`is_uid_exist()` predicate, dropping every allowlist entry whose
`(uid % PER_USER_RANGE, package)` pair is absent from `packages.list`. One row is exempt:
`KSU_APP_PROFILE_PRESERVE_UID` (9999, key `$`), the synthetic entry that carries the global
default non-root profile. Pruning also refuses to run while `ksu_boot_completed` is false,
so callers need no guard: early in boot `packages.list` may not list every package yet, and
pruning then would destroy the user's allowlist.

Nothing here overrides credentials. The `packages.list` read and the `/data/app` walk run
as whatever task triggered them -- `system_server` on the fsnotify path, and on late load
the `ksud` process, which [`init.c`](../core/init.c) has already moved into `u:r:ksu:s0`
via `escape_to_root_for_init()` before calling `track_throne(false)`.

## Watching packages.list

[`pkg_observer.c`](pkg_observer.c) is the only thing that re-triggers discovery at
runtime. It allocates one `fsnotify_group` whose sole operation is `handle_inode_event`,
resolves `/data/system` with `kern_path()`, pins the inode with `ihold()` and adds a bare
`fsnotify_mark` with mask `FS_CREATE | FS_MOVE | FS_EVENT_ON_CHILD`. `FS_MOVE` is
`FS_MOVED_FROM | FS_MOVED_TO`, and `FS_EVENT_ON_CHILD` is what makes a mark on a directory
report events for entries inside it. PackageManagerService writes `packages.list` by
creating a temporary file and renaming it into place, so those two event classes catch
every durable update with no polling and no timer.

The handler drops nameless and directory events, compares thirteen bytes against
`packages.list`, and calls `track_throne(false)` inline. It runs synchronously in the
context of the task doing the rename, which is exactly why the incfs skip and the APK hash
cache matter. Using the fsnotify backend rather than an inotify file descriptor also keeps
the watch invisible: nothing appears in `/proc/<pid>/fd` and no `fdinfo` names a watch on
`/data/system`.

`ksu_observer_init()` has no idempotence guard. Both the group pointer `g` and the single
static `g_watch` are overwritten on a second call, which strands the first group, its mark
and the inode reference `ihold()` took, and leaves that first mark attached to
`/data/system` -- so every later `packages.list` rename runs `track_throne()` twice. One
thing stands between the module and that state: the `static bool done` in
`on_post_fs_data()` ([`boot_event.c`](../runtime/boot_event.c)). A normal boot leans on it
every time, because `on_post_fs_data()` has two callers -- `ksud post-fs-data` reporting
`EVENT_POST_FS_DATA` through `KSU_IOCTL_REPORT_EVENT`, then the first
`/system/bin/app_process -Xzygote` reaching the [`execve`][execve-2] hook -- and the second
one finds `done` already set and logs `on_post_fs_data already done`. The hook then disarms
itself anyway: `ksu_handle_execveat_ksud()` clears its `first_zygote` flag and calls
`ksu_stop_ksud_execve_hook()`, which flips the `ksud_execve_key` static branch off.

A late load is where the guard never gets its chance. `kernelsu_init()` calls
`ksu_observer_init()` directly and never routes through `on_post_fs_data()`, so `done` is
still false, and the ioctl cannot set it either: `do_report_event()` refuses to run either
boot callback once `ksu_late_loaded` is true. Meanwhile `ksu_syscall_hook_manager_init()`
registers the `execve` and [`execveat`][execveat-2] syscall hooks on that path exactly as it
does on the normal one, and `ksud_execve_key` is still enabled. Restart zygote afterwards --
a framework restart is enough -- and the first `/system/bin/app_process -Xzygote` walks into
`on_post_fs_data()` with `done` false and initialises the observer a second time.

The failure at the other end is quieter. `watch_one_dir()`'s return value is stored and
then ignored, so an unresolvable `/data/system` yields one `pr_info("path not ready")` and
a module that never notices another package install.

## When each piece runs

On a normal boot [`init.c`](../core/init.c) calls `ksu_throne_tracker_init()`
(deliberately empty) and `ksu_ksud_init()`; the observer comes up later from
`on_post_fs_data()`, which has two callers. `ksud post-fs-data` reports
`EVENT_POST_FS_DATA` over `KSU_IOCTL_REPORT_EVENT` as its first act, and that is what
normally brings the observer up: the rc stanza
[`ksud_integration.c`](../runtime/ksud_integration.c) injects runs that binary from
`on post-fs-data`, ahead of the trigger that starts zygote. The `execve`/`execveat` hook
calls it again on the first `/system/bin/app_process -Xzygote` and finds the work already
done. Every `packages.list` write after that drives `track_throne(false)`. When `ksud`
reports boot completion through the same ioctl, `on_boot_completed()` sets
`ksu_boot_completed` and calls `track_throne(true)` -- prune only, since an earlier
`packages.list` event has already crowned the manager. On a late load (`ksu_late_loaded`,
i.e. `current->pid != 1` at `insmod`) `kernelsu_init()` does all of it inline -- tracker
init, observer init, `ksu_boot_completed`, one `track_throne(false)` -- and
`do_report_event()` in [`dispatch.c`](../supercall/dispatch.c) skips both boot callbacks.
Teardown runs in `kernelsu_exit()` after the `synchronize_rcu()` that separates hook
removal from data-structure release: `ksu_observer_exit()`, then
`ksu_throne_tracker_exit()`, which frees the whole `apk_path_hash_list`.

## What the crown is worth

[`setuid_hook.c`](../hook/setuid_hook.c) is the most consequential consumer. On a successful
`setresuid` to the manager's appid it takes `current->sighand->siglock`, sets the
`__NR_reboot` bit in the task's [seccomp][seccomp-filter] constant-action bitmap via
[`seccomp_cache.c`](../infra/seccomp_cache.c), marks the task for the syscall
[tracepoint][tracepoints] and calls `ksu_install_fd()`, so the `[ksu_driver]` anonymous
inode is open in the app's file table before its first line of Java runs;
[`ksu.cc`](../../manager/app/src/main/cpp/ksu.cc) finds it by `readlink`ing every entry of
`/proc/self/fd`. The seccomp poke matters because the other route to that fd -- the
[`reboot(2)`][reboot-2] [kprobe][kprobes] in [`supercall.c`](../supercall/supercall.c),
which recognises the magic pair `(0xDEADBEEF, 0xCAFEBABE)` and queues a `task_work` to
install the fd -- is blocked for apps by the filter zygote installs, not by any credential
check of its own.

Holding the fd is therefore not authority. `reboot_handler_pre` checks no credentials; the
real gate is the per-command `perm_check` in [`perm.c`](../supercall/perm.c).
`only_manager()` is bare `is_manager()`, and it guards exactly two commands,
`KSU_IOCTL_GET_APP_PROFILE` and `KSU_IOCTL_SET_APP_PROFILE`. `manager_or_root()`
(`uid == 0 || is_manager()`) guards thirteen more, and `allowed_for_su()` admits the
manager to `KSU_IOCTL_GRANT_ROOT` unconditionally; the full table is in
[`kernel/supercall/README.md`](../supercall/README.md).

The app never decides its own status either. The `is_manager()` on the `ksu.cc` side issues
`KSU_IOCTL_GET_INFO`, a command whose `perm_check` is `always_allow` so any uid may ask,
and reads `KSU_GET_INFO_FLAG_MANAGER` back out of the reply -- a bit `do_get_info()` sets
from the kernel-side `is_manager()` at the instant of the call. An app that has lost the
throne finds out on its next `GET_INFO` and in no other way; nothing observable inside its
own process changes.

[`allowlist.c`](../policy/allowlist.c) folds the same identity into four more decisions:
`__ksu_is_allow_uid()` returns true for the manager before consulting the hash table,
`ksu_uid_should_umount()` returns false so module mounts are never detached from it,
`ksu_get_root_profile()` hands it the default full-root profile, and
`ksu_get_allow_list()` filters it out of the list the UI renders. The umount answer is
ordered on purpose: the manager test runs first, ahead of the webview-zygote branch for
uid 1053 that `KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT` now toggles at runtime and ahead of the
per-app profile lookup, so neither a feature flag nor an allowlist row can reach the
manager's own view of the filesystem.

The threat model follows. An attacker who gets an APK crowned obtains `SET_APP_PROFILE`, the
ability to write an arbitrary `struct app_profile` for any uid on the device: `allow_su`, an
arbitrary uid/gid/supplementary group set, arbitrary effective, permitted and inheritable
capability sets, an arbitrary [SELinux][selinux] domain and a mount-namespace mode,
persisted to `/data/adb/ksu/.allowlist` and surviving reboot. They also get `GRANT_ROOT`
directly, root-equivalent access to every `manager_or_root()` command, and immunity from the
umount and mount-hiding machinery -- `ksu_should_hide_mount_for_current()` in
[`mount_hide.c`](../feature/mount_hide.c) reaches that exemption through the same
`ksu_uid_should_umount()`. One edge stays closed: the immunity attaches to the uid, not to
the app, and an isolated process is filtered whatever the crown says, so a crowned
attacker's isolated children still read a censored `/proc/self/mountinfo`. There is
otherwise no meaningful gap between "crowned" and "owns the device".

The boundary is softer than it looks. The existence test compares `np->uid ==
ksu_get_manager_appid()` -- uid only, never the package name -- so if the manager is
uninstalled and Android reassigns that appid before the next `packages.list` event, the
throne is never invalidated and the new occupant inherits everything, fd included. An APK is
never re-verified after crowning either: an in-place update keeps the uid, so the line stays
in `packages.list` and `search_manager()` never runs again for the life of the boot. And
there is no locking anywhere in this directory -- no mutex, no spinlock, no
[RCU][whatisrcu], no `READ_ONCE` -- while `track_throne()` is reachable concurrently from
the fsnotify handler, from `KSU_IOCTL_REPORT_EVENT` and from module init, mutating both
`apk_path_hash_list` and `ksu_manager_appid`. Correctness rests on PackageManagerService
serialising its own writes.

One build dependency is easy to miss: `CONFIG_KSU` in [`Kconfig`](../Kconfig) depends only
on `KPROBES` and `EXT4_FS` and does not select `CRYPTO_SHA256`. Without it the module
builds and loads cleanly and every manager check fails at `crypto_alloc_shash("sha256")`,
logging "can't alloc alg sha256".

## CONFIG_KSU_DISABLE_MANAGER

[`Kbuild`](../Kbuild) drops the three object files from `kernelsu-objs` and, for
out-of-tree builds, re-adds `-DCONFIG_KSU_DISABLE_MANAGER=1` to `ccflags-y`, because an
external module does not see the in-tree Kconfig symbol through `autoconf.h`. The headers
then supply a complete alternative implementation rather than making callers use `#ifdef`:
[`manager_identity.h`](manager_identity.h) redefines `is_manager()` as a test of
`current_uid().val` against 0, `is_uid_manager(uid)` as `uid == 0`,
`ksu_get_manager_appid()` as 0 and `ksu_is_manager_appid_valid()` as unconditionally true,
with the setters as no-ops, while [`throne_tracker.h`](throne_tracker.h) and
[`manager_observer.h`](manager_observer.h) turn their five entry points into empty inlines.
Every caller compiles unchanged either way, which is why the stub signatures must track the
real ones exactly.

The configuration targets deployments that ship no manager app and want none of the APK
parser, the directory walker or the fsnotify group. Nothing then calls
`ksu_prune_allowlist()`, so entries for uninstalled apps accumulate forever. And
`__ksu_is_allow_uid(0)` still returns false, because `forbid_system_uid()`, which rejects
any uid below 2000 other than 1000, runs before the `is_uid_manager()` branch; root is
admitted through the separate `is_ksu_domain()` check in
`__ksu_is_allow_uid_for_current()` instead. Meanwhile
[`setuid_hook.c`](../hook/setuid_hook.c) installs the `[ksu_driver]` fd on every
successful `setresuid` to 0.

## See also

- [`kernel/README.md`](../README.md) -- build modes, init order, layer map
- [`kernel/core/README.md`](../core/README.md) -- `kernelsu_init` / `kernelsu_exit` and
  the Kbuild knobs
- [`kernel/policy/README.md`](../policy/README.md) -- the allowlist and app profiles this
  identity gates
- [`kernel/supercall/README.md`](../supercall/README.md) -- the ioctl table and the
  permission predicates
- [`kernel/hook/README.md`](../hook/README.md) -- the `setresuid` hook that plants the
  driver fd
- [`kernel/runtime/README.md`](../runtime/README.md) -- the boot milestones that drive the
  tracker
- [`uapi/README.md`](../../uapi/README.md) -- the wire structs behind the manager flags
  and profiles
- [`manager/README.md`](../../manager/README.md) -- the Android app on the other side of
  the crown
- [`docs/architecture.md`](../../docs/architecture.md) -- repository-wide map

<!-- reference links: kernel documentation and man pages -->
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[execveat-2]: https://man7.org/linux/man-pages/man2/execveat.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[setresuid-2]: https://man7.org/linux/man-pages/man2/setresuid.2.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
[whatisrcu]: https://docs.kernel.org/RCU/whatisRCU.html
