# The Attachment Layer

`kernel/hook/` is the code that splices KernelSU into a running kernel. Nothing here
implements a user-visible feature; it implements the four ways this module gets control, and
the lifecycle rules that let it hand control back without leaving a pointer into freed
module text behind. Everything above it - sucompat, kernel umount, mount hiding, adb root,
the ksud boot pipeline, [SELinux][selinux] hiding - is a handler function this layer calls.
The four techniques are not interchangeable; which one a piece of code uses is decided by
three questions: must it fire for every task or only for a chosen few, can it tolerate
running with preemption disabled, and does the kernel offer a supported registration path at
all.

| Technique | Used for | Cost | Files |
|---|---|---|---|
| Syscall dispatcher ([tracepoint][tracepoints] redirect into a borrowed `ni_syscall` slot) | [`setresuid`][setresuid-2], [`execve`][execve-2], [`execveat`][execveat-2], `newfstatat`, [`faccessat`][faccessat-2], for *marked* tasks only | one syscall-table entry, total | [`syscall_hook_manager.c`](syscall_hook_manager.c), [`arm64/syscall_hook.c`](arm64/syscall_hook.c), [`x86_64/syscall_hook.c`](x86_64/syscall_hook.c) |
| Direct syscall-table patching | `read` and `fstat` during early boot, for *every* task | one entry per hooked syscall, system-wide | [`arm64/syscall_hook.c`](arm64/syscall_hook.c), [`x86_64/syscall_hook.c`](x86_64/syscall_hook.c) |
| [LSM][lsm] hook-slot patching | displacing `selinux_setprocattr` | one pointer inside an existing LSM registration | [`lsm_hook.c`](lsm_hook.c) |
| [kprobes][kprobes] / [kretprobes][kprobes] | observing `syscall_regfunc` and `syscall_unregfunc` | a breakpoint, and a handler in atomic context | [`syscall_hook_manager.c`](syscall_hook_manager.c) |

## Writing to read-only kernel memory

Three of the four techniques come down to storing a pointer into memory the kernel has
mapped read-only: a syscall-table entry for the two syscall techniques, an LSM hook slot
for the third. `CONFIG_STRICT_KERNEL_RWX` makes those pages unwritable, and the obvious
workaround - find the PTE, clear the read-only bit, store, put it back - is exactly what
must not happen here. The comment above `ksu_patch_text_nosync()` in
[`arm64/patch_memory.c`](arm64/patch_memory.c) names the reason: vendor monitors at a higher
exception level watch for it. MediaTek's MKP reports fixmap addresses to its hypervisor, so
a write through the fixmap looks like ordinary kernel text poking, while a PTE flip does
not.

The module therefore does the address translation itself, in `phys_from_virt()`, which
`ksu_patch_text_nosync()` calls before it maps anything. It walks `init_mm`'s page
tables from `pgd_offset()` down to `pte_offset_kernel()`, short-circuiting at each
level whose `p4d_leaf`/`pud_leaf`/`pmd_leaf` test says the address is covered by a huge
page. Those cases are not decoration: the syscall table lives in rodata that is frequently
huge-mapped, and the kernel's own `aarch64_insn_write()` resolves its target with
`vmalloc_to_page()`, which mishandled huge mappings before 5.13 - and writes only four
bytes at a time. The two copies of the walk differ only where the architecture forces them
to. Both guard every huge-page test with `#if defined(p4d_leaf)` and its two siblings; what
the x86_64 copy adds is an `#elif defined(p4d_large)` arm, so a kernel that predates the
`_leaf` rename still short-circuits instead of descending into a table that is not there.
It also turns a level entry into a physical address through its own
`KSU_P4D_TO_PHYS()`/`KSU_PUD_TO_PHYS()`/`KSU_PMD_TO_PHYS()`/`KSU_PTE_TO_PHYS()` macros,
each one the matching `*_pfn()` shifted by `PAGE_SHIFT`, because arm64's `__pte_to_phys()`
family has no x86 counterpart. `phys_from_virt()` reports failure through an out-parameter
rather than a sentinel return, because physical address zero is a legal address. The master
CPU then maps the page through the fixmap (`set_fixmap_offset(FIX_TEXT_POKE0, phy)` on
arm64; modern x86 removed `FIX_TEXT_POKE0`, so
[`x86_64/patch_memory.c`](x86_64/patch_memory.c) uses `FIX_BTMAP_BEGIN` plus the in-page
offset), stores with `copy_to_kernel_nofault()`, and clears the fixmap.

No TLB maintenance appears anywhere in this path, and none is needed. `set_fixmap_offset()`
on arm64, `set_fixmap()` on x86_64 and `clear_fixmap()` on both install and tear down the
temporary mapping through `__set_fixmap()`, which flushes the entry it changes; no existing
mapping's permissions are altered, so nothing else in the address space has gone stale.

All of it runs under `stop_machine()`, which parks every online CPU. A multi-byte,
non-atomic store into a live function-pointer table has no other protection against another
CPU reading a half-written value. Inside `ksu_patch_text_cb()` the CPUs rendezvous on an
atomic counter: whichever one's `atomic_inc_return(&pp->cpu_count)` reaches
`num_online_cpus()` becomes master and does the write, the rest `cpu_relax()` until the
master's extra increment, and each of them then issues `isb()` (arm64) or `smp_mb()`
(x86_64) so none retires an instruction fetched before the change.

