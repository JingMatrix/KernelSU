# Policy: who may become root, and what root means for them

Three files decide every access question the module asks. [`allowlist.c`](allowlist.c)
holds the store of per-app records and answers "may this uid become root?" and "should
module mounts be taken away from this uid?". [`app_profile.c`](app_profile.c) is the only
place in the tree that turns a stored record into real kernel credentials.
[`feature.c`](feature.c) maps a small integer to a pair of get/set callbacks owned
elsewhere, which is how the manager and `ksud` toggle optional features at runtime.

Everything here is reached over the `[ksu_driver]` anon-inode fd through the table in
[`supercall/dispatch.c`](../supercall/dispatch.c), which attaches a predicate from
[`supercall/perm.c`](../supercall/perm.c) to each command: `GET_APP_PROFILE` (`'K'`, 11)
and `SET_APP_PROFILE` (12) are `only_manager`, `GET_FEATURE` (13), `SET_FEATURE` (14),
`UID_SHOULD_UMOUNT` (9), `UID_GRANTED_ROOT` (8) and the allow-list readers (6, 7) are
`manager_or_root`, `GRANT_ROOT` (1) is `allowed_for_su`. None uses `capable()`.

## The allowlist

### In-memory representation

There are no uid bitmaps in this fork. Every record lives in one heap object:

```c
struct perm_data {
    struct hlist_node list;
    struct rcu_head rcu;
    struct kref ref;
    struct app_profile profile;
};
```

Those hang off `static DEFINE_HASHTABLE(allow_list, ALLOW_LIST_BITS)` with
`ALLOW_LIST_BITS` 8, so 256 buckets, hashed on `profile.curr_uid` - the full Android uid
including the user offset, 1010123 for the work-profile clone (user 10) of appid 10123,
not the appid. `allow_list_count` is a `u16`; a new insert is refused with `-E2BIG` at
`U16_MAX`. `curr_uid` is the only lookup key. The `key` field carries the package name; it
gates the `"$"` pseudo-entry described below, validates a record during pruning, prints in
log lines, and produces a `key changed` warning when a set overwrites an entry whose
package differs.

Reading the whole table back out is `ksu_get_allow_list()`, the enumerator behind ioctls 6
and 7 - the deny list is the same walk with `allow` false. It takes `rcu_read_lock()`,
walks every bucket with `hash_for_each_rcu`, and skips any entry `is_uid_manager()`
matches, so the manager never appears in the list it is displaying. Two counts come back:
how many uids were copied into the caller's array, and how many matched in total.
`do_new_get_allow_list_common()` in [`supercall/dispatch.c`](../supercall/dispatch.c)
returns both, so a caller whose buffer was too small can tell it was truncated and ask
again with a bigger one; the deprecated `'K', 6` form caps the walk at 128 uids and passes
`NULL` for the total, so truncation there is invisible. `ksu_show_allow_list()` walks the
same table into the kernel log, manager entry included, and `ksu_load_allow_list()` calls
it on both exit paths that follow a successful open, so a boot log records what was
actually restored.

Two profiles are cached outside the table because they are consulted constantly.
`init_default_profiles()`, marked `__init` and called from `ksu_allowlist_init()`, fills
`default_root_profile` with uid 0, gid 0, one supplementary group 0, `CAP_FULL_SET` in
`capabilities.effective`, `KSU_NS_INHERITED`, `flags` 0, and a `selinux_domain` of
`KSU_DEFAULT_SELINUX_DOMAIN` - built by the preprocessor as `"u:r:" KERNEL_SU_DOMAIN
":s0"` from [`selinux/selinux.h`](../selinux/selinux.h), so `u:r:ksu:s0`. It also sets
`default_non_root_profile.umount_modules` to true, so an app with no record has module
mounts taken away from it.

That second default is settable through a pseudo-entry: `ksu_set_app_profile()` returns
`-EINVAL` for `curr_uid == KSU_APP_PROFILE_PRESERVE_UID` (9999) unless `key` is exactly
`"$"`, and on success copies `nrp_config.profile.umount_modules` into
`default_non_root_profile`. Because 9999 is an ordinary table entry it is persisted and
reloaded like any other, which is how the toggle survives a reboot, and
`ksu_prune_allowlist()` skips it. The manager drives it through
`NON_ROOT_DEFAULT_PROFILE_KEY` and `NOBODY_UID` in
[`Natives.kt`](../../manager/app/src/main/java/me/weishu/kernelsu/Natives.kt).

