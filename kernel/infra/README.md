# Shared kernel plumbing

`kernel/infra/` holds five services the rest of the module is built on, each a mechanism
rather than a policy: a runtime kernel-symbol resolver, a file-descriptor proxy, a record
queue, a [seccomp][seccomp-filter] bitmap patcher and a mount-namespace switcher. All five
objects are listed unconditionally in [`Kbuild`](../Kbuild); none sits behind a
`CONFIG_KSU_*` symbol or registers a feature handler, so nothing here can be switched off at
run time. [`symbol_resolver.c`](symbol_resolver.c) is the foundation - its whole interface,
[`symbol_resolver.h`](symbol_resolver.h), is three functions and no struct - and is called
from `hook/`, `feature/`, `supercall/` and `policy/`, every subsystem that patches, probes
or reads kernel text; the other four are leaves, each serving a single subsystem.
[`core/init.c`](../core/init.c) calls `ksu_init_symbol_resolver()` ahead of every other
subsystem in `kernelsu_init()`, and `ksu_file_wrapper_init()` late.

## symbol_resolver.c

### Why the address has to come from kallsyms

An out-of-tree module can only bind a symbol the kernel published with `EXPORT_SYMBOL`;
[`init_module(2)`][init-module-2] rejects anything else with "Unknown symbol". Almost
nothing this module needs is exported. `kallsyms_lookup_name()` itself lost its export in
Linux 5.7, and neither it nor `path_mount`, `ksys_unshare`, `__arm64_sys_setns` or the
[SELinux][selinux] internals [`selinux/sepolicy.c`](../selinux/sepolicy.c) calls carry one
in the kernel tree this fork builds against. For an address the module loader will not
supply, kallsyms - the kernel's own name-to-address table, which lists every text symbol
including `static` functions and, under `CONFIG_KALLSYMS_ALL=y` as GKI's defconfig sets, the
data symbols too (`sys_call_table`, `security_hook_heads`, `proc_mounts_operations`,
`totalcma_pages`) - is the only source there is.

The fork reads that table at two different times, and the split is what to understand before
adding a kernel call. Before load, [`ksuinit`](../../userspace/ksuinit/src/lib.rs) parses
`kernelsu.ko`, collects every [`SHN_UNDEF`][elf-5] symbol from `.symtab`, streams
`/proc/kallsyms` with `/proc/sys/kernel/kptr_restrict` pinned to 1 - the value at which a
`CAP_SYSLOG` reader sees real addresses rather than zeros, with a guard object that restores
the previous setting when it drops - and rewrites each matching `Elf64_Sym` to `SHN_ABS`
with the resolved `st_value` before calling [`init_module`][init-module-2]. That is why
[`su_mount_ns.c`](su_mount_ns.c) can declare `extern int path_mount(...)` and just call it.
[`Makefile`](../Makefile) guards the scheme with
[`tools/check_symbol.c`](../tools/check_symbol.c), which errors out on any `SHN_UNDEF`
symbol the target `vmlinux` does not define, and on a `__versions` section of non-zero size.
An empty `__versions` silences the loader's per-symbol CRC check but not its version-magic
check, and a `vermagic=` string the running kernel disagrees with sinks the load on its own.
`ksuinit` therefore holds `/dev/kmsg` open across `init_module`; when the failure is that
mismatch it rewrites `.modinfo` with the string the kernel named in its complaint and calls
`init_module` again on the same buffer, so the `SHN_ABS` addresses written in the first pass
carry into the retry.

That route only works for a symbol you can promise exists everywhere you ship. One present
on this kernel and absent on the next is a hard build failure there, and `ksuinit` would
only log `Cannot find symbol` and load a module with a zeroed relocation - a NULL call on
first use. So anything version- or config-dependent has to be a pointer looked up after
load and NULL-checked at every use: `task_call_func`, `perf_event_output_forward`,
`valid_user_regs`, `uprobe_unregister_nosync`, `system_cpucaps`, `totalcma_pages`. A
second class needs runtime lookup because the wanted address is not the one the linker
would hand over at all, as the CFI section explains. A `CONFIG_KSU=y` build resolves the
direct externs at link time but still needs the optional set by name, so one mechanism
covers both build modes.

### find_kernel_symbol_exact: two lookups, one rejection