Cache maintenance is selected by the `flags` argument. On arm64 the D-cache macro is chosen
by `KSU_NEW_DCACHE_FLUSH`, computed in [`../Kbuild`](../Kbuild) by grepping the target
kernel's `arch/arm64/include/asm/cacheflush.h` for `__flush_dcache_area`. Watch the polarity:
the value is `grep -q`'s exit status, so `1` means the old symbol is absent and
`dcache_clean_inval_poc()` applies. A version check would be wrong, because that API change
landed in 5.14 upstream but was backported to android13-5.10 and not android12-5.10.

Every rodata write in the module goes through `ksu_patch_text()`, including those made from
outside this directory: [`../feature/selinux_hide.c`](../feature/selinux_hide.c) patches
selinuxfs `write_op` entries and `sel_open_handle_status`, and
[`../feature/mount_hide.c`](../feature/mount_hide.c) patches the `.open` slot of the three
procfs mount `file_operations`.

Both `patch_memory.c` files export a third function that has nothing to do with patching.
`scan_call_to(start, size, target)` walks a span of kernel text looking for a call
instruction whose destination is `target` and returns the address of the first match, or
NULL. The arm64 version decodes the AArch64 `BL` encoding by hand - opcode bits 31:26 are
`0b100101`, and the sign-extended 26-bit immediate scaled by four gives the displacement
from the instruction's own address - so it is a four-byte-at-a-time scan with no
disassembler behind it. The x86_64 copy is a `// TODO:` stub that returns NULL.

Its one caller is `ksu_app_profile_init()` in
[`../policy/app_profile.c`](../policy/app_profile.c), which runs from `kernelsu_init()` once
the symbol resolver is up, and it exists because a version test cannot answer the question
it asks. `disable_seccomp()` drops a task's [seccomp][seccomp-filter] filter by handing
`seccomp_filter_release()` a heap copy of `task_struct` rather than the live one; what that
copy has to look like changed when upstream made `seccomp_filter_release()` take
`sighand->siglock` itself, after which the fake needs `PF_EXITING` set instead of a NULL
`sighand`. That change is 6.11 upstream but reached some android15-6.6 kernels as a
backport and not others - Pixel 10 is among the ones without it - so `LINUX_VERSION_CODE`
cannot distinguish them. Between 6.6 and 6.11 the module therefore asks the text itself:
`kallsyms_lookup_size_offset()` bounds `seccomp_filter_release`, and a `BL` to
`_raw_spin_lock_irq` anywhere inside those bounds means the backport is present. Should
that lookup fail, the probe falls back to a flat 128 bytes, and the guess errs in one
direction only: a window shorter than the real function hides the `BL`, and a `BL` not
found reads as "no backport".

## The syscall dispatcher

Patching `sys_call_table[__NR_execve]` would run KernelSU code for every `execve` on the
device and would leave five visibly altered entries for an integrity checker to find. The
dispatcher avoids both: it patches exactly one entry, an unused one, and reaches it only for
tasks that have been marked.

`ksu_syscall_hook_init()` resolves `sys_call_table`, then scans it for the first slot still
pointing at `__arm64_sys_ni_syscall` (or `__x64_sys_ni_syscall`). That index becomes
`ksu_dispatcher_nr`, and `ksu_syscall_table_hook()` patches it to `ksu_syscall_dispatcher`.
Both lookups go through `ksu_resolve_symbol_for_functable_hook()` in
[`../infra/symbol_resolver.c`](../infra/symbol_resolver.c) rather than a plain kallsyms
lookup, because the value being compared against is whatever the table holds: on kernels
before 6.1, built with Clang's jump-table CFI, that is the `.cfi_jt` thunk and not the
function, so resolving the function address would match nothing and the dispatcher would
never be installed. `ksu_syscall_hook_manager_init()` then fills a routing table -
`syscall_hooks[]`, indexed by syscall number - and registers a probe on the `sys_enter`
tracepoint. A tracepoint is a static instrumentation point the kernel calls when at least
one probe is attached; the per-task `SYSCALL_TRACEPOINT` work flag decides whether a given
task's syscall entry path reaches `trace_sys_enter()` at all.

### Redirecting a marked task

`ksu_sys_enter_handler()` returns immediately for 32-bit tasks (`in_compat_syscall()` on
x86_64, `is_compat_task()` on arm64) and when the dispatcher slot was never found. For a
hooked syscall number it rewrites the live register file:

```c
    if (ksu_has_syscall_hook(id)) {
        struct pt_regs *current_regs = task_pt_regs(current);

#if defined(__x86_64__)
        // Stash the original syscall number in ax.
        // We use ax because it currently just holds -ENOSYS and is safe to overwrite.
        current_regs->ax = id;
        current_regs->orig_ax = ksu_dispatcher_nr;
#elif defined(__aarch64__)
        PT_REGS_ORIG_SYSCALL(current_regs) = id;
        current_regs->syscallno = ksu_dispatcher_nr;
#endif
    }
```