### Persistence file and format

The table is written verbatim to `/data/adb/ksu/.allowlist`: a `u32` magic `0x7f4b5355`,
a `u32` `FILE_FORMAT_VERSION` of 4, then a flat run of raw `struct app_profile` records
with no count, no length prefix and no padding. At version 4 that record is 784 bytes;
before `flags` was appended to `struct root_profile` it was 776, which the loader carries
as `kAppProfileSizePreV4`. Since the stride is implicit in the version number, bumping
`KSU_APP_PROFILE_VER` in [`uapi/app_profile.h`](../../uapi/app_profile.h) obliges you to
add a `case` to `migrate_profile()` and a size constant beside `kAppProfileSizePreV4`, or
the reader desynchronises after the first record. `FILE_FORMAT_VERSION`, a second constant
local to `allowlist.c` and the one the writer actually stamps into the header, has to move
in step, or freshly saved files keep advertising the old layout.

Writing never happens in the caller's context. `ksu_persistent_allow_list()` resolves
PID 1 with `get_pid_task(find_vpid(1), PIDTYPE_PID)`, allocates a bare
`struct callback_head`, points it at `do_persistent_allow_list` and queues it with
`task_work_add(tsk, cb, TWA_RESUME)`. Task work is a callback the kernel runs in another
task's context the next time that task returns to userspace, so it is sleepable and it
runs in that task's namespaces. Both properties are the point: the caller of
`SET_APP_PROFILE` is the manager app, whose mount namespace may not contain `/data/adb`
and whose domain has no rule for the file, while PID 1 always has the global namespace
and `do_persistent_allow_list()` wraps the whole open-write-close in
`override_creds(ksu_cred)`, so the 0644 file is written as `u:r:ksu:s0`, which
[`selinux/rules.c`](../selinux/rules.c) blanket-allows. Three events queue a save: a
successful `do_set_app_profile()`, a `ksu_prune_allowlist()` that removed something, and
a `ksu_load_allow_list()` that read a pre-v4 file. The source carries a
`// TODO: move to kernel thread or work queue`, and `TWA_RESUME` is why - the save lands
only when init happens to return to userspace.

Loading is the mirror image minus the cred override, because both call sites are already
privileged and in init's namespace. `ksu_load_allow_list()` checks the magic, accepts a
version between 2 and `KSU_APP_PROFILE_VER`, picks the stride, then reads records into a
stack `struct app_profile`, each passed through `migrate_profile()` and
`ksu_set_app_profile()`. `migrate_profile()` is a fallthrough switch: version 2 rewrites a
`selinux_domain` of exactly `"u:r:su:s0"` to `KSU_DEFAULT_SELINUX_DOMAIN` and falls through,
version 3 sets `flags = FLAG_KSU_NO_NEW_PRIVS`; both touch only `allow_su` records. The
domain rewrite exists because this fork renamed its [SELinux][selinux] domain from `su` to
`ksu`: without it a saved root profile names a type that no longer exists,
`security_secctx_to_secid()` fails, and the escalated task stays in the app's domain.

Two format details deserve a warning. The migration path reads 776 bytes into a 784-byte
stack struct that is never zeroed, and `migrate_profile()` writes `flags` only for
`allow_su` records, so a non-root record carries eight bytes of uninitialised kernel
stack into the table and back out to the 0644 file on disk. And
[`scripts/allowlist.bt`](../../scripts/allowlist.bt), the 010 Editor template, still
describes the pre-v4 776-byte record.

### Concurrency discipline

[RCU][whatisrcu] is the read side. A reader enters `rcu_read_lock()`, walks the bucket and
leaves; a writer that removes a node cannot free it until every reader already inside such a
section has left, which is what a grace period means. So `release_perm_data()` calls
`kfree_rcu(p, rcu)` rather than `kfree`. Every writer holds `allowlist_mutex`:
`ksu_set_app_profile()`, `ksu_prune_allowlist()`, `ksu_allowlist_exit()`, and the save walk
in `do_persistent_allow_list()`. Writers use the plain `hash_for_each_possible` /
`hash_for_each_safe` iterators, readers the `_rcu` variants. A record is never mutated in
place: updating a uid allocates a fresh `perm_data`, `kref_init`s it, memcpys the whole
784-byte profile in, calls `hlist_replace_rcu(&p->list, &np->list)` and drops the old node's
reference, so a reader sees the complete old profile or the complete new one, never a new
`selinux_domain` paired with the old capability set.