From 6.1, if the runtime-resolved `kallsyms_on_each_match_symbol_fn` is available, it
drives that helper, which binary-searches the name-sorted index and invokes the callback
once per exact-name hit; `find_kernel_symbol_exact_cb()` stores the address and returns 0,
so iteration continues and the last match wins. That helper walks vmlinux symbols only.
The fallback, `kallsyms_lookup_name()`, is not so restrained: when a name is absent from
vmlinux it falls through to `module_kallsyms_lookup_name()`. An address inside another
loaded module would become a use-after-free the moment that module unloads, so the result
is checked:

```c
    addr = kallsyms_lookup_name(symbol_name);
    // check if it is kernel symbol
    kallsyms_lookup(addr, NULL, NULL, &module_name, buf);
    if (unlikely(module_name)) {
        pr_warn("ignore symbol %s of module %s\n", symbol_name, module_name);
        return 0;
    }
```

The function is `__nocfi` because the fast path calls through a pointer harvested from
kallsyms. Control-Flow Integrity checks that an indirect call target is a legal member of
the callee's type set, and a raw kallsyms address has no such membership recorded in this
module, so the call would otherwise trap.

`ksu_init_symbol_resolver()` fills those helper pointers in. It is `__init` and runs first
among the subsystem initialisers in `kernelsu_init()`, ahead of `ksu_syscall_hook_init()`.
Which pointer it fills depends on the target: only `kallsyms_on_each_symbol` below 5.19,
only `kallsyms_on_each_match_symbol` from 6.1, and neither in between, where the body
compiles away to nothing. Whichever it is, the resolution goes through
`kallsyms_lookup_name()`: on 6.1 the accelerated path is the very pointer being resolved
and is still NULL at that instant, and below 5.19 no accelerated path is compiled in.
Move a resolver-dependent init above this one and every later lookup loses the fast path;
below 5.19 the variant walk of the next section then has no iterator at all and returns
NULL outright, the gap the standing `// TODO: iterate kallsyms by sprint_symbol` marks.

### Function tables, CFI and the .cfi_jt alias

`ksu_resolve_symbol_for_functable_hook()` serves a narrow case: the caller is not after
something to call, but after the exact value the kernel itself put in a function table, or
after the table symbol, so it can recognise or replace an entry. Three sites need that -
[`hook/lsm_hook.c`](../hook/lsm_hook.c) resolving the [LSM][lsm] handler it is about to
displace, and [`hook/arm64/syscall_hook.c`](../hook/arm64/syscall_hook.c) plus
[`hook/x86_64/syscall_hook.c`](../hook/x86_64/syscall_hook.c) resolving `sys_call_table` and
`__arm64_sys_ni_syscall` / `__x64_sys_ni_syscall`.

Clang's older CFI implementation (`-fsanitize=cfi`, everything before 6.1 here) routes
every address-taken function through a jump-table thunk named `<sym>.cfi_jt`, and it is
the thunk address that lands in the table. Resolve `__arm64_sys_ni_syscall` the ordinary
way on such a kernel and the `ksu_syscall_table[i] == ni_syscall` scan in
`ksu_find_ni_syscall_slots()` matches nothing, the dispatcher never gets a free slot,
`ksu_syscall_hook_init()` bails after logging `failed to find ni_syscall slot for
dispatcher`, and every syscall hook above it stays uninstalled. So below 6.1 the function
tries `"<sym>.cfi_jt"` first, then `resolve_symbol_variant()`, then the plain name; 6.1
switched to `-fsanitize=kcfi`, which has no jump tables, and the order inverts.

`resolve_symbol_variant()` walks every vmlinux symbol via `kallsyms_on_each_symbol`,
accepting an exact match or `symbol_name` followed by `.` or `$` - the shapes LTO produces
(`foo.llvm.1234`, `foo.constprop.0`). That middle step is not a formality:
`sys_call_table` is data, never address-taken code, so there is no `sys_call_table.cfi_jt`
to find and the pre-6.1 lookup lands on the variant walk, whose callback accepts the bare
name and returns before the plain-name lookup is ever reached. That last step only runs
when the walk finds nothing - below 5.19 on a kernel whose `kallsyms_on_each_symbol` never
resolved, leaving `resolve_symbol_variant()` with no iterator to drive. Do not substitute
one resolver for the other: a `.cfi_jt` thunk is the right value to compare against a
table and the wrong value to call.

