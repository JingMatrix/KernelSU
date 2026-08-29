# Live SELinux policy editing and the ksu domain

KernelSU needs an [SELinux][selinux] domain that Android's policy has never heard of. That
policy is compiled on a build server and loaded by init before any of KernelSU's userspace
exists, and the only supported way to change it afterwards is a full reload from userspace,
which is loud and needs the policy binary. This directory takes the other route: it
serialises the policy already running inside the kernel, re-parses it into a private copy,
edits that copy's data structures directly, and swaps the result into `selinux_state.policy`
under [RCU][whatisrcu]. Everything KernelSU can do as root -- [`execve`][execve-2]ing
`/data/adb/ksud`, opening an app's tty, transitioning a shell into `u:r:ksu:s0` -- rests on
the rules built here.

## The SELinux pieces you need

Every process and every object on an SELinux system carries a *security context*, a
colon-separated string like `u:r:ksu:s0` (user, role, type, MLS level). Contexts are
interned: the kernel stores a 32-bit *SID* in each `struct cred` and each inode, and the
*sidtab* maps SIDs to contexts. Access checks compare SIDs, not strings, which is why
[`selinux.c`](selinux.c) caches four of them.

The *policydb* (`struct policydb`, `security/selinux/ss/policydb.h` in the kernel tree) is
the parsed form of the policy binary. It holds symbol tables for types, classes, roles and
users; a set of value-indexed arrays parallel to those tables; and the *avtab*, a hash
table of access-vector rules keyed by `(source_type, target_type, target_class,
specified)` where `specified` selects allow, auditallow, auditdeny or a type transition.
A *type* and an *attribute* are the same C struct; an attribute is a `type_datum` with
`attribute = 1`, and `type_attr_map_array[type - 1]` is the bitmap of attributes a type
belongs to. That indirection is what lets `allow domain ksu_file:file *` be one avtab node
keyed on the `domain` attribute instead of one node for every concrete domain on the device.

An `allow` rule grants a bitmask of permissions inside one class, and a class caps at 32
permissions. *Extended permissions* (xperms) add a second level for [`ioctl`][ioctl-2]: a
256-bit map selecting either whole ioctl drivers (the high byte of the command) or the
functions inside one driver. They only exist from policy version
`POLICYDB_VERSION_XPERMS_IOCTL`, which is why the xperm block in [`rules.c`](rules.c) is
behind a `db->policyvers` test. Decisions are then cached: the *AVC* memoises `(ssid, tsid,
tclass)` results. Because the policy pointer is swapped under RCU, a tuple the AVC has never
held consults the edited policy on its very next miss; a decision already resident in the
cache survives the swap unchanged. Without `reset_avc_cache()` an edit would stay invisible
to any caller the old policy had already denied once, so both publication sites call it.

Finally, Android is always enforcing in production. `setenforce 0` requires root and is
itself the thing apps look for. A domain that is merely `permissive` still logs every
denial it would have hit; a domain that is both permissive *and* allow-all logs nothing
and survives constraints the allow rules do not reach. `apply_kernelsu_rules()` does both.

## Layout

| File | What it owns |
| --- | --- |
| [`selinux.h`](selinux.h) | The context strings (`u:r:ksu:s0`, `u:object_r:ksu_file:s0`) and the small runtime API |
| [`selinux.c`](selinux.c) | Domain transitions on a `struct cred`, the SID cache, setenforce/getenforce |
| [`sepolicy.c`](sepolicy.c) | The policydb editor: duplicate, mutate symtabs/avtab/ebitmaps/constraints, destroy |
| [`sepolicy.h`](sepolicy.h) | The `ksu_allow` / `ksu_type` / `ksu_allowxperm` / ... verb set |
| [`rules.c`](rules.c) | The hardcoded ksu ruleset, the userspace batch decoder, the RCU swap |
| [`../../uapi/selinux.h`](../../uapi/selinux.h) | The nine batch command ids and their subcommands |
| [`../feature/selinux_hide.c`](../feature/selinux_hide.c) | The concealment feature, which consumes the pristine policy copy this area sets aside |

## Entering the domain

[`selinux.c`](selinux.c) never touches the policydb. It only writes SIDs into credential
blobs. `transive_to_domain()` resolves a context string with
`security_secctx_to_secid()` and stores the result in the cred's SELinux blob:

```c
tsec->sid = sid;
tsec->create_sid = 0;
tsec->keycreate_sid = 0;
tsec->sockcreate_sid = 0;
if (clear_exec_sid) {
    tsec->exec_sid = 0;
}
```