Handing a profile to a caller that will sleep needs more than RCU, which is what the
`kref` is for. Both getters take one: `ksu_get_app_profile()` and `ksu_get_root_profile()`
walk the bucket and call `kref_get_unless_zero(&p->ref)`; if that fails the node is dying
and the walk restarts at the `retry` label. The reference `ksu_get_root_profile()` returns
is what lets `escape_with_root_profile()` hold a `struct root_profile *` across
`commit_creds()`, `disable_seccomp()` and `setup_mount_ns()`, all of which sleep, while
the manager rewrites the record. The two release helpers are not interchangeable:
`ksu_put_app_profile()` does `container_of(profile, struct perm_data, profile)` while
`ksu_put_root_profile()` does
`container_of(profile, struct perm_data, profile.rp_config.profile)` and returns at once
for the shared `&default_root_profile`, which has no `perm_data` around it. Every path
must release, error paths included - see `out_abort_creds` in
`escape_with_root_profile()`. Teardown depends on ordering in
[`core/init.c`](../core/init.c): `kernelsu_exit()` stops the hook sources, tears down
uhook, ptctl and the supercall fd, calls `synchronize_rcu()` - its comment names "handler
traversing allow_list" as the reader it waits for - and only then runs
`ksu_allowlist_exit()`, which is why that walk can use plain `hlist_del`.

### How a uid is looked up on the hot path

`__ksu_is_allow_uid()` is the cheap boolean and takes no reference. It rejects at once
when `forbid_system_uid()` says the uid is below `SHELL_UID` (2000) and is not
`SYSTEM_UID` (1000), so no native daemon uid - root included - can be granted by a
crafted `.allowlist`. It then returns true for `is_uid_manager()`, true for uid 2000 when
the `allow_shell` module parameter from [`include/ksu.h`](../include/ksu.h) is set, and
otherwise scans the bucket under `rcu_read_lock()` for a record with matching `curr_uid`
and `allow_su`. The `ksu_is_allow_uid` macro in [`allowlist.h`](allowlist.h) wraps it in
`unlikely()`, because almost every call misses.
`__ksu_is_allow_uid_for_current()` adds the rule that matters: for uid 0 it does not
consult the table at all, it returns `is_ksu_domain()` from
[`selinux/selinux.c`](../selinux/selinux.c). Any root process on the device would
otherwise pass `allowed_for_su()` and drive the whole supercall interface; requiring
`u:r:ksu:s0` narrows that to processes KernelSU itself created or transitioned.

Which root a caller gets is decided by `ksu_get_root_profile(uid)`, in this order: the
manager gets `default_root_profile`; uid 2000 with `allow_shell` gets
`default_root_profile`; a table entry that is `allow_su` with
`rp_config.use_default == false` gets its own embedded profile with a reference taken;
everything else falls back to the default. The two hard-wired cases exist so that
misconfiguring the manager's own profile cannot lock you out of fixing it.

The non-root question goes to `ksu_uid_should_umount(uid)`. It returns false for the
manager. For `WEBVIEW_ZYGOTE_UID` (1053) it returns `ksu_webview_zygote_umount_enabled`,
a bool owned by [`feature/kernel_umount.c`](../feature/kernel_umount.c) and defaulting to
false: uid 1053 belongs to no package, so it can never have a record to consult, and the
answer has to come from a toggle instead of the table. Every other uid goes through
`rcu_read_lock()` and the record: no record gives the global default, an `allow_su`
record gives false (a rooted app keeps the module view), and a non-root record gives the
global default or its own `umount_modules`. Three consumers share the answer, which is why
it lives here - `ksu_handle_umount()` in
[`feature/kernel_umount.c`](../feature/kernel_umount.c) and
`ksu_should_hide_mount_for_current()` in
[`feature/mount_hide.c`](../feature/mount_hide.c), plus `KSU_IOCTL_UID_SHOULD_UMOUNT` for
userspace hiding modules. Those first two answer different questions - unmounting changes
what is mounted for a process, filtering changes what `/proc` prints for a reader - so a
disagreement between them is observable by a detection app. For uid 1053 the two do not
even overlap. `ksu_should_hide_mount_for_current()` drops the webview zygote at its
uid classification, because 1053 is neither an appuid nor isolated, so the umount toggle
is the only lever on what that process itself has mounted; the sandboxed renderers it
forks land in the isolated range and are filtered whichever way the toggle sits. Neither
mechanism makes the other redundant.