Two other sites resolve a function table without going through that helper at all, and the
difference is worth naming. `hook_selinux_status_open()` in
[`feature/selinux_hide.c`](../feature/selinux_hide.c) resolves `sel_handle_status_ops`, and
`ksu_mount_hide_enable()` in [`feature/mount_hide.c`](../feature/mount_hide.c) resolves
`proc_mounts_operations`, `proc_mountinfo_operations` and `proc_mountstats_operations`;
each then overwrites the `.open` slot in place with `ksu_patch_text()`, keeping the
displaced pointer so it can call through to the original and put it back on unhook. What
those callers want is the address of the `struct file_operations` object itself, so
`find_kernel_symbol_exact()` is the whole requirement: a data symbol has no `.cfi_jt` alias
for the helper to prefer, and the name resolves without one. The helper's extra work pays
off in the other two cases - a value that has to match a pointer the kernel already
installed, or a symbol LTO may have renamed out from under an exact lookup.

### Resolving a symbol in order to read it

A third kind of caller wants neither an address to jump to nor a table entry to compare
against, but the function's instruction bytes. `ksu_app_profile_init()` in
[`policy/app_profile.c`](../policy/app_profile.c) resolves `seccomp_filter_release` and
`_raw_spin_lock_irq`, asks `kallsyms_lookup_size_offset()` how many bytes the first one
spans, and hands that range plus the second address to `scan_call_to()` from
[`hook/patch_memory.h`](../hook/patch_memory.h), which walks the function for a `BL` whose
sign-extended imm26 lands on the target. It is a disassembler with one opcode, and it
exists because a version number cannot answer the question being asked. AOSP backported
the rework that makes `seccomp_filter_release()` acquire `siglock` into some 6.6 kernels
and not others - Pixel 10 is the counterexample the source comment names - so
`LINUX_VERSION_CODE` reads the same on both and only the text distinguishes them. The
whole body sits behind `NEED_BACKPORT_COMPAT`, the 6.6-to-6.11 window where that ambiguity
lives; outside it the two lookups never happen.

The answer selects the shape of the throwaway `task_struct` that `disable_seccomp()` hands
to the release path: `PF_EXITING` when the call is present, a NULL `sighand` when it is
not. Each version asserts on the way in the field the other leaves alone: a kernel without
the rework opens with `WARN_ON(tsk->sighand != NULL)`, treating a severed `sighand` as the
caller's stand-in for holding `siglock`, while the reworked one takes `siglock` for real
and wants `PF_EXITING`. Guess wrong and the release path trips its own check. Two
resolver properties earn their keep here. The address has to be the function's real entry,
which is why the site calls `find_kernel_symbol_exact()` rather than
`ksu_resolve_symbol_for_functable_hook()`: a `.cfi_jt` thunk is a `B` to the function and
holds no `BL` to find. And a zero size is survivable, since the code falls back to
scanning 128 bytes. On x86_64 the question is moot by construction -
[`hook/x86_64/patch_memory.c`](../hook/x86_64/patch_memory.c) defines `scan_call_to()` as a
stub returning NULL - so that build always takes the NULL-`sighand` branch. That branch is
no longer hypothetical: [`build-all-x64.sh`](../build-all-x64.sh) turns out an x86_64
`kernelsu.ko` for every KMI beside the arm64 one, and CI ships both.
`ksu_app_profile_init()` runs in `kernelsu_init()` after `ksu_supercalls_init()`, far
enough downstream that the resolver is long since filled in.

### A register primitive is useless without its unregister

Every consumer that resolves a registration primitive must resolve the matching
unregistration primitive, and must refuse to arm anything when either is missing. A
[uprobe][uprobetracer] consumer, a [kprobe][kprobes], a [kretprobe][kprobes] and a perf
breakpoint all install a pointer into module text; once `rmmod` frees that text, anything
still armed is a jump into freed memory the next time it fires, and without the unregister
symbol there is no way to retire it. Half a resolution is worse than no feature.