`PT_REGS_ORIG_SYSCALL` is `regs[8]` on arm64 and `orig_ax` on x86_64, per
[`../include/arch.h`](../include/arch.h). On arm64 `x8` already holds the syscall number, so
that store changes nothing and the real redirect is the write to `regs->syscallno`, which
`el0_svc_common()` re-reads after the trace hook. On x86_64 the stash into `ax` does work:
the entry path left `-ENOSYS` there, so the register is free, and `syscall_get_nr()` reads
`orig_ax`.

The probe is registered with `register_trace_prio_sys_enter(..., INT_MIN)`. The kernel keeps
its probe array in descending priority order, so `INT_MIN` places this probe last; commit
c20d48a1 states the intent, which is that it "allows other tracepoint handler like perf/bpf
to run before we corrupt the context".

### The dispatcher and its two guards

The `ni_syscall` slot is reachable from userspace by number, so the dispatcher checks twice
that it was entered through the redirect. On arm64:

```c
    if (regs->syscallno != ksu_dispatcher_nr)
        return -ENOSYS;

    int orig_nr = (int)PT_REGS_ORIG_SYSCALL(regs);

    if (regs->syscallno == orig_nr)
        return -ENOSYS;
```

The x86_64 copy in [`x86_64/syscall_hook.c`](x86_64/syscall_hook.c) is the same shape
against different registers, because `struct pt_regs` there has no `syscallno` member at
all: it tests `regs->orig_ax` against `ksu_dispatcher_nr`, then reads the original number
out of `regs->ax`.

A process calling `syscall(ksu_dispatcher_nr, ...)` directly on arm64 arrives with `x8`
equal to `syscallno`, so the second test fires and it gets the `-ENOSYS` that
`sys_ni_syscall` would have returned; on x86_64 the same call leaves `ax` at `-ENOSYS`, so
`orig_nr` is negative and falls out of the range check that follows. Remove either guard and
unprivileged userspace can drive KernelSU handlers with a hand-crafted register frame. Past
them, the dispatcher restores the borrowed registers and calls
`READ_ONCE(syscall_hooks[orig_nr])(orig_nr, regs)`.

### The hooked syscalls

A registered handler *owns* the dispatch, as [`syscall_hook.h`](syscall_hook.h) says: it must
invoke the real syscall itself with `ksu_syscall_table[orig_nr](regs)`, or the syscall
silently returns whatever the handler returned. All five are declared in
[`syscall_event_bridge.h`](syscall_event_bridge.h), defined in
[`syscall_event_bridge.c`](syscall_event_bridge.c), and are `__nocfi`, because calling
through a raw pointer read out of rodata has no CFI type id the module could check.

| Syscall | Handler | What it drives |
|---|---|---|
| `__NR_setresuid` | `ksu_hook_setresuid` | runs the real syscall first, then `ksu_handle_setresuid()` in [`setuid_hook.c`](setuid_hook.c) |
| `__NR_execve` | `ksu_hook_execve` | ksud boot milestones, sulog capture, adb_root for init children, sucompat for everyone else |
| `__NR_execveat` | `ksu_hook_execveat` | the same four jobs, against the `execveat` register layout |
| `__NR_newfstatat` | `ksu_hook_newfstatat` | `ksu_handle_stat_sucompat()` in [`../feature/sucompat.c`](../feature/sucompat.c) |
| `__NR_faccessat` | `ksu_hook_faccessat` | `ksu_handle_faccessat_sucompat()` |

Both exec entries are one-line wrappers over a shared
`ksu_hook_execve_common(orig_nr, regs, execveat)`, and the `bool` is the whole difference.
The second entry point is not redundancy. Bionic in Android 17 QPR2 stopped issuing
`__NR_execve` at all - `execve`, `execv` and their relatives now tail-call
`__execveat(AT_FDCWD, path, argv, envp, 0)` - so a build that routed only `__NR_execve`
would observe no exec whatsoever on such a device, and ksud's boot milestones, sulog, adb
root and sucompat would all go quiet at once. The register layout shifts by one: the path
is the second argument rather than the first, argv the third rather than the second, and
envp lands in the syscall's fourth register, `PT_REGS_SYSCALL_PARM4`, which is `r10` on
x86_64 and not `rdx`. Instead of normalizing once in the bridge, the flag travels into the
ksud boot hook, [`../feature/adb_root.c`](../feature/adb_root.c) and
[`../feature/sucompat.c`](../feature/sucompat.c), each of which grew a paired `_execveat`
entry point that picks the right registers before joining the common body.
`ksu_handle_execveat_sucompat()` additionally refuses anything but the `execve`-equivalent
form, falling straight through to the real syscall unless the dirfd is `AT_FDCWD` and the
flags are zero, because its su-path rewrite shuffles arguments on the assumption that the
call means "execute this path".