Both of those consumers classify the uid before they ever reach the table, using two
inline predicates that [`allowlist.h`](allowlist.h) publishes for exactly that purpose.
Android composes an app uid as `user * PER_USER_RANGE + appid`, so each helper reduces
with `uid % PER_USER_RANGE` and then tests the appid: `is_appuid()` accepts
[10000, 19999], and `is_isolated_process()` ors two windows that abut - the classic
isolated range [99000, 99999], and the app-zygote range [90000, 98999] named by
`FIRST_APP_ZYGOTE_ISOLATED_UID` and `LAST_APP_ZYGOTE_ISOLATED_UID` - so the predicate
answers true for every appid from 90000 through 99999. The lower window is a fork
addition. An app zygote - Chrome's sandboxed renderers are the common case - forks
children into that band, and while it was unrecognised those children were neither
unmounted by `ksu_handle_umount()` nor filtered by
`ksu_should_hide_mount_for_current()`, which is precisely the gap a detection app running
as an isolated process walked through. Both consumers now read that one helper;
[`feature/mount_hide.c`](../feature/mount_hide.c) keeps no private copy of the test, so
the unmounter and the `/proc` filter cannot drift apart on who counts as isolated.

Pruning runs from `track_throne()` in
[`manager/throne_tracker.c`](../manager/throne_tracker.c) and does nothing while
`ksu_boot_completed` (owned by [`runtime/boot_event.c`](../runtime/boot_event.c)) is
false, because `/data/system/packages.list` is not readable early in boot and pruning
against an empty parse would delete every grant. Its callback `is_uid_exist()` requires
both `np->uid == uid % PER_USER_RANGE` and an exact `strncmp` of the package name, so a
reinstall or a recycled appid drops the stale grant.

## App profiles

`struct app_profile` in [`uapi/app_profile.h`](../../uapi/app_profile.h) is the ABI,
shared verbatim with the manager's JNI and with the on-disk file.

| Field | Type | Meaning |
| --- | --- | --- |
| `version` | `__u32` | Must equal `KSU_APP_PROFILE_VER` (4) or `profile_valid()` rejects the record |
| `key` | `char[256]` | Package name; must be NUL-terminated within the array |
| `curr_uid` | `__s32` | Full Android uid, the hash key |
| `allow_su` | `bool` | Selects which arm of the union is live |
| `rp_config` | union arm | `use_default`, `template_name[256]`, `struct root_profile profile` |
| `nrp_config` | union arm | `use_default`, `struct non_root_profile profile` |

`struct root_profile`, the interesting half:

| Field | Type | Applied by |
| --- | --- | --- |
| `uid`, `gid` | `__s32` | Written into all four uid and all four gid slots of the new cred |
| `groups_count`, `groups[32]` | `__u32`, `__s32[]` | `setup_groups()` |
| `capabilities.effective` | `__u64` | Copied into `cap_effective`, `cap_permitted` and `cap_bset` |
| `capabilities.permitted`, `.inheritable` | `__u64` | Never read by the kernel |
| `selinux_domain` | `char[64]` | `setup_selinux()` |
| `namespaces` | `__s32` | `setup_mount_ns()`; 0 inherited, 1 global, 2 individual |
| `flags` | `__u64` | Only `FLAG_KSU_NO_NEW_PRIVS` is defined |

`struct non_root_profile` is a single `bool umount_modules`. The union overlaps
`rp_config.use_default` with `nrp_config.use_default` at offset 0, and
`rp_config.template_name[0]` with `nrp_config.profile.umount_modules` at offset 1, which
matters for exactly one record: an `allow_su` profile submitted for uid 9999 with key
`"$"` would set the global umount default from the first byte of its template name.