`ksu_uhook_init()` in [`feature/uhook.c`](../feature/uhook.c) applies the rule
feature-wide, computing `uhook_ready` from `p_uprobe_register` plus either
`p_uprobe_unregister_nosync` and `p_uprobe_unregister_sync` or `p_uprobe_unregister`, and
clearing it again if the record ring fails to allocate; `uh_add()` refuses with `-ENOSYS`
while it is false, and with nothing armable the other `KSU_UHOOK_*` verbs have nothing to
find. `hwbp_set()` in [`feature/ptctl.c`](../feature/ptctl.c) applies it per operation,
with `if (!p_reg_hwbp || !p_unreg_hwbp) return -ENOSYS;`, and
`resolve_kretprobe_symbols()` in [`feature/mem_spoof.c`](../feature/mem_spoof.c) resolves
`register_kretprobe` and `unregister_kretprobe` together.
[`feature/mount_hide.c`](../feature/mount_hide.c) takes the same shape for a different
reason: all three `show_*` functions and all three `proc_*_operations` must resolve or the
feature returns `-ENOSYS`, because a partial hook makes `/proc/pid/mounts` and
`/proc/pid/mountinfo` disagree, a differential no stock device exhibits. The ptctl and
uhook symbol sets, and what degrades when one of them fails to resolve, are covered in
[`docs/instrumentation.md`](../../docs/instrumentation.md).

## file_wrapper.c

A terminal app's pty is labelled with that app's SELinux type, and SELinux normally waves
an already-open fd through: `selinux_file_permission()` short-circuits while the calling
task's SID still equals the one stamped on the `struct file` at open time. Once `su`
switches the caller into the domain its profile names - `u:r:ksu:s0` by default, from
`KSU_DEFAULT_SELINUX_DOMAIN` in [`policy/allowlist.c`](../policy/allowlist.c), but any
string the profile carries, or any string `-Z`/`--context` names on the command line -
those SIDs differ and every read and write on the inherited fds 0/1/2 is revalidated
against the new domain, so a domain without access to the app's pty type leaves the shell
deaf and mute. Re-opening the tty by path is not an option: the path and the session are
wrong. [`file_wrapper.c`](file_wrapper.c) hands over a different `struct file` instead: an
anonymous inode - a `struct file` with no name on any filesystem - called
`[ksu_fdwrapper]`, carrying the `ksu_file` label, whose every `file_operations` slot
re-issues the operation on the original file, whose own open-time check already passed in
the app's own domain.

`ksu_install_file_wrapper()`, one of the two entry points in
[`file_wrapper.h`](file_wrapper.h), is reached only from `do_get_wrapper_fd()` in
[`supercall/dispatch.c`](../supercall/dispatch.c), behind `KSU_IOCTL_GET_WRAPPER_FD` with
`perm_check = manager_or_root`, and that handler refuses with `-EINVAL` while
`ksu_file_sid` is 0. The SID is cached by `cache_sid()` in
[`selinux/selinux.c`](../selinux/selinux.c) and is only useful because
`apply_kernelsu_rules()` in [`selinux/rules.c`](../selinux/rules.c) created the type and
installed `ksu_allow(db, "domain", KERNEL_SU_FILE, ALL, ALL)` in the live policy database;
without the relabel the wrapper would carry the creating task's SID and defeat its own
purpose. The userspace caller is `wrap_tty()` in
[`ksud/src/su.rs`](../../userspace/ksud/src/su.rs), applied to fds 0, 1 and 2 unless
`-W`/`--no-wrapper` was passed and skipped for any of the three that `isatty()` reports is
not a terminal. It runs in the `su` process itself, before `exec` and before the `-Z`
write to `/proc/thread-self/attr/current`, so a caller that names a context of its own
still inherits descriptors the `domain` rule above already covers.

Three pieces of surgery follow, after the anon inode is created and before the file is
published. `i_mode` is copied from the original inode, because libc's stdio picks its
buffering mode from `fstat()`. `selinux_inode(wrapper_inode)->sid` is set to
`ksu_file_sid`. `i_fop` is pointed at `ksu_file_wrapper_inode_fops`, which carries nothing
but `.owner` and `ksu_wrapper_open()`, so re-opening the wrapper through `/proc/<pid>/fd/N`
yields another working wrapper - and since that path calls `dentry_open(orig_path, ...,
current_cred())`, the re-opener's own credentials are checked against the original file.
The dentry separately gets `d_fsdata` holding a copy of the original `struct path` and a
`d_dname` that renders it, because `readlink("/proc/self/fd/0")` returning
`anon_inode:[ksu_fdwrapper]` breaks anything that reattaches to its tty by path. Slots are
populated conditionally (`fp->f_op->read ? ksu_wrapper_read : NULL`, and so on for roughly
thirty entries): an unconditional stub would advertise a capability the original file
lacks and then dereference a NULL `orig->f_op->X`.