`ksu_hook_execve_common()` is where the arbitration happens. For an init-domain child that
is not PID 1 it runs `ksu_handle_init_mark_tracker()` and then
`ksu_adb_root_handle_execve()` or `ksu_adb_root_handle_execveat()`; otherwise, if sucompat
is enabled, it hands off to `ksu_handle_execve_sucompat()` or its `execveat` twin and
returns that result directly. The ksud boot hook runs first on both paths, behind
`DEFINE_STATIC_KEY_TRUE(ksud_execve_key)` - a static branch patched out of the instruction
stream by `ksu_stop_ksud_execve_hook()` once
[`../runtime/ksud_integration.c`](../runtime/ksud_integration.c) sees the first
`app_process -Xzygote` exec. `ksu_handle_init_mark_tracker()` copies the exec target into a
64-byte buffer with `strncpy_from_user()` and branches on it. An exact match against
`KSUD_PATH` calls `escape_to_root_for_init()`, which is how ksud itself acquires its
credentials at the moment init launches it. Every other target falls to the `else if`,
which clears the task's tracepoint flag unless the path contains `/app_process`, `/adbd` or
`/stub_zygote` - the ancestors that must keep it, zygote so its app children inherit the flag
and adbd so `su` inside `adb shell` still reaches sucompat.

`ksu_hook_setresuid` is the one handler that runs the real syscall before it decides
anything, because the decision depends on the uid the task ended up with.
`ksu_handle_setresuid()` in [`setuid_hook.c`](setuid_hook.c) then splits three ways. For the
manager's uid it sets the tracepoint flag and a seccomp cache entry for `__NR_reboot`, both
under `spin_lock_irq(&current->sighand->siglock)`, calls `ksu_install_fd()` to hand the
process a fresh anon-inode fd, and returns there. Seccomp keeps a per-filter bitmap of the
syscall numbers its [BPF][bpf] program is known to allow unconditionally, consulted before
the program runs; setting that bit is what lets the magic-argument [`reboot()`][reboot-2]
supercall, and the [kprobe][kprobes] on it in
[`../supercall/supercall.c`](../supercall/supercall.c), reach the kernel from a process the
zygote has already sealed behind a filter. An allow-listed uid gets the same seccomp poke
when it is running a filter at all (`current->seccomp.mode == SECCOMP_MODE_FILTER &&
current->seccomp.filter`), plus the mark; the second conjunct is not redundant, because the
filter pointer is the argument `ksu_seccomp_allow_cache()` dereferences. Anything else gets
`ksu_clear_task_tracepoint_flag_if_needed()`. Those two paths, but not the manager's, finish
in `ksu_handle_umount()`.

## Keeping the tracepoint narrow

The kernel's syscall [tracepoints][tracepoints] declare `syscall_regfunc()` as their
registration callback, and that function sets `SYSCALL_TRACEPOINT` on *every* task on the
system - which would put every syscall of every process on the tracepoint slow path and into
`ksu_sys_enter_handler()`. Blindly clearing the flag instead is equally wrong: it would
break any other user of syscall tracepoints, such as an `strace` the device owner attached.

[`tp_marker.c`](tp_marker.c) resolves this with a reference count, `tracepoint_reg_count`,
guarded by a file-static `tracepoint_reg_lock`. Two kretprobes - probes whose handler runs
when the probed function *returns* - are installed by
[`syscall_hook_manager.c`](syscall_hook_manager.c) on `syscall_regfunc` and
`syscall_unregfunc`, so they observe the blanket marking after it has happened.
`syscall_regfunc_handler()` reaches the count through the `ksu_tp_marker_lock()` and
`ksu_tp_marker_reg_count()` accessors, since the spinlock itself never leaves tp_marker.c,
and, if the count is below one, calls `ksu_mark_running_process_locked()` to undo that
marking and re-narrow it; if the count is exactly one, another subsystem is turning tracing
on while only KernelSU was present, so it calls `ksu_mark_all_process()` to restore what
that subsystem expects. `syscall_unregfunc_handler()` is the mirror image, and
`ksu_clear_task_tracepoint_flag_if_needed()` - the guarded clear used everywhere else -
takes the lock for itself and only clears while the count is at most one.

Setting and clearing that flag is two inline functions in [`tp_marker.h`](tp_marker.h), and
they hide a kernel version split. From 5.11 the flag is syscall work, so
`ksu_set_task_tracepoint_flag()` expands to `set_task_syscall_work(t, SYSCALL_TRACEPOINT)`;
below 5.11 it is an ordinary thread-info flag,
`set_tsk_thread_flag(t, TIF_SYSCALL_TRACEPOINT)`. Every mention of setting or clearing the
tracepoint flag elsewhere in this file means a call to that pair. The same header exports
`ksu_get_task_mark()` and `ksu_set_task_mark()`, the per-pid operations behind the
`KSU_IOCTL_MANAGE_MARK` ioctl; both wrap
`find_task_by_vpid()` in `rcu_read_lock()` and pin the result with `get_task_struct()`
before dropping that lock, because the lookup only borrows the task for the length of the
[RCU][whatisrcu] read-side critical section and the flag write happens after it.