Zeroing `create_sid`, `keycreate_sid` and `sockcreate_sid` discards per-object labelling
overrides the calling app may have installed, so files and sockets the new root process
creates get labelled from the ksu domain rather than from the app's. `exec_sid` is a pending
transition for the next `execve`; it is cleared only for `escape_to_root_for_adb_root()`,
called from [`../feature/adb_root.c`](../feature/adb_root.c), because adbd is about to exec
and a surviving `exec_sid` would drop it straight back out of `u:r:ksu:s0`. A `su` process
keeps whatever `exec_sid` its App Profile arranged, so `setup_selinux()` passes `false`. The
blob type changed name in 6.18 (`struct task_security_struct` became `struct
cred_security_struct`) and `security_secid_to_secctx()` gained a `struct lsm_context`
argument in 6.14. The rename is absorbed by a local `#if` inside `transive_to_domain()` and
again inside `is_sid_match()`, each declaring its own `tsec` pointer; the 6.14 signature
change by a `struct lsm_context` compatibility struct and a pair of
`__security_secid_to_secctx` / `__security_release_secctx` wrappers that sit midway down
the file rather than at the top.

Not every domain change goes through this file. `su` grew a `-Z` / `--context` option that
writes the requested context to `/proc/thread-self/attr/current` in
[`../../userspace/ksud/src/su.rs`](../../userspace/ksud/src/su.rs), after any requested uid
and group change and immediately before `execve`. That is the kernel's own dynamic
transition path -- `selinux_setprocattr`, a `process:dyntransition` check and a bounds test
-- not `transive_to_domain()`, so the App Profile still decides the domain the process
starts in and `-Z` only moves it from there. The two meet again under selinux_hide, whose
`my_setprocattr` intercepts exactly that write for callers with uid >= 10000: an invocation
that has already dropped to an app uid cannot name a type which exists only in the edited
policy, because the hook resolves the context against the pristine copy and returns
`-EINVAL` before the real handler ever sees it.

`cache_sid()` resolves `u:r:ksu:s0`, `u:r:zygote:s0`, `u:r:init:s0` and
`u:object_r:ksu_file:s0` once: the first three into file-static `u32`s, the last into the
module-global `ksu_file_sid`. The predicates `is_ksu_domain()`, `is_zygote()` and
`is_init()` all go through `is_sid_match()`, which compares the cached value when it is
non-zero and otherwise translates the task's SID back to a string and `strncmp`s. Zero
deliberately means "not cached", covering both the window before `cache_sid()` runs and an
outright resolution failure, so the code degrades to correct-but-slow rather than to wrong.
That matters because these run on hot paths:
[`../hook/syscall_event_bridge.c`](../hook/syscall_event_bridge.c) calls `is_init()` from
`ksu_hook_execve_common()`, the body shared by the `execve` and [`execveat`][execveat-2]
entry points, [`../feature/kernel_umount.c`](../feature/kernel_umount.c) calls `is_zygote()`
from `ksu_handle_umount()` on every setuid that lands on an app uid, an isolated uid or the
webview zygote's 1053, and [`../hook/tp_marker.c`](../hook/tp_marker.c) calls
`is_task_ksu_domain()` while walking the whole task list. `ksu_file_sid` is the odd one out:
not a predicate but a value, and not file-static either, because two other files read it.
`ksu_install_file_wrapper()` in [`../infra/file_wrapper.c`](../infra/file_wrapper.c) stamps
it onto the anon inode it hands back, and `do_get_wrapper_fd()` in
[`../supercall/dispatch.c`](../supercall/dispatch.c) fails the ioctl with `-EINVAL` while it
is still zero, rather than mint a wrapper the SELinux checks would then reject.

`setenforce()` writes `selinux_state.enforcing` and stops there. The bare store is not the
shortcut it looks like -- `enforcing_set()` in `security/selinux/include/security.h` is a
one-line `WRITE_ONCE()` and would do exactly the same thing. What the function skips is
everything `sel_write_enforce()` in `security/selinux/selinuxfs.c` does *around* that store
when userspace writes to `/sys/fs/selinux/enforce`: an `AUDIT_MAC_STATUS` record reading
`enforcing=1 old_enforcing=0 auid=... lsm=selinux res=1`, then `avc_ss_reset()`,
`selnl_notify_setenforce()` and `selinux_status_update_setenforce()`, the last of which
bumps the mmap'd status page's sequence counter twice around the new value, seqlock style.
Any one of those is evidence that something outside init restored enforcing.

The only caller is [`../core/init.c`](../core/init.c), at the very end of the late-load
branch, after the allowlist, the throne tracker and the file wrapper are up and
`ksu_boot_completed` is set. Late loading normally requires the user to run `setenforce 0`
first, and leaving the device permissive afterwards is both a hole and trivially
detectable. The ordering is the interesting part: everything the late-load path does to
bring itself up runs while the device is still permissive, which is what lets
`escape_to_root_for_init()` put the loading process into the ksu domain in time to keep its
access to `/data/app` once enforcement resumes. The whole body of `setenforce()` sits
inside `#ifdef CONFIG_SECURITY_SELINUX_DEVELOP`; without that option it is a no-op.
`getenforce()` is not quite its mirror. It consults `selinux_state.disabled` first, under
`CONFIG_SECURITY_SELINUX_DISABLE`, and answers `false` when the old runtime-disable path has
been taken, because a disabled SELinux is not an enforcing one; only after that does it read
`selinux_state.enforcing`, or, on a kernel built without
`CONFIG_SECURITY_SELINUX_DEVELOP`, return a hardcoded `true`.