`template_name` is passthrough storage - nothing under `kernel/` reads it, the only
kernel reference being the `memset` under `CONFIG_KSU_DISABLE_POLICY`. Template bodies
live in `/data/adb/ksu/profile/templates/`, owned by
[`ksud/src/profile.rs`](../../userspace/ksud/src/profile.rs); the manager expands a
template into a complete `Profile` before calling `setAppProfile`
([`jni.cc`](../../manager/app/src/main/cpp/jni.cc)), and `ksud` applies the template's
SELinux half at post-fs-data. `profile_valid()`, the only gate on ABI input, runs before
any insert and rejects an unterminated `key`, a `version` that is not exactly 4, and -
for `allow_su` records, and only when policy is enabled - a `groups_count` above
`KSU_MAX_GROUPS` or an empty `selinux_domain`. Migration is the loader's job.

### Turning a profile into a cred change

`escape_with_root_profile()` in [`app_profile.c`](app_profile.c) has two callers:
`do_grant_root()` behind `KSU_IOCTL_GRANT_ROOT`, and `ksu_handle_execve_sucompat_common()`
in [`feature/sucompat.c`](../feature/sucompat.c), after it has rewritten the syscall
registers and just before it dispatches the call as an [`execveat`][execveat-2] of
`/data/adb/ksud`. That second site is entered from two thin wrappers,
`ksu_handle_execve_sucompat()` and `ksu_handle_execveat_sucompat()`, because recent bionic
spells the same exec as `execveat(AT_FDCWD, path, ..., 0)` where older versions issued
[`execve`][execve-2], and a `su` that watched only `execve` would silently stop working on
those devices. The wrappers differ in which registers carry `argv` and `envp`; the
`execveat` form additionally bails out unless the dirfd is `AT_FDCWD` and the flags are
zero, since only that spelling is equivalent to the `execve` the rest of the path assumes.
It runs in this order.

1. `prepare_creds()`, then bail out if `cred->euid.val == 0` or if
   `TIF_KSU_DISABLE_ESCAPE_WITH_ROOT` is already set on the thread.
2. `ksu_get_root_profile(cred->uid.val)` - the key is the **real** uid, so a task that
   already changed its real uid gets the new uid's profile.
3. All four uid slots and all four gid slots are overwritten, `securebits` is zeroed.
4. `alloc_uid()` / `free_uid()` re-home `cred->user`; on 5.14 and later
   `set_cred_ucounts(cred)` refreshes `cred->ucounts`.
5. `capabilities.effective` is copied into `cap_effective`, `cap_permitted`, `cap_bset`.
6. `setup_groups()`, `setup_selinux()` ([`selinux/selinux.c`](../selinux/selinux.c)),
   `commit_creds()`.
7. `disable_seccomp()`, the `FLAG_KSU_NO_NEW_PRIVS` latch, a `for_each_thread` walk
   calling `ksu_set_task_tracepoint_flag()`
   ([`hook/tp_marker.h`](../hook/tp_marker.h)), then `setup_mount_ns()`
   ([`infra/su_mount_ns.c`](../infra/su_mount_ns.c)).

Step 4 looks gratuitous and is not. `commit_creds()` moves the RLIMIT_NPROC charge based on
`cred->user`, so changing the uid while leaving `user` and `ucounts` on the old identity
keeps the process count charged to the app's uid forever, and that uid eventually cannot
fork; the source cites `kernel/sys.c:set_user()` for the pattern. Step 7's thread walk
exists because the syscall hooks fire only for tasks marked for the `sys_enter`
[tracepoint][tracepoints], so a freshly rooted process that is not re-marked stops being
hooked - the same reason `do_set_app_profile()` calls `ksu_mark_running_process()`.

A fast path in `setup_groups()` is worth reading carefully. For a profile of exactly one
group whose gid is 0 it installs the file-static `root_groups`, whose `ngroups` is 0 -
that is an *empty* supplementary list, not one containing gid 0. The slow path
converts each `gid_t` with `make_kgid(current_user_ns(), gid)` and calls `groups_sort()`
before `set_groups()`; the sort is mandatory, because `groups_search()` binary-searches
and an unsorted list silently returns wrong membership answers.