Teardown is the subtle part. The wrapper's `file_operations` lives inside the heap
allocation and its `.owner` is `THIS_MODULE`, so creation takes a module reference. But
`__fput()` in `fs/file_table.c` calls `->release()` first and `fops_put(f_op)` afterwards,
and `->release()` is what frees the memory `f_op` points into:

```c
static int ksu_wrapper_release(struct inode *inode, struct file *filp)
{
    // ... (source link to fs/file_table.c)
    // f_op->release is called before fops_put(f_op), so we put it manually.
    fops_put(filp->f_op);
    // prevent it from being put again
    filp->f_op = NULL;
    ksu_release_file_wrapper(filp->private_data);
    return 0;
}
```

Dropping the reference early and NULLing the pointer turns `__fput()`'s own `fops_put`
into a no-op, since the macro skips a NULL argument. One consequence is that an open root
shell pins the module: `rmmod` fails while any wrapper fd is alive on the device. The
other entry point, `ksu_file_wrapper_init()`, is a no-op from 5.16 on; below that there is
no `anon_inode_getfile_secure`, so the file reimplements it and the init borrows the
kernel's private `anon_inode_mnt` from a throwaway `anon_inode_getfile()` file.

## event_queue.c

[`event_queue.c`](event_queue.c) is a bounded, sequence-numbered record queue with many
producers, one reader, and drop accounting that is itself delivered as a record. Its only
producer is [`sulog/event.c`](../sulog/event.c), which owns one static queue of 256
records of at most 2048 bytes; its only consumer is [`sulog/fd.c`](../sulog/fd.c), which
wraps it in the `[ksu_sulog]` anon inode that `KSU_IOCTL_GET_SULOG_FD` (`only_root`) hands
out one at a time.

Two locks do different jobs. An irqsave spinlock protects the list and every counter; a
mutex, `read_lock`, serialises readers. `ksu_event_queue_push()` allocates the node and
copies the payload before taking the spinlock, because its callers pass `GFP_KERNEL` and
may sleep. Inside the lock it burns a sequence number unconditionally, so a dropped event
still consumes one and the reader can name the exact range it lost; then it either links
the node or calls `ksu_event_queue_note_drop_locked()` and returns `-ENOSPC` or `-ENOMEM`.
Every current caller ignores that return, so loss is visible only through the drop record.
A producer that cannot even assemble a record has nothing to push and calls
`ksu_event_queue_drop()` instead, which burns a sequence number and notes the drop without
allocating; `ksu_sulog_capture()` in [`sulog/event.c`](../sulog/event.c) reaches it from
the `out_drop` label every failure path in that function falls through to, so an entry lost
to memory pressure still appears in the stream as a numbered gap.

Copying to userspace can fault and therefore cannot happen under a spinlock.
`ksu_event_queue_read_node()` peeks the head and computes the record size under the lock,
drops it, `copy_to_user`s header and payload, then retakes the lock to unlink and free.
That is safe only because `read_lock` excludes other readers and producers only ever
append; [`sulog/fd.c`](../sulog/fd.c) reinforces it with `ksu_sulog_fd_active`, since
reads are destructive and two readers would split the stream.

Drops are reported in band. `ksu_event_queue_read_drop()` builds a synthetic record of
type `KSU_EVENT_QUEUE_TYPE_DROPPED` (0xFFFF) with `KSU_EVENT_RECORD_FLAG_INTERNAL` set,
and before unlocking it moves the pending counters into `dropped_inflight*` fields rather
than clearing them. If a `copy_to_user` faults, `out_restore` folds them back, widening
the reported range rather than overwriting it when new drops landed during the copy, and
`ksu_event_queue_has_data_locked()` counts the inflight fields so `poll()` still reports
readable mid-copy. Clearing before the copy would lose the count on a fault; clearing
after would clobber a concurrent producer's increment.

`ksu_event_queue_close()` only sets `closed` and wakes pollers, so the fd stays valid and
returns EOF. `ksu_event_queue_destroy()` sets `closed` before taking `read_lock`, which
lets a blocked reader wake and release the mutex instead of deadlocking module exit.
`struct ksu_event_record_hdr` in [`event_queue.h`](event_queue.h) is `{u16, u16, u32, u64,
u64}` - 24 bytes, no padding - and does not live in [`uapi/`](../../uapi/README.md), so
[`ksud/src/sulog.rs`](../../userspace/ksud/src/sulog.rs) hand-mirrors it as a `#[repr(C,
packed)]` struct; its 8192-byte buffer clears the `-EMSGSIZE` a short buffer earns.