## Duplicating the running policy

`ksu_dup_sepolicy()` in [`sepolicy.c`](sepolicy.c) is the whole trick. It reads
`old_pol->policydb.len` -- the byte size of the policy image init originally wrote to
`/sys/fs/selinux/load` -- `vmalloc`s that much, calls the kernel's own `policydb_write()`
to serialise the live policy into the buffer, `kmemdup`s the `struct selinux_policy`
wrapper, zeroes its embedded policydb, rewinds the `struct policy_file` cursor, and calls
`policydb_read()` to build a completely independent parse.

Mutating the live policydb in place is not an option: it is read lock-free by every AVC
miss on every CPU, and inserting an avtab node or growing `type_val_to_struct` would race
with those readers. Reusing the kernel's reader and writer, rather than hand-writing a deep
copy, means the copy is structurally exactly what the kernel expects -- ebitmaps,
constraint expressions, the conditional avtab and the ocontext chains included -- and does
not rot when upstream changes a layout.

The `kmemdup` is shallow on purpose. `new_pol->sidtab` and `new_pol->map` alias the old
policy's, which is mandatory: SIDs already handed out to running processes must keep
resolving, so the new generation has to inherit the same SID space. That is why
`ksu_destroy_sepolicy()` is `policydb_destroy(&pol->policydb); kfree(pol);` and nothing
else, and must never free `->sidtab`.

Two details around the serialisation are easy to miss. First, `put_entry()` in
`security/selinux/ss/policydb.h` returns `-EINVAL` the moment the writer runs past
`fp->len`, so `db->len` is a hard budget: every mutation that makes the policy serialise
larger must grow it or the *next* duplication fails. `get_avtab_node()` charges each new
avtab node `sizeof(struct avtab_key) + sizeof(struct avtab_datum)` plus the xperms payload,
and `remove_avtab_node()` subtracts the mirror amount. The charge is deliberately generous
-- 16 bytes for an AV node that serialises to 12 -- and that slack silently covers the
growth this code does *not* account for: type names, ebitmap nodes, filename-transition
entries.