`ksu_mark_running_process_locked()` walks `for_each_process_thread` under
`read_lock(&tasklist_lock)`, skips kernel threads (`!t->mm`) except PID 1, and marks a
thread when it is a uid-0 task in the KernelSU SELinux domain, a zygote, uid 2000, PID 1, or
an allow-listed uid; everything else is explicitly cleared. Marks are also set directly by
`ksu_handle_setresuid()`, by `escape_with_root_profile()` in
[`../policy/app_profile.c`](../policy/app_profile.c) for every thread of a newly escalated
process, and through the `KSU_IOCTL_MANAGE_MARK` ioctl in
[`../supercall/dispatch.c`](../supercall/dispatch.c). A whole re-scan is
`ksu_mark_running_process()`, the wrapper that takes `tracepoint_reg_lock` itself and
declines while the count says another tracer is attached; that ioctl's `KSU_MARK_REFRESH`
operation and every successful `ksu_set_app_profile()` call it, so a profile change reaches
processes that are already running. Two costs: the [kretprobe][kprobes] handlers run in
atomic context with interrupts disabled and from there take `tasklist_lock` and `pr_info()`
once per thread on the system, and because `ksu_sys_enter_handler()` returns early for
compat tasks, no dispatcher-routed feature covers a 32-bit process.

Both halves of the scheme are conditional on kernel config, and the degraded builds behave
differently. The kretprobes and the counting live under `#ifdef CONFIG_KRETPROBES`; with
that off, `ksu_syscall_hook_manager_init()` calls `ksu_mark_running_process_locked()` once
at init and nothing ever re-narrows the marking afterwards, so the first `strace` that
fires `syscall_regfunc` leaves every task on the tracepoint path permanently. The probe
registration and `ksu_sys_enter_handler()` themselves sit under
`#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS`; without it the dispatcher slot is still patched
into the syscall table but nothing ever redirects into it, so none of the five routed
syscalls reach a handler.

Those two kretprobes are the only probes this directory installs, and they show what the
fourth technique is good for: a kprobe plants a breakpoint at a symbol and runs a handler
with preemption disabled, which suits observing an event but not acting on one. Probes
elsewhere in the module - the [`reboot`][reboot-2] kprobe in
[`../supercall/supercall.c`](../supercall/supercall.c), `input_event` in
[`../runtime/ksud_integration.c`](../runtime/ksud_integration.c), killguard in
[`../feature/ptctl.c`](../feature/ptctl.c), the three kretprobes
[`../feature/mem_spoof.c`](../feature/mem_spoof.c) arms on `si_meminfo`, `si_mem_available`
and `vm_commit_limit`, and the [uprobes][uprobetracer] in
[`../feature/uhook.c`](../feature/uhook.c) - matter here only because module exit has to
retire every one before the text they point at is freed.

## Direct syscall-table patching

`ksu_syscall_table_hook(nr, fn, old)` overwrites `ksu_syscall_table[nr]`, hands the caller
the previous value, and - the part that matters - records `{nr, orig}` in `hooked_entries[]`
so the slot can be restored at module exit; calling `ksu_patch_text()` on
`&ksu_syscall_table[nr]` directly would leave module text installed in the table after
`rmmod`. Only two real syscall entries are replaced this way, both from `ksu_ksud_init()`
in [`../runtime/ksud_integration.c`](../runtime/ksud_integration.c) - the dispatcher slot is
installed through the same `ksu_syscall_table_hook()` entry point, which is why a single
restore loop covers all three:

| Syscall | Replacement | Purpose |
|---|---|---|
| `__NR_read` | `ksu_sys_read` | detect init's first read of `init.rc` and install a proxy `file_operations` on that `struct file` |
| `__NR_fstat` | `ksu_sys_fstat` | inflate the reported `st_size` so init allocates a buffer large enough for the injected stanzas |

These cannot use the dispatcher: they must fire for PID 1 during very early boot, before any
allowlist or marking policy exists. The deeper reason they are table hooks rather than
kprobes is the teardown path. `stop_init_rc_hook()` calls `ksu_syscall_table_unhook()`, and
therefore `stop_machine()`, from *inside* `ksu_sys_read` while that syscall is executing -
legal only because a syscall-table hook runs in ordinary process context, whereas a kprobe
pre-handler runs with preemption disabled, where `stop_machine()` would deadlock. Commit
225ffbbf puts it in its own words: "Make most of atomic-context-unsafe code running in
normal context".

## The x86_64 hardened dispatcher

Upstream commit `1e3ad78334a6` (6.9, backported to nearly every GKI branch except 5.10)
replaced x86_64's indirect call through `sys_call_table` with `x64_sys_call()`, a switch of
direct branches. Patching the table then has no effect at all. There are two ways out and
this tree supports both.

Without `CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER` the kernel is expected to carry a source
patch that reinstates the indirection behind `X86_FEATURE_INDIRECT_SAFE`. Its absence is
caught twice, in two blocks of [`../core/init.c`](../core/init.c) that both sit under
`#if defined(__x86_64__) && !defined(CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER)`, and only
one of the two is a runtime check. When `X86_FEATURE_INDIRECT_SAFE` is not even defined,
the file fails the *build* with a `#error` reading "FATAL: Your kernel is missing the
indirect syscall bypass patches!"; when the macro exists but `boot_cpu_has()` finds the
feature bit clear at boot, `kernelsu_init()` prints a banner and returns `-ENOSYS`.