## seccomp_cache.c

The `[ksu_driver]` fd is created `O_CLOEXEC`, so it does not survive [`execve`][execve-2].
The manager app receives one directly when [`hook/setuid_hook.c`](../hook/setuid_hook.c)
sees zygote [`setresuid()`][setresuid-2] to its uid, but anything it execs - `libksud.so`
above all - starts with nothing and must ask for one. The only way to ask is
`reboot(0xDEADBEEF, 0xCAFEBABE, 0, &out_fd)`, which
[`supercall/supercall.c`](../supercall/supercall.c) catches with a kprobe on `REBOOT_SYMBOL`
and answers from `task_work`. An app forked from zygote runs under zygote's seccomp filter,
and if that filter does not unconditionally permit [`reboot`][reboot-2], the handshake never
reaches the kprobe.

Since 5.11 the kernel emulates each installed [BPF][bpf] filter over every syscall number at
attach time and records the unconditionally-allowed ones in a bitmap.
`seccomp_run_filters()` consults only the head filter's bitmap and, on a hit, returns
`SECCOMP_RET_ALLOW` without running a single BPF instruction, so setting one bit makes the
whole chain short-circuit for that syscall. There is one bitmap per syscall ABI -
`seccomp_cache_check_allow()` picks `allow_native` or `allow_compat` off `sd->arch` - and
`ksu_seccomp_allow_cache()` sets the bit in both, the compat one under `#ifdef
SECCOMP_ARCH_COMPAT` and each within its own `SECCOMP_ARCH_*_NR` bound. It writes the same
number to both, though, and the call sites pass the kernel's own `__NR_reboot`, 142 on
arm64, where a 32-bit caller's `reboot` is compat number 88. A 32-bit app entering through
the compat table gets no short-circuit and stays at its filter's mercy, so the handshake is
dependable only for 64-bit callers here. `struct seccomp_filter` and `struct action_cache`
are private to `kernel/seccomp.c` with no header, so [`seccomp_cache.c`](seccomp_cache.c)
re-declares both field for field:

```c
struct seccomp_filter {
    refcount_t refs;
    refcount_t users;
    bool log;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
    bool wait_killable_recv;
#endif
    struct action_cache cache;
    struct seccomp_filter *prev;
    // ... prog, notif, notify_lock, wqh
};
```

Nothing checks this at compile time. A field added upstream before `cache` moves the
bitmap and turns `set_bit` into memory corruption inside a live filter, and the
`wait_killable_recv` guard is the only version adaptation present. Both call sites sit in
`ksu_handle_setresuid()` and hold `current->sighand->siglock` across the call - the rule
`disable_seccomp()` in [`policy/app_profile.c`](../policy/app_profile.c) documents from
`seccomp_set_mode_strict`. That setresuid moment is the only opportunity:
`escape_with_root_profile()` calls `disable_seccomp()` itself, right after
`commit_creds()`, so once an app has actually taken root there is no filter left to patch.
`ksu_seccomp_clear_cache()` is defined but called from nowhere in the tree; its
declaration sits in [`seccomp_cache.h`](seccomp_cache.h), whose include guard
`__KSU_H_KERNEL_COMPAT` is a leftover from an earlier file name.

## su_mount_ns.c

A mount namespace is a private view of the mount tree; `setns(fd, CLONE_NEWNS)` joins an
existing one and `unshare(CLONE_NEWNS)` clones the current one.
[`su_mount_ns.c`](su_mount_ns.c) implements the two non-inherited values of `struct
root_profile.namespaces` ([`uapi/app_profile.h`](../../uapi/app_profile.h)) and is called
from one place, the tail of `escape_with_root_profile()` in
[`policy/app_profile.c`](../policy/app_profile.c), just before it drops its reference to
the profile. `KSU_NS_INHERITED` returns immediately and is the default
[`policy/allowlist.c`](../policy/allowlist.c) installs; neither real mode reports failure
upward, since `setup_mount_ns()` returns `void` and logs. Both run under
`override_creds(ksu_cred)`, which installs KernelSU's own credential for the duration, and
neither is capability-free: `ksys_unshare(CLONE_NEWNS)` demands `CAP_SYS_ADMIN`, while
`mntns_install()` demands `CAP_SYS_ADMIN` in the target namespace's user namespace and
again in the caller's, plus `CAP_SYS_CHROOT`, as the comment above `ksu_mnt_ns_global()`
records. That is why the override matters: the function runs after `commit_creds()` has
applied the profile's capability set, which a restrictive profile may have trimmed to
nothing, and a profile left holding `CAP_SYS_ADMIN` alone would fail global mode with
`-EPERM` on the `CAP_SYS_CHROOT` test.