Second, `policydb_read()` sets `p->android_netlink_route` and `p->android_netlink_getneigh`
from bits 31 and 30 of the header config word, but `policydb_write()` only ever emits
`POLICYDB_CONFIG_MLS`, `REJECT_UNKNOWN` and `ALLOW_UNKNOWN`. A naive round trip therefore
clears two Android policy capabilities and changes how netlink route and neighbour messages
are classified for every domain on the device. `ksu_dup_sepolicy()` patches the serialised
bytes before re-reading them, at the offset the header arithmetic gives -- two u32s, the
eight-byte `"SE Linux"` string, then policyvers -- so `config` starts at byte 20
([`sepolicy.c#L932`](sepolicy.c#L932)).

That patch is now conditional. AOSP reverted its RTM_GETLINK extension for 6.18 in favour of
the upstream `nlmsg` extended permission, taking `android_netlink_route` and
`POLICYDB_CONFIG_ANDROID_NETLINK_ROUTE` out of the policydb with it, so there the fixup has
nothing left to repair and would not even compile. The whole block, the getneigh half
included, sits behind `#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)`; from 6.18 the
config word survives the round trip unaided, because the two bits it used to lose no longer
exist.

## Editing the copy

`add_rule()` looks up source type, target type, class and permission in the policydb's
symbol tables and hands concrete pointers to `add_rule_raw()`, which recurses to expand
wildcards. `ALL` is `#define ALL NULL` in [`rules.c`](rules.c), so a NULL argument means
"every entry of the corresponding symtab". One macro decides which entries:
`#define strip_av(effect, invert) ((effect == AVTAB_AUDITDENY) == !invert)`. When the
operation *adds* permissions, a NULL type expands only over entries with
`type->attribute` set: one rule on an attribute already covers every member type, and
expanding over concrete types would create thousands of redundant nodes. When it *removes*
permissions -- a plain `deny`, which is an inverted `AVTAB_ALLOWED` write -- the expansion
has to cover every concrete type, because the grant being stripped may sit on any attribute
the type belongs to. `dontaudit` inverts the logic again, because `auditdeny` is
`&=`-combined where `allowed` and `auditallow` are `|=`-combined. A NULL *class* gets no
such shortcut, since classes have no attributes to collapse into: the recursion walks every
entry of `db->p_classes`, so `ksu_allow(db, "domain", KERNEL_SU_FILE, ALL, ALL)` in
[`rules.c`](rules.c) costs one avtab node per class the device defines.

After every write, `is_redundant_avtab_node()` asks whether the node now carries no
information (zero `allowed`, all-ones `auditdeny`, NULL xperms) and `remove_avtab_node()`
unlinks it, freeing it by parking it in a one-slot throwaway `struct avtab` and calling
`avtab_destroy()` -- the nodes come from private slab caches the module cannot name.

`add_type()` is the only place allowed to add a type, and it grows five things in lockstep:
`db->p_types.nprim`, the symtab, `db->type_val_to_struct`, `db->type_attr_map_array` and
`db->sym_val_to_name[SYM_TYPES]`. Everything downstream refers to types by 1-based value
and `policydb_write()` emits `nprim`, so a disagreement produces an image the kernel cannot
re-read. It also sets the new type's bit in every role's `types` ebitmap, because
`policydb_context_isvalid()` checks that the role permits the type -- without it,
`security_secctx_to_secid("u:r:ksu:s0")` in `cache_sid()` would fail and nothing could enter
the domain. The three reallocations go through `ksu_kvrealloc`, a three-way macro covering
the 6.12 and 5.15 `kvrealloc` signatures plus a hand-rolled copy for the 5.10 Android
kernels that never got the backport.

Attribute membership needs a second pass. `add_typeattribute_raw()` sets one bit in
`type_attr_map_array`, then walks every class, every constraint node and every constraint
expression, setting `type->value - 1` in `e->names` wherever a `CEXPR_NAMES` expression
already mentions the attribute in `e->type_names->types`. Constraints (`mlsconstrain`,
`constrain`) are evaluated after the type-enforcement decision and can only subtract
permissions. They were flattened at policy-compile time: `type_names->types` keeps the
attribute set written in the source, while `names` holds the pre-expanded concrete set
consulted at runtime. Adding `ksu` to `mlstrustedsubject`'s bitmap alone would do nothing,
and every MLS constraint in Android's policy would still veto the domain despite
`allow ksu *:* *`, so this loop is what makes
`ksu_typeattribute(db, "ksu", "mlstrustedsubject")` mean anything. Two index conventions
coexist here and neither is a bug: `type_attr_map_array`, constraint `names` and role
`types` are indexed by `value - 1`, while `permissive_map` is indexed by the raw
`type->value`, which is what `set_type_state()` uses -- matching
`security/selinux/ss/services.c`, which reads `permissive_map` at `scontext->type`.

`add_xperm_rule()` parses `"%hx-%hx"` or `"%hx"`, defaulting to the full 0x0000-0xffff
range, then picks `AVTAB_XPERMS_IOCTLDRIVER` when the high bytes of low and high differ and
`AVTAB_XPERMS_IOCTLFUNCTION` otherwise; because xperms avtab entries are legitimately
non-unique, `get_avtab_node()` walks `avtab_search_node_next()` for a node whose `specified`
and `driver` both match before inserting a new one. `add_filename_trans()` builds the
compressed filename-transition layout Android uses -- one hashtable entry keyed by `(target
type, class, name)`, source types in an ebitmap, output types chained through `->next` --
using the four-argument `hashtab_insert()` and a verbatim copy of the kernel's file-static
`filenametr_hash` / `filenametr_cmp`, which is why this file will not build against anything
older than 5.9. `add_genfscon()`, by contrast, is `return false;`: `genfscon` statements are
parsed and shipped by ksud and then rejected here, logging `sepol: 9 failed.`.

## Building the ksu domain

`apply_kernelsu_rules()` in [`rules.c`](rules.c) takes `selinux_state.policy_mutex` and
makes *two* duplicates. The first becomes the global `backup_sepolicy` (declared in
[`../include/ksu.h`](../include/ksu.h)); it is given a freshly allocated sidtab and
`policydb_load_isids()`, so it is a standalone, queryable policy with its own SID space. It
is taken before any rule is added, is never edited afterwards, and leaves the global NULL
without aborting anything if any step of it fails.

The second duplicate gets the rules: `ksu` created as a `domain` and made permissive, given
`mlstrustedsubject`, `netdomain` and `bluetoothdomain`; `ksu_file` created as a `file_type`
with `mlstrustedobject` and a blanket `allow domain ksu_file`; `allow ksu * * *`; the four
`allowxperm` classes when the policy version supports them; `allow init ksu`; and a block
copied from Magisk's `suRights`/`allowLog` covering servicemanager, hwservicemanager, logd,
fd passing, fifos, unix stream sockets, memfd, binder and `system_server` killing su.
Publication is the standard RCU sequence -- `rcu_assign_pointer`, `synchronize_rcu`, destroy
the old generation -- followed by `reset_avc_cache()`.

Ordering matters twice. `apply_kernelsu_rules()` must run when init execs its second stage,
not earlier, because that is the first moment init has loaded the real Android policy;
[`../runtime/ksud_integration.c`](../runtime/ksud_integration.c) watches for that `execve`
and calls it there. And `cache_sid()` plus `setup_ksu_cred()` must follow it, because both
resolve `u:r:ksu:s0` against the just-patched policy. The late-load path in
[`../core/init.c`](../core/init.c) runs the same three calls back to back.

## The userspace payload

Userspace reaches this code through one ioctl. `KSU_IOCTL_SET_SEPOLICY` (`_IOC(_IOC_READ |
_IOC_WRITE, 'K', 4, 0)` in [`../../uapi/supercall.h`](../../uapi/supercall.h)) carries a
`{data_len, data}` pair; `do_set_sepolicy()` in
[`../supercall/dispatch.c`](../supercall/dispatch.c) forwards it to `handle_sepolicy()`. The
permission gate is `only_root` ([`../supercall/perm.c`](../supercall/perm.c)), literally
`current_uid().val == 0` -- not the manager, not the ksu domain.

`handle_sepolicy()` refuses a payload over 8 MiB with `-E2BIG`, copies the whole thing in
once with `kvmalloc` plus `copy_from_user`, then walks it with a two-pointer cursor. Each
command is an 8-byte `{u32 cmd, u32 subcmd}` header followed by exactly
`sepol_expected_argc(cmd)` arguments, each argument being `[u32 len][len bytes][NUL]`.
`sepol_read_string()` enforces `len < remaining`, rejects embedded NULs with `memchr`,
requires the trailing NUL, and maps `len == 0` to `ALL`.

| Command | `cmd` | Arguments |
| --- | --- | --- |
| `NORMAL_PERM` (allow / deny / auditallow / dontaudit) | 1 | 4 |
| `XPERM` (allowxperm / auditallowxperm / dontauditxperm) | 2 | 5 |
| `TYPE_STATE` (permissive / enforce) | 3 | 1 |
| `TYPE` | 4 | 2 |
| `TYPE_ATTR` | 5 | 2 |
| `ATTR` | 6 | 1 |
| `TYPE_TRANSITION` | 7 | 5 |
| `TYPE_CHANGE` (change / member) | 8 | 4 |
| `GENFSCON` | 9 | 3 |

That table is duplicated in `cmd_expected_argc()` in
[`../../userspace/ksud/src/sepolicy.rs`](../../userspace/ksud/src/sepolicy.rs) and in a
comment in [`../../uapi/supercall.h`](../../uapi/supercall.h); a mismatch desynchronises the
cursor and the whole batch is rejected.

The two error policies are deliberately opposite. A *decoding* error -- a truncated header,
a missing NUL, an unknown command id -- jumps to `out_drop_new_policy`, destroys the edited
copy and returns the negative errno, so a malformed or hostile buffer can never leave a
half-applied policy live. A *semantic* failure -- a type that does not exist on this device,
a class the vendor never defined -- is logged and the batch continues: one bad line in a
module's `sepolicy.rule` should not abort the other forty-nine. On success the ioctl returns
the count of commands that applied, which lets `apply_rules_batch()` in ksud report partial
application without a second call.

Batching matters because each ioctl costs a full serialise, re-parse and
`synchronize_rcu()`. ksud does not exploit that as well as it could: `load_sepolicy_rule()`
in [`../../userspace/ksud/src/module.rs`](../../userspace/ksud/src/module.rs) and
`apply_sepolies()` in
[`../../userspace/ksud/src/profile.rs`](../../userspace/ksud/src/profile.rs) each issue one
ioctl per file, so N module rule files cost N policy rebuilds at post-fs-data.

## Concealment: selinux_hide

[`../feature/selinux_hide.c`](../feature/selinux_hide.c) exists because the edits above are
visible. An app can write a context to `/sys/fs/selinux/context` and see whether the kernel
accepts `u:r:ksu:s0`, write an `scontext tcontext tclass` triple to `/sys/fs/selinux/access`
and read back a vector no stock device would produce, or write to `/proc/self/attr/current`
and learn the same thing. The feature answers those three questions from `backup_sepolicy`
rather than the live policy, for callers with uid >= 10000 only; shell (2000) and
system_server (1000) keep the real answers.

Answering from the pristine policy is not quite enough for `/sys/fs/selinux/access`, because
the reply carries more than a permission vector. `avd_init()` seeds `avd->seqno` from
`policy->latest_granting`, the running count of policy loads the kernel has committed, and
libselinux reads that number to decide when its own userspace AVC has gone stale. A stock
Android device loads the policy exactly once, so the counter reads 1. `ksu_dup_sepolicy()`
copies it verbatim into `backup_sepolicy` along with the rest of the `struct selinux_policy`
wrapper, so after a late load that followed a userspace `load_policy` the backup would
truthfully report 2 or more and hand the caller a number no stock boot produces.
`my_write_access()` therefore overwrites `avd.seqno` with 1 once the decision is computed
and before `scnprintf` formats the six fields.

Three of the four hooks are rodata function-pointer patches. `find_kernel_symbol_exact()`
from [`../infra/symbol_resolver.c`](../infra/symbol_resolver.c) resolves the file-static
`write_op[]` table in `security/selinux/selinuxfs.c`, and the module's own verbatim copy of
`enum sel_inos` indexes `SEL_CONTEXT` and `SEL_ACCESS`. The slots are overwritten with
`ksu_patch_text()`, which [`../Kbuild`](../Kbuild) resolves per architecture to
[`../hook/arm64/patch_memory.c`](../hook/arm64/patch_memory.c) or
[`../hook/x86_64/patch_memory.c`](../hook/x86_64/patch_memory.c) -- the x86_64 half now
matters, because the module ships for that target too. Both walk `init_mm`'s page tables for
the slot's physical address, map that page through a fixmap entry -- `FIX_TEXT_POKE0` on
arm64, `FIX_BTMAP_BEGIN` on x86_64, where modern kernels no longer define the poke slot --
and do the store from inside `stop_machine()`. A plain store would fault on any kernel with
`CONFIG_STRICT_KERNEL_RWX`, and flipping the PTE writable is exactly what vendor hypervisor
monitors watch for. The third patch of the same kind belongs to the status page, below. Only
`/proc/self/attr/current` is intercepted differently:
[`../hook/lsm_hook.c`](../hook/lsm_hook.c) replaces the [LSM][lsm] slot that currently
points at `selinux_setprocattr`, and `my_setprocattr` is `__nocfi` because it tail-calls the
saved original through a pointer cast whose CFI type id no longer matches.

The mmap'd status page is the awkward one. libselinux maps `/sys/fs/selinux/status` and
treats `sequence` as a seqlock over `enforcing` and `policyload`, so it never has to
syscall. Three things would betray KernelSU there: `reset_avc_cache()` calls
`selinux_status_update_policyload(0)`, which resets `policyload` to zero and bumps
`sequence` by two, where a booted device should show a non-zero policyload; a late load
usually happens while the device is permissive, so the page says `enforcing = 0`; and an
mmap'd page cannot be filtered per-reader after the fact. `initialize_fake_status()`
snapshots the live page into a private one under `selinux_state.status_lock`. On anything
other than a late load it declines to copy a page that still reads `enforcing = 0`, since
handing apps that value advertises the state the feature exists to hide; a device booted
with `androidboot.selinux=permissive` therefore never gets a fake page at all, and the retry
described below runs to its deadline and logs the failure. When the capture does happen, the
snapshot is only trustworthy on a normal boot. A late load may well follow a userspace
`load_policy` -- the usual shape of a permissive-then-reload exploit -- and every such
reload has already incremented `policyload` and added two to `sequence`, so there is no
sensible way to repair what was captured. The late-load branch discards it and writes the
pair a stock device ends boot with instead, which depends on when the kernel creates the
page ([`../feature/selinux_hide.c#L279`](../feature/selinux_hide.c#L279)). Below 6.10
`selinux_kernel_status_page()` allocates it on the first open of the file, long after init
loaded the policy and went enforcing, and stamps it with the values it was born with, so a
stock reader sees `sequence = 0, policyload = 0`. From 6.10 `init_sel_fs()` pre-allocates
the page precisely so that the initial policy load can be recorded, and that load
(`policyload = 1`, `sequence` 1 then 2) followed by init's single `setenforce` (`sequence` 3
then 4) leaves stock at `sequence = 4, policyload = 1`. `enforcing` is forced to 1 only when
the capture found 0, which is the permissive late load the exercise exists for.
`hook_selinux_status_open()` resolves `sel_handle_status_ops` the same way the `write_op[]`
patches resolve their table, and patches its `open` slot with the same `ksu_patch_text()`
call; `sel_handle_status_ops` is a `const struct file_operations`, so it is rodata too.
`my_sel_open_handle_status()` then hands app callers `filp->private_data = fake_status`.

Unlike the other three, that slot is patched from `ksu_selinux_hide_init()` at module load
rather than when the feature is switched on, and it has to be. The snapshot must happen
*before* `apply_kernelsu_rules()` rewrites the real page, which is why
`ksu_selinux_hide_handle_second_stage()` is called one line earlier in
[`../runtime/ksud_integration.c`](../runtime/ksud_integration.c) -- but on a normal boot
below 6.10 the kernel has not allocated `selinux_state.status_page` yet, because
`selinux_kernel_status_page()` creates it on the first open of the file and nothing has
opened it. So the hook goes in early, a [static key][static-keys] arms the retry, and every
subsequent open re-attempts the snapshot through `my_sel_open_handle_status()` until
`ksu_selinux_hide_handle_post_fs_data()` disables the key and logs the failure. The 6.10
pre-allocation makes the second-stage attempt succeed outright, and the key is disabled
there and then; the retry stays because the same module still has to boot on 5.10 through
6.6. Turning the feature off runs `ksu_selinux_hide_unhook()`, which restores this slot
along with the others, so a later enable calls `hook_selinux_status_open()` again to put
it back.

Below 6.6 the query API still took an explicit `struct selinux_state *`, so the file builds
a `fake_state` whose `policy` is `backup_sepolicy` and points the kernel's own
`security_context_to_sid()`, `security_context_str_to_sid()`, `security_sid_to_context()`
and `security_compute_av_user()` at it. None of those is an exported symbol either -- they
reach the module the same way everything else in this directory does, through the load-time
rewrite described in the last section -- but on those kernels they at least take the state
to query as an argument.

From 6.6 that parameter is gone and those functions read the global policy, which is the
one policy selinux_hide must not answer from. The back half of the file therefore carries
ten private copies: `string_to_context_struct()`, `security_context_to_sid_with_policy()`,
`context_struct_to_string()`, `sidtab_entry_to_string()`,
`security_sid_to_context_with_policy()`, `avd_init()`, `type_attribute_bounds_av()`,
`constraint_expr_eval()`, `context_struct_compute_av()` and
`security_compute_av_user_with_policy()`. Each takes an explicit policy pointer, and each
has the upstream RCU locking and `selinux_initialized()` guards stripped, with a
`// removed:` comment left where they stood. Both guards are about the *global* policy: the
`rcu_read_lock()` in the originals exists to hold `rcu_dereference(selinux_state.policy)`
alive, and these copies never dereference it, while `selinux_initialized()` asks whether
the global state has a policy at all, which says nothing about `backup_sepolicy`. Three of
the ten are entry points that `my_write_context()`, `my_write_access()` and
`my_setprocattr()` call directly; the rest exist only because those three need them. One
copy is skipped when the resolver can find the original: `ksu_selinux_hide_enable()` looks
up `context_struct_compute_av`, and `security_compute_av_user_with_policy()` calls that in
preference to the private version -- the nested call inside `type_attribute_bounds_av()`
still goes to the copy. `security_dump_masked_av` has no copy at all, because
`type_attribute_bounds_av()` only logs through it, so that call happens only when the
resolver finds the symbol. This is the most upstream-sensitive code in the fork: it mirrors
internal layouts that upstream is free to change.

`backup_sepolicy` is a multi-megabyte permanent allocation, so `on_boot_completed()` in
[`../runtime/boot_event.c`](../runtime/boot_event.c) calls
`ksu_selinux_hide_drop_backup_if_unused()` to free it when the feature is not running. ksud
replays the persisted toggle at post-fs-data
([`../../userspace/ksud/src/feature.rs`](../../userspace/ksud/src/feature.rs)), which wins
that race; enabling it later returns `-EAGAIN` and the manager asks the user to reboot.
Enabling is all-or-nothing -- any failure jumps to `unhook`, which restores every slot --
because a half-installed set of hooks giving inconsistent answers is more detectable than
no hiding at all.

### What it does not conceal

`write_op[]` also has entries for `SEL_CREATE`, `SEL_RELABEL`, `SEL_USER` and `SEL_MEMBER`,
and none of them is intercepted. A caller whose domain is permitted to use
`/sys/fs/selinux/create` or `/sys/fs/selinux/user` gets answers computed against the live,
modified policy. `/sys/fs/selinux/policy` would dump the whole edited image, though reading
it requires `security:read_policy`, which app domains normally lack.
`/sys/fs/selinux/validatetrans` sits outside `write_op[]` altogether: `sel_transition_ops`
names `sel_write_validatetrans` as its own `write` method rather than routing through the
transaction table, so no slot patch in this feature can reach it. The node is mode 0222 and
resolves all three of its contexts against the live policy, which tells any domain holding
`security:validatetrans` that `u:r:ksu:s0` parses.

The uid gate is `>= 10000`, so anything running as shell, system or root sees the
unmodified behaviour by design -- which also means an app that can reach a shell reads the
truth. And `setenforce()`'s deliberate silence cuts both ways: it does not update the
status page, so `/sys/fs/selinux/enforce` and the mmap'd page can disagree until something
else bumps the sequence.

## Why this is the most version-fragile code in the tree

Nothing here goes through an exported symbol: `policydb_write`, `policydb_read`,
`policydb_destroy`, `policydb_load_isids`, `policydb_filenametr_search`,
`avtab_insert_nonunique`, `avtab_destroy`, `symtab_insert`, `sidtab_context_to_sid`,
`cond_compute_av`, `avc_ss_reset`, `selinux_status_update_policyload`, the pre-6.6
`security_context_to_sid` and `security_compute_av_user` that selinux_hide calls, and the
`selinux_state` global itself are all internal. [`../Kbuild`](../Kbuild) adds
`-I$(srctree)/security/selinux` and `-I$(srctree)/security/selinux/include` so the module
can include `ss/policydb.h`, `objsec.h` and `avc.h`, which means every struct layout in this
directory is taken verbatim from the kernel being built against -- and `struct policydb`,
`struct selinux_policy` and `struct selinux_state` all carry `__randomize_layout`.

Two things keep that honest. [`../Makefile`](../Makefile) runs
[`../tools/check_symbol.c`](../tools/check_symbol.c) after every build; it requires every
[`SHN_UNDEF`][elf-5] symbol in `kernelsu.ko` to resolve to a defined symbol in the target
`vmlinux` and the `__versions` section to be zero-sized, so a call to a SELinux internal
that is absent from a target kernel breaks the build, not the boot. At load time,
[`../../userspace/ksuinit/src/lib.rs`](../../userspace/ksuinit/src/lib.rs) rewrites each of
those undefined symbols to `SHN_ABS` with an address harvested from `/proc/kallsyms` before
calling [`init_module`][init-module-2], which is the only reason an out-of-tree module can
reference them at all. The `.ko` is consequently locked to the kernel it was built against.
`load_module()` softens that lock in one direction only: when the first `init_module` fails,
it reads the new records out of `/dev/kmsg`, and if the kernel complained about a version
magic mismatch it rewrites the module's vermagic string to the one demanded and tries again.
What that defeats is the kernel's own version check, not the coupling this section is about
-- the struct layouts baked into the object are still the build kernel's, so a forced load
onto a kernel that really did move a `policydb` field trades a clean refusal for corruption
inside the code described here.

A handful of rough edges are worth knowing before you touch anything. The `db->len`
accounting covers only avtab insert and remove: `add_type()`, `add_typeattribute_raw()`,
`set_type_state()` and `add_filename_trans()` all grow the serialised image without bumping
it. It works today because `apply_kernelsu_rules()` inserts a very large number of
over-charged avtab nodes before anything else needs the margin; a batch that adds many types
and few rules erodes that. `add_type()` also has no failure unwinding: if `symtab_insert()`
or any of the three reallocations fails it returns false having already incremented
`p_types.nprim`, leaving the value-indexed arrays shorter than the counter.

`my_write_context()` performs the `len > SIMPLE_TRANSACTION_LIMIT` bound check only on the
6.6-and-later branch. Upstream `sel_write_context()` does it unconditionally, so on a
5.10/5.15/6.1 GKI kernel with selinux_hide enabled, an app whose canonical context is longer
than the limit writes past the transaction buffer.

In `add_xperm_rule_raw()`, the `if (datum->u.xperms == NULL)` branch is unreachable --
`avtab_insert_node()` deep-copies the xperms into its own slab cache -- so a second
`allowxperm` against an existing `(specified, driver)` node computes its bits into a local
struct and drops them rather than merging. `backup_sepolicy->map` is a shallow copy that is
never duplicated; nothing in selinux_hide reads it, so that is latent rather than live, but
a policy reload from userspace after boot would free the array out from under it. And
`KSU_IOCTL_SET_SEPOLICY` is gated on uid 0 alone: any root process on the device, not just
ksud or the manager, can make any type permissive.

## See also

- [`../README.md`](../README.md) -- the kernel module: build modes, init order, layer map
- [`../core/README.md`](../core/README.md) -- where the three bring-up calls sit in
  `kernelsu_init()`
- [`../runtime/README.md`](../runtime/README.md) -- the boot pipeline that triggers them on
  a normal boot
- [`../supercall/README.md`](../supercall/README.md) -- the ioctl control plane and its
  permission predicates
- [`../hook/README.md`](../hook/README.md) -- `ksu_patch_text()` and LSM slot patching
- [`../infra/README.md`](../infra/README.md) -- the symbol resolver and the `ksu_file_sid`
  consumer
- [`../feature/README.md`](../feature/README.md) -- selinux_hide among the toggleable
  features
- [`../policy/README.md`](../policy/README.md) -- App Profiles, which carry the target
  domain string
- [`../../uapi/README.md`](../../uapi/README.md) -- the ABI contract, including the
  sepolicy batch format
- [`../../userspace/ksud/README.md`](../../userspace/ksud/README.md) -- the statement
  parser and batch serialiser
- [`../../docs/architecture.md`](../../docs/architecture.md) -- repository-wide hub

<!-- reference links: kernel documentation and man pages -->
[elf-5]: https://man7.org/linux/man-pages/man5/elf.5.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[execveat-2]: https://man7.org/linux/man-pages/man2/execveat.2.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
[ioctl-2]: https://man7.org/linux/man-pages/man2/ioctl.2.html
[lsm]: https://docs.kernel.org/security/lsm.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[static-keys]: https://docs.kernel.org/staging/static-keys.html
[whatisrcu]: https://docs.kernel.org/RCU/whatisRCU.html