With the config on, `ksu_syscall_hook_init()` restores the indirection at runtime before
installing the dispatcher. `patch_abs_jump()` resolves `x64_sys_call`, compares the first
four bytes against `f3 0f 1e fa` and steps past them if present - overwriting an `endbr64`
landing pad would fault every indirect call to that function on IBT hardware - backs up 14
bytes, and writes `ff 25 00 00 00 00` (`jmp *0(%rip)`) followed by the 8-byte absolute
target. Fourteen bytes rather than a five-byte `rel32` because module text sits far outside
the 2GB reach of a relative jump from the kernel image. The target, `my_x64_sys_call()`, is
one line: `return ksu_syscall_table[nr](regs);`. This is the path the tree's own x86_64
module builds take: [`../build-all-x64.sh`](../build-all-x64.sh) passes
`CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER=y` for every KMI it walks, android12-5.10 through
android17-6.18, because an LKM cannot require that the kernel it is loading into was
patched at build time.

There is a second, older gap. Kernels before 5.16 lack commit `fb13b11d5387`, so
`syscall_trace_enter()` does not re-read the syscall number after `trace_sys_enter()` and
the tracepoint's rewrite is ignored outright. For that case (the android13-5.15 AVD) the same
machinery patches all of `do_syscall_64` to `my_do_syscall_64()`, which calls the resolved
`syscall_enter_from_user_mode`, re-reads the number with `syscall_get_nr()`, dispatches
through `ksu_syscall_table[]`, and finishes with `syscall_exit_to_user_mode`; x32 is
deliberately not handled. Both 14-byte backups are restored at the top of
`ksu_syscall_hook_exit()`, before the syscall table itself.

## LSM hook slots

The Linux Security Module framework registers hooks with `security_add_hooks()`, which is
`__init`-only and fixes the LSM order at boot. A module loaded later cannot use it.
[`lsm_hook.c`](lsm_hook.c) instead overwrites the function pointer inside a registration
that already exists, so the hook runs in a position the kernel has already wired up and no
new list entry has to be made visible to an RCU reader.

The mechanism split at Linux 6.12. Before it, `ksu_lsm_hook()` resolves
`security_hook_heads`, sizes it with `kallsyms_lookup_size_offset()`, and walks every
`struct hlist_head` in that region, reading the slot at `(char *)entry + hook->hook_offset`
of each `struct security_hook_list`. From 6.12 the hlist walk was replaced by static calls:
it resolves `static_calls_table` and `lsm_active_cnt`, derives the array length, and walks
`struct lsm_static_call` entries. Patching there takes two steps - the pointer inside
`struct security_hook_list`, then `__static_call_update()` on the trampoline - and the first
is rolled back if the second fails, because the call site branches through the trampoline
and writing only the pointer would change nothing.

Selection is by resolved function-pointer identity, never by head. Both branches scan the
entire region and ignore `hook->head_offset`; since `hook.member` is a union, `hook_offset`
is the same constant for every member, so the `target_symbol` argument of `KSU_LSM_HOOK_INIT`
is the load-bearing one and `member` is close to documentation. Before comparing, the scan
de-aliases against `ksu_lsm_hook_entries[]`: a slot holding another tracked hook's
`replacement` is read as that hook's `original`, so two KernelSU hooks on one slot chain
correctly instead of the second recording the first's replacement as its own original. One
caller exists in the whole tree - [`../feature/selinux_hide.c`](../feature/selinux_hide.c)
declares `KSU_LSM_HOOK_INIT(setprocattr, "selinux_setprocattr", my_setprocattr, 0)`.

Both `ksu_lsm_hook()` and `ksu_lsm_unhook()` hold `ksu_lsm_hook_lock` across the entire
find-and-patch sequence, because the de-aliasing scan reads other hooks' `->original` and
`->replacement`. `ksu_lsm_unhook()` ends with `synchronize_rcu()`: LSM hooks run inside RCU
read-side critical sections, and the restore is only safe once every reader that could have
loaded the replacement pointer has finished. `ksu_lsm_hook_exit()` snapshots the registry
under the mutex and unhooks *outside* it, in reverse index order, because
`ksu_lsm_unhook()` re-acquires the same mutex and chained hooks must be unwound
outermost-first.

The comments in [`lsm_hook.h`](lsm_hook.h) have drifted from the code. There is no
`bpf_lsm_<hook>` default when `target_name` is NULL - `ksu_lsm_hook()` logs
`target_name is required` and returns `-EINVAL`; `ksu_register_lsm_hook()` and
`ksu_unregister_lsm_hook()` are one-line aliases, not a separate chaining API; and
`ksu_lsm_hook_init()` initializes nothing, it prints a count.

## Bring-up order