Global mode joins PID 1's namespace. It finds init with `find_pid_ns(1, &init_pid_ns)`
under `rcu_read_lock()` - the comment notes that `&init_task` is swapper/idle, not init -
takes an nsfs path with `ns_get_path()`, opens it as `ksu_cred`, and installs a real fd,
because the syscall takes an fd. Kernel code cannot call `setns` directly on a
`CONFIG_ARCH_HAS_SYSCALL_WRAPPER` kernel, where `SYSCALL_DEFINE` generates a wrapper that
unpacks its arguments from `pt_regs`, so `ksu_sys_setns()` zeroes a local `struct
pt_regs`, writes the fd and flags through the `PT_REGS_PARM1`/`PT_REGS_PARM2` macros of
[`include/arch.h`](../include/arch.h), and calls `__arm64_sys_setns` or `__x64_sys_setns`
with it.

The cwd is saved and restored around that call because `mntns_install()` ends with
`set_fs_pwd()` and `set_fs_root()` and unconditionally relocates the task to `/`. It is
saved as an absolute string from `d_path()` and re-resolved with `kern_path()` afterwards,
since the old `struct path` belongs to the tree being left behind; only the cwd comes
back, the fs root stays where `mntns_install()` put it. Both halves are best-effort:
`d_path()` appends ` (deleted)` for an unlinked directory, on which `kern_path()` fails,
and `mntns_install()` returns `-EINVAL` when `fs->users != 1`, so a caller sharing its
`fs_struct` logs `call setns failed: -22` and keeps its old namespace.

Individual mode is `ksys_unshare(CLONE_NEWNS)` followed by `path_mount(NULL, &root_path,
NULL, MS_PRIVATE | MS_REC, NULL)`. The recursive `MS_PRIVATE` is not optional: unsharing
copies the parent's mount tree with its propagation types intact, and Android's mounts are
extensively shared, so without severing propagation every mount and umount the root
process performs would travel straight back into the app's - and often the system's -
namespace, the exact visibility this mode exists to prevent. `struct ksu_mns_tw` in
[`su_mount_ns.h`](su_mount_ns.h) is dead residue of a `task_work`-deferred design, and the
file builds only for `__aarch64__` and `__x86_64__`.

## See also

- [`kernel/README.md`](../README.md) - build modes, init order, layer map
- [`kernel/core/README.md`](../core/README.md) - init and exit ordering
- [`kernel/hook/README.md`](../hook/README.md) - the resolver's and seccomp cache's
  callers
- [`kernel/supercall/README.md`](../supercall/README.md) - the `[ksu_driver]` fd and its
  ioctls
- [`kernel/sulog/README.md`](../sulog/README.md) - the only user of `event_queue`
- [`kernel/policy/README.md`](../policy/README.md) - App Profiles and
  `escape_with_root_profile()`
- [`kernel/selinux/README.md`](../selinux/README.md) - where `ksu_file_sid` comes from
- [`kernel/feature/README.md`](../feature/README.md) - the heaviest resolver consumers
- [`uapi/README.md`](../../uapi/README.md) - the ABI these fds are requested over
- [`userspace/ksuinit/README.md`](../../userspace/ksuinit/README.md) - the load-time
  symbol half
- [`docs/architecture.md`](../../docs/architecture.md) - end-to-end flows

<!-- reference links: kernel documentation and man pages -->
[bpf]: https://docs.kernel.org/bpf/index.html
[elf-5]: https://man7.org/linux/man-pages/man5/elf.5.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[init-module-2]: https://man7.org/linux/man-pages/man2/init_module.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[lsm]: https://docs.kernel.org/security/lsm.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[setresuid-2]: https://man7.org/linux/man-pages/man2/setresuid.2.html
[uprobetracer]: https://docs.kernel.org/trace/uprobetracer.html