Every app inherits a [seccomp][seccomp-filter] filter from zygote, and `disable_seccomp()`
tears it down, because that filter would otherwise block syscalls a root shell needs.
Clearing `current->seccomp.filter` alone would leak the filter chain and its [BPF][bpf]
programs, so the function takes `current->sighand->siglock`, clears the seccomp work bit,
memcpys `current` into a throwaway `task_struct`, zeroes the real task's seccomp state, and
calls `seccomp_filter_release()` on the fake. That fake is doctored first, because upstream
put assertions inside `seccomp_filter_release()` that hold only for a task which is really
exiting, and the assertion changed shape: 5.11 through 6.10 expect the task to have no
`sighand` at all (that absence is how the caller signals it already owns `siglock`), while
6.11 and later expect `PF_EXITING` in `task->flags` and take `siglock` themselves. So the
fake is handed a NULL `sighand` on the older kernels and `PF_EXITING` on the newer ones, and
either way the filter chain and its BPF programs are dropped instead of leaked.

Picking between those two shapes by `LINUX_VERSION_CODE` alone breaks on 6.6, which is
what `ksu_app_profile_init()` exists to repair. The `PF_EXITING` rework was backported
into some Android 15 6.6 kernels and not others - a Pixel 10 carries it, a stock GKI 6.6
does not - so two kernels reporting the same version want opposite treatment, and the
wrong choice trips the very assertion the fake was built to satisfy. Inside the
`NEED_BACKPORT_COMPAT` window - 6.6 up to but not including 6.11 - the module therefore
reads the running kernel's own text. `find_kernel_symbol_exact()` from
[`infra/symbol_resolver.c`](../infra/symbol_resolver.c) resolves both
`seccomp_filter_release` and `_raw_spin_lock_irq`, `kallsyms_lookup_size_offset()` gives
the length of the former (128 bytes is assumed if the lookup fails), and `scan_call_to()`
from [`hook/patch_memory.h`](../hook/patch_memory.h) walks those instructions for a `BL`
whose sign-extended displacement lands exactly on the spinlock helper. A hit means the
backport is present and `disable_seccomp()` should set `PF_EXITING`; a miss means the
pre-backport shape and a NULL `sighand`. The verdict is latched once into
`has_call_to_spin_lock`, which is why the function is `__init` and why
[`core/init.c`](../core/init.c) calls it from `kernelsu_init()` immediately after
`ksu_supercalls_init()`. On x86_64 `scan_call_to()` is a stub that returns NULL, so the
probe there always reports the pre-backport shape.

`FLAG_KSU_NO_NEW_PRIVS` is a per-thread latch rather than cred state:
[`app_profile.h`](app_profile.h) defines `TIF_KSU_DISABLE_ESCAPE_WITH_ROOT` as bit 63, a
bit the kernel itself never uses, set with `set_thread_flag()` so it survives
`commit_creds()`. Nothing ever clears it. It blocks the documented double escalation: a
profile maps the app to uid 2000, uid 2000 is itself granted, and a second `su` would
land on full root. Userspace can set the bit with `KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT`
(`_IO('K', 21)`, `only_root`). The latch stops only KernelSU's own escalation path; it is
not `PR_SET_NO_NEW_PRIVS`. See
[`app-profile.md`](../../website/docs/guide/app-profile.md), which documents the same
scenario from the user's side.

`escape_to_root_for_init()` is the smaller sibling: `prepare_creds()`, `setup_selinux()`
with `KERNEL_SU_CONTEXT`, `commit_creds()` - only the domain changes. It runs from
[`core/init.c`](../core/init.c) on late load and from
[`hook/syscall_event_bridge.c`](../hook/syscall_event_bridge.c) on an exec of `ksud`.

## The feature registry

[`feature.c`](feature.c) is not a bitmap. It is
`static const struct ksu_feature_handler *feature_handlers[KSU_FEATURE_MAX]`, one mutex,
no state of its own and no persistence. [`feature.h`](feature.h) carries the contract: the
`ksu_feature_get_t` and `ksu_feature_set_t` callback typedefs, the
`struct ksu_feature_handler` that pairs them with an id and a name, and - beside the
register, get and set entry points - `ksu_unregister_feature_handler()`. Each feature's
value is a full `u64` carried by `struct ksu_get_feature_cmd` /
`struct ksu_set_feature_cmd`, used as a boolean by all seven.
`ksu_register_feature_handler()` is `__init`, so handlers may only be registered during
module bring-up, and only the pointer is stored - the `struct ksu_feature_handler` must
have static storage duration. That is also why unregistration exists: the pointer aims at
static data inside the registering file, so every feature's `__exit` path drops its own
slot and `ksu_feature_exit()` NULLs whatever is left, leaving no pointer into module data
behind when the module text is freed. `ksu_get_feature()` reports
`supported = false, value = 0` and returns 0 for an id with no handler, which lets one
manager binary probe a kernel built with a different feature set; `ksu_set_feature()`
returns `-EOPNOTSUPP`. Because `feature_mutex` is held across the callbacks themselves, a
handler must never call back into `ksu_get_feature()` or `ksu_set_feature()`.