[`../core/init.c`](../core/init.c) is the only place this order is expressed, and two of its
steps are hard requirements rather than convention. `ksu_init_symbol_resolver()` comes
first, since it caches whichever kallsyms iterator this kernel offers - a pointer to
`kallsyms_on_each_match_symbol` from 6.1, a pointer to `kallsyms_on_each_symbol` below 5.19,
and neither in between, where the symbol is exported and called directly - and every lookup
below it degrades or fails without that. `ksu_syscall_hook_init()` comes next, publishing
`ksu_syscall_table` and `ksu_dispatcher_nr`; it must precede
`ksu_syscall_hook_manager_init()`, whose entire interface is the two functions declared in
[`syscall_hook_manager.h`](syscall_hook_manager.h), because the dispatcher slot has to exist
before the tracepoint can redirect anything into it. The
`if (ksu_dispatcher_nr < 0) return;` in `ksu_sys_enter_handler()` is the safety net for a
boot where no free slot was found. Inside the manager's init, all five
`ksu_register_syscall_hook()` calls happen *before* the tracepoint is registered; with the
tracepoint live first a syscall could be redirected, find a NULL handler and return a
spurious `-ENOSYS` for real work.

That same init ends by starting two things that hang off the hook layer rather than off
`kernelsu_init()` directly.
`ksu_setuid_hook_init()`, declared in [`setuid_hook.h`](setuid_hook.h), brings up
[`../feature/kernel_umount.c`](../feature/kernel_umount.c) and
[`../feature/mount_hide.c`](../feature/mount_hide.c). Those two attack the same visibility
problem from opposite ends and neither replaces the other: umounting changes what is
actually mounted for a uid - now including the webview zygote's uid 1053, once
`KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT` turns that on - while mount hiding leaves the mounts in
place and changes what `/proc/*/mounts` and its two siblings print to an isolated reader.
`ksu_sucompat_init()` registers `su_compat_handler` through
`ksu_register_feature_handler()`, which is the feature-flag plumbing behind
`ksu_su_compat_enabled` rather than a second hook - the sucompat syscall routing was
already done by the five `ksu_register_syscall_hook()` calls above. Mount
hiding's rodata patching therefore happens inside `ksu_syscall_hook_manager_init()`, on both
the built-in and the late-load path.

## Teardown order

None of these mechanisms takes a module reference. A syscall-table slot, a tracepoint probe
list, an LSM hook slot, a static-call trampoline and a 14-byte inline jump all hold a raw
pointer into module `.text` that the kernel will call, so the only protection against `rmmod`
freeing that text under a CPU about to enter it is strictly ordered teardown plus the
kernel's own drain primitives.

`ksu_syscall_hook_manager_exit()` runs first, in this order:

1. `unregister_trace_sys_enter()` then `tracepoint_synchronize_unregister()`. After this no
   CPU is inside `ksu_sys_enter_handler()` and no new syscall can be redirected.
2. `destroy_kretprobe()` on both kretprobes: `unregister_kretprobe()`, `synchronize_rcu()`,
   `kfree()`. The `synchronize_rcu()` is what makes the `kfree()` safe.
3. `ksu_unregister_syscall_hook()` for the five routed numbers.
4. `ksu_syscall_hook_exit()`: restore the x86 inline-jump backups, then walk
   `hooked_entries[]` under `hooked_entries_lock` and patch every recorded original back -
   one loop that covers the dispatcher slot and `__NR_read`/`__NR_fstat` alike - and only
   then clear `syscall_hooks[]` and reset `ksu_dispatcher_nr` to -1. That internal ordering
   is deliberate and commented: the table goes back while the routing state is still intact.
5. `ksu_sucompat_exit()`, which only drops `KSU_FEATURE_SU_COMPAT` from the feature table,
   then `ksu_setuid_hook_exit()`, which calls `ksu_kernel_umount_exit()` - itself dropping
   both of the handlers that file registers, `KSU_FEATURE_KERNEL_UMOUNT` and
   `KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT` - and `ksu_mount_hide_exit()`, which unpatches the
   three procfs `.open` slots and then drops `KSU_FEATURE_MOUNT_HIDE`.

`kernelsu_exit()` then continues with `ksu_uhook_exit()` and `ksu_ptctl_exit()` - the
[uprobe][uprobetracer] consumers, killguard kprobe and armed hardware breakpoints in
[`../feature/uhook.c`](../feature/uhook.c) and [`../feature/ptctl.c`](../feature/ptctl.c)
all point at module text - followed by `ksu_supercalls_exit()`, `ksu_ksud_exit()` (only when
the module was not late-loaded), a `synchronize_rcu()` that waits out readers such as a
handler still walking `allow_list`, and the data-structure teardown: `ksu_observer_exit()`,
`ksu_throne_tracker_exit()`, `ksu_allowlist_exit()`. `ksu_selinux_hide_exit()` and
`ksu_lsm_hook_exit()` come after all of that, because `ksu_lsm_unhook()` carries a
`synchronize_rcu()` of its own per hook and this is the last chance to drain an LSM reader
that may already have loaded a replacement pointer. `ksu_adb_root_exit()`,
`ksu_sulog_exit()`, `ksu_feature_exit()` and `put_cred(ksu_cred)` close the function.

Note one implicit dependency. `stop_init_rc_hook()` normally runs during boot, from inside
`ksu_sys_read` at init's first read of `/system/etc/init/hw/init.rc`, and takes both
`__NR_read` and `__NR_fstat` out of `hooked_entries[]` right there. The commented-out
`stop_init_rc_hook()` in `ksu_ksud_exit()` therefore only matters on a boot where that read
never happened - and even then the two entries still come back, because
`ksu_syscall_hook_exit()` replays whatever `hooked_entries[]` holds.

## Known limits

Teardown is ordered, not drained. Steps 3 and 4 above clear the routing table before the
syscall table is restored, so a syscall the tracepoint redirected just before the unregister
can reach the dispatcher, find a NULL handler and get `-ENOSYS`; and nothing waits for tasks
already executing inside `ksu_syscall_dispatcher()` or a handler, because `stop_machine()`
parks CPUs at the stopper thread rather than evicting a preempted task from module text.

The rule that every probe must be retired before module text is freed is not honoured
module-wide. `ksu_uhook_exit()` and `ksu_ptctl_exit()` are called from `kernelsu_exit()`,
and the `reboot` and `input_event` kprobes go down inside `ksu_supercalls_exit()` and
`ksu_ksud_exit()`, but [`../feature/mem_spoof.c`](../feature/mem_spoof.c) arms its three
kretprobes lazily from `ksu_set_spoof_mem()` and unregisters them only when a later ioctl
asks for a spoofed RAM size of zero. No exit path reaches that file, so an `rmmod` while
memory spoofing is active leaves `si_meminfo`, `si_mem_available` and `vm_commit_limit`
carrying breakpoints whose handlers sit in text the module allocator has already freed.

`ksu_flush_icache` on arm64 is a no-op by accident. Lines 99 and 102 of
[`arm64/patch_memory.c`](arm64/patch_memory.c) take `(start, end)` but expand to a bare
function name - `caches_clean_inval_pou` or `__flush_icache_range` - with no argument list,
so `ksu_flush_icache(a, b);` expands to an expression statement that names a function and
discards it. That is harmless today only because `KSU_PATCH_TEXT_FLUSH_ICACHE` is passed
exclusively from [`x86_64/syscall_hook.c`](x86_64/syscall_hook.c), where the x86 macro is a
deliberate empty `do {} while (0)`; future arm64 instruction patching that trusts the flag
would skip I-cache maintenance without a word.

`scan_call_to()` leaves the mirror-image hole on the other architecture. The x86_64 copy is
a `// TODO:` that returns NULL for every input, so on an x86_64 kernel between 6.6 and 6.11
the backport probe in `ksu_app_profile_init()` always answers no and `disable_seccomp()`
builds its fake `task_struct` in the pre-backport shape. That is the right answer for a
stock 6.6 and the wrong one for an x86_64 build of a 6.6 that carries the backport, and the
caller cannot tell the two cases apart, because a NULL return means "no such call" and
"never looked" alike.

`hooked_entries[]` and `ksu_lsm_hook_entries[]` are both fixed at 16 and fail differently
past that: the syscall path warns and patches anyway, losing restorability, while
`ksu_lsm_hook_track()` returns `-ENOSPC` and the hook is refused. The non-zero
`hook->offset` path in [`lsm_hook.c`](lsm_hook.c) - the one that links a hook's embedded
`list` into an empty hlist head, or jumps by whole LSM strides on 6.12 and later - has no
caller and is effectively untested; on 6.12+, a hook that activated a previously empty slot
with `static_branch_enable()` never disables that branch again on unhook. And the build is
arm64 and x86_64 only: [`patch_memory.h`](patch_memory.h) and
[`../include/arch.h`](../include/arch.h) both close their architecture `#if` with
`#error "Unsupported arch"`, and [`../Kbuild`](../Kbuild) has no third branch, so on any
other architecture neither `patch_memory.o` nor `syscall_hook.o` is compiled in at all.
Separately, `CONFIG_KSU` in [`../Kconfig`](../Kconfig) depends on `KPROBES && EXT4_FS` -
kprobes for the probes above, ext4 for `ext4_unregister_sysfs`.

## See also

- [`../README.md`](../README.md) - the kernel module: build modes, init order, layer map
- [`../core/README.md`](../core/README.md) - module entry, exit and build configuration
- [`../infra/README.md`](../infra/README.md) - the symbol resolver every lookup here uses
- [`../feature/README.md`](../feature/README.md) - the handlers this layer calls
- [`../runtime/README.md`](../runtime/README.md) - the only user of direct table patching
- [`../supercall/README.md`](../supercall/README.md) - the ioctl that drives `tp_marker`
- [`../policy/README.md`](../policy/README.md) - who is allow-listed, and so who gets marked
- [`../../docs/architecture.md`](../../docs/architecture.md) - repository-wide hub

<!-- reference links: kernel documentation and man pages -->
[bpf]: https://docs.kernel.org/bpf/index.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[execveat-2]: https://man7.org/linux/man-pages/man2/execveat.2.html
[faccessat-2]: https://man7.org/linux/man-pages/man2/faccessat.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[lsm]: https://docs.kernel.org/security/lsm.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[setresuid-2]: https://man7.org/linux/man-pages/man2/setresuid.2.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
[uprobetracer]: https://docs.kernel.org/trace/uprobetracer.html
[whatisrcu]: https://docs.kernel.org/RCU/whatisRCU.html