The ids come from [`uapi/feature.h`](../../uapi/feature.h), and `KSU_FEATURE_MAX` is
reported to userspace as the `features` field of `KSU_IOCTL_GET_INFO`:

| Id | Name | Default | Owner and read site |
| --- | --- | --- | --- |
| 0 | `su_compat` | on | `ksu_su_compat_enabled` in [`feature/sucompat.c`](../feature/sucompat.c), tested three times in [`hook/syscall_event_bridge.c`](../hook/syscall_event_bridge.c) |
| 1 | `kernel_umount` | on | `ksu_kernel_umount_enabled` in [`feature/kernel_umount.c`](../feature/kernel_umount.c), checked in `ksu_handle_umount()` right after the `ksu_module_mounted` test |
| 2 | `sulog` | off | `ksu_sulog_enabled` in [`feature/sulog.c`](../feature/sulog.c), read by `ksu_sulog_is_enabled()` in the capture path |
| 3 | `adb_root` | off | the `ksu_adb_root` [static key][static-keys] in [`feature/adb_root.c`](../feature/adb_root.c), tested by `ksu_adb_root_handle_execve()` and `ksu_adb_root_handle_execveat()` |
| 4 | `selinux_hide` | off | `ksu_selinux_hide_enabled` in [`feature/selinux_hide.c`](../feature/selinux_hide.c); the setter installs or removes the selinuxfs hooks |
| 5 | `webview_zygote_umount` | off | `ksu_webview_zygote_umount_enabled` in [`feature/kernel_umount.c`](../feature/kernel_umount.c), read by `ksu_uid_should_umount()` for uid 1053 |
| 16 | `mount_hide` | on | `ksu_mount_hide_enabled` in [`feature/mount_hide.c`](../feature/mount_hide.c), tested in `ksu_should_hide_mount_for_current()` |

The jump from 5 to 16 is deliberate and [`uapi/feature.h`](../../uapi/feature.h) says so in
a comment. A feature id is a wire value: it crosses the ioctl boundary in
`struct ksu_set_feature_cmd`, the manager hard-codes it, and `ksud` writes it into
`.feature_config` on disk. Upstream allocates ids from 0 upwards, so a fork that also
allocates from 0 upwards eventually collides - which is exactly what happened when
upstream claimed 5 for `webview_zygote_umount` while this fork was already using 5 for
`mount_hide`. A saved config would then have re-read a stored 5 as the wrong feature after
a rebase. Fork-local features start at 16 instead, leaving upstream ten more ids to spend
before the two allocators can meet again. The cost is a sparse array: `KSU_FEATURE_MAX` is
17 and slots 6 through 15 stay NULL for the life of the module, eighty bytes that buy
nothing and cost nothing, since `ksu_get_feature()` already answers `supported = false`
for an unclaimed slot and `ksu_set_feature()` answers `-EOPNOTSUPP`.

The registry is deliberately dumb because "enabled" means something different for each
feature: `adb_root` flips a static key so the disabled case costs a `nop`, `selinux_hide`
installs and removes the selinuxfs hooks under `selinux_hide_mutex`, `mount_hide` installs
its `.open` hooks under `ksu_mount_hide_mutex` from `ksu_mount_hide_init()` (it defaults
on, so they go in at bring-up rather than on a later set) and then leaves them there,
since disabling only clears `ksu_mount_hide_enabled`, which avoids repeated rodata
patching and brings the hooks down only at module exit - while `sulog` and
`webview_zygote_umount` need nothing more than a flag the hot path already reads.
Registration timing differs too. `ksu_feature_init()` runs third in `kernelsu_init()`,
after `ksu_init_symbol_resolver()` and `ksu_syscall_hook_init()`, and walks the array
setting every slot to NULL, so anything registered earlier is silently discarded; sulog,
adb_root and selinux_hide register from `kernelsu_init()`, su_compat from
`ksu_syscall_hook_manager_init()`, and `ksu_setuid_hook_init()` in
[`hook/setuid_hook.c`](../hook/setuid_hook.c) pulls in the remaining three by calling
`ksu_kernel_umount_init()`, which registers both `kernel_umount` and
`webview_zygote_umount`, and `ksu_mount_hide_init()`.

There is no kernel-side feature file. `/data/adb/ksu/.feature_config` belongs to
[`ksud/src/feature.rs`](../../userspace/ksud/src/feature.rs): magic `0x7f4b5355` (the
same constant as `.allowlist`, different layout), version 1, a count, then
`(u32 id, u64 value)` pairs, replayed at post-fs-data and on late load. A toggle takes
effect at once and becomes durable only on `ksud feature save`, whose `save_config()`
enumerates `SuCompat`, `KernelUmount`, `Sulog`, `AdbRoot`, `SelinuxHide`,
`WebviewZygoteUmount` and `MountHide`, skipping any the kernel reports unsupported. The
same seven names appear in the `FeatureId` enum, in `from_u32`, `name`, `description` and
`parse_feature_id`, in `list_features`, and in the `--help` text of `ksud feature get` and
`ksud feature check`, and every one of them spells `mount_hide` as id 16; a half-updated
copy of that enum is the one way a saved config gets replayed onto the wrong feature.
`apply_config()` is the escape hatch: an id it does not recognise is still passed straight
to `KSU_IOCTL_SET_FEATURE`, so a kernel that knows a feature an older `ksud` does not is
still configurable.

## CONFIG_KSU_DISABLE_POLICY

Declared in [`Kconfig`](../Kconfig) and propagated into out-of-tree builds by
[`Kbuild`](../Kbuild), which adds `-DCONFIG_KSU_DISABLE_POLICY=1` to `ccflags-y` inside its
`ifdef KBUILD_EXTMOD` block when `CONFIG_KSU_DISABLE_POLICY` is `y`. It compiles the area
down to "manager-only root, always the default profile". `ksu_load_allow_list()` returns
immediately, so the table starts empty; `do_get_app_profile()` and `do_set_app_profile()`
return `-EOPNOTSUPP` before touching anything, so it can never be filled at runtime
either; `ksu_get_root_profile()` unconditionally returns `&default_root_profile`;
`profile_valid()` skips the `groups_count` and `selinux_domain` checks. Past its manager
and `WEBVIEW_ZYGOTE_UID` cases, which sit above the `#ifdef` and behave as they always do,
`ksu_uid_should_umount()` degenerates to `!__ksu_is_allow_uid(uid)`, so the global default
is not merely fixed but ignored - and because `SET_APP_PROFILE` is refused, the `"$"`/9999
entry cannot be changed at all, which is stronger than the Kconfig help text's "will
follow the global umount policy only". The reason to build this way is attack surface, not
size: with no writable profile store there is no ioctl that can install a custom SELinux
domain or capability set on a granted process, and no file under `/data` whose contents
affect who gets root. Anything added under the `#ifdef` must preserve that property.

## See also

- [`kernel/README.md`](../README.md) - build modes, init order and the layer map
- [`supercall/README.md`](../supercall/README.md) - the ioctl control plane above this
- [`manager/README.md`](../manager/README.md) - how `ksu_manager_appid` is established
- [`feature/README.md`](../feature/README.md) - the seven features this registry toggles
- [`selinux/README.md`](../selinux/README.md) - `u:r:ksu:s0` and live policy editing
- [`infra/README.md`](../infra/README.md) - `setup_mount_ns()` and the seccomp cache
- [`hook/README.md`](../hook/README.md) - tracepoint marking and the setuid hook
- [`uapi/README.md`](../../uapi/README.md) - the ABI shared with `ksud` and the manager
- [`docs/architecture.md`](../../docs/architecture.md) - end-to-end flows across layers
- [`userspace/ksud/README.md`](../../userspace/ksud/README.md) - feature persistence

<!-- reference links: kernel documentation and man pages -->
[bpf]: https://docs.kernel.org/bpf/index.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[execveat-2]: https://man7.org/linux/man-pages/man2/execveat.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[static-keys]: https://docs.kernel.org/staging/static-keys.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
[whatisrcu]: https://docs.kernel.org/RCU/whatisRCU.html
