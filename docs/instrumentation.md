# Process control and userspace instrumentation

Two supercalls in this fork exist for work that root in userspace cannot do at all:
`KSU_IOCTL_PTCTL` (`'K'`, 50) and `KSU_IOCTL_UHOOK` (`'K'`, 51). Both are implemented in
[`kernel/feature/ptctl.c`](../kernel/feature/ptctl.c) and
[`kernel/feature/uhook.c`](../kernel/feature/uhook.c), and both are declared in
[`uapi/supercall.h`](../uapi/supercall.h), which is the normative contract; this document
explains how to drive them and what the results mean.

The two are complementary. **ptctl** is interactive and keyed on a pid or tid: attach to a
task that is already running, stop a thread, read and write its memory and registers, then
let it go. It leaves `TracerPid` at 0 and changes no instruction bytes, so a target that
ptraces itself to lock out debuggers, or checksums its own text, notices nothing. **uhook**
is persistent and keyed on `(file inode, file offset)`: place a probe on an instruction in a
library and it fires for every thread that reaches it, across [`execve`][execve-2], immune
to ASLR, with an in-kernel condition and action so no userspace round trip is needed per
hit. It pays for that with a modified instruction byte in the target's private copy of its
text.

Nothing in `userspace/` or `manager/` issues either command. They are reachable only from a
root-owned tool you build against the UAPI header, which is deliberate: neither has a
policy model beyond `only_root`.

## Reaching the commands

Both ride the same `[ksu_driver]` descriptor as every other supercall. A tool that does not
already hold one asks the kernel for it through the [`reboot`][reboot-2] magic, which a
[kprobe][kprobes] on `__arm64_sys_reboot` intercepts:

```c
#include "uapi/supercall.h"

static int ksu_open(void)
{
    int fd = -1;
    syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd);
    return fd; /* -1 if the module is not loaded */
}
```

The kprobe is a `pre_handler` on the syscall entry wrapper, so it runs before the syscall
body checks `CAP_SYS_BOOT`. Any process can therefore obtain the descriptor; what it can do
with it is decided per command. `PTCTL` and `UHOOK` are both `only_root`, which in
[`supercall/perm.c`](../kernel/supercall/perm.c) is the bare test `current_uid().val == 0` --
not "the manager", not "ksud". Any uid-0 context on the device has full cross-process memory
and register access through these calls. Treat that as the security model, because it is.

Both handlers copy the command struct back to userspace **unconditionally**, including on
the error paths, so output fields such as `ret`, `arg1` and `arg2` are still worth reading
after a failed call:

```c
static long ksu_ptctl(int fd, struct ksu_ptctl_cmd *c)
{
    return ioctl(fd, KSU_IOCTL_PTCTL, c);   /* c is updated either way */
}
```

Every 64-bit field in both structs is `__aligned_u64` or `__aligned_s64`. That is not
decoration: i386 aligns `__u64` to 4 bytes while every 64-bit ABI aligns it to 8, and the
driver points `.compat_ioctl` at the same handler as `.unlocked_ioctl`, so forcing the
alignment is what keeps one struct layout for a 32-bit and a 64-bit caller. Build your tool
against this header rather than retyping the structs.

## The tracing substrate

Neither of these features invents its own way into a running process. Both are assembled
from facilities the Linux kernel already provides for tracing, and almost every constraint
in the reference below is inherited from one of them rather than chosen. `KILLGUARD` is a
kprobe, the hold breakpoint is a perf hardware breakpoint, uhook is a
[uprobe][uprobetracer], and the syscall interception elsewhere in the module is a
[tracepoint][tracepoints] plus a patched dispatch table. Knowing how those four work, and
what each costs, is what makes the restrictions read as consequences instead of arbitrary
rules.

This section is background. If you only want the verb tables, skip to [ptctl](#ptctl).

### What a probe has to solve

Every instrumentation mechanism answers the same two questions: how does control get from
the instrumented code to your handler, and what does the mechanism cost when nobody is
looking. The second question decides whether a facility can ship enabled; the first decides
what your handler is allowed to do when it runs.

Mechanisms divide along two axes. **Static** instrumentation is compiled in: a developer
placed the probe site deliberately, it has a name and a documented payload, and it survives
kernel upgrades. **Dynamic** instrumentation is imposed from outside on code that knows
nothing about it: any instruction can become a probe site, nothing is stable, and a rename
upstream silently breaks you. Orthogonally, a mechanism instruments either **kernel** or
**user** text, and the two differ sharply in cost, because reaching user text means touching
an address space that is paged, shared between processes, and copy-on-write.

| | Kernel | User |
| --- | --- | --- |
| Static | [tracepoints][tracepoints] (1,441 declared under `include/trace/events/` in the 6.1 tree this builds against) | USDT / SDT notes |
| Dynamic | [kprobes][kprobes] and [kretprobes][kprobes] (any of ~50,000 functions) | [uprobes][uprobetracer] and [uretprobes][uprobetracer] |
| Hardware | PMU counters, `perf_events` | debug registers (breakpoints and watchpoints) |

The rule of thumb that follows is Gregg's: prefer a tracepoint when one exists and answers
your question, and fall back to a dynamic probe only when it does not. A tracepoint is
cheaper, is documented, and will still be there next release. This fork does exactly that
where it can -- the syscall entry path rides the `sys_enter` tracepoint -- and reaches for
kprobes and uprobes only where no static site exists, which for "intercept a signal being
delivered to an arbitrary process" is always.

### The cost model

Overhead is worth making quantitative before discussing mechanisms, because it explains
several design decisions later. Gregg reduces it to one relationship: cost is the event
frequency multiplied by the work done per event, divided across the CPUs available to
absorb it. The number of CPUs and the per-event work each vary by about one order of
magnitude. Event frequency varies by *seven*, which makes it the term that decides
everything.

His scale of typical rates is worth internalising. Process execution runs at tens per
second; file opens at tens; VFS calls at thousands to tens of thousands; syscalls at
thousands to fifty thousand; memory allocations from ten thousand to a million; locking
events up to five million; function calls up to a hundred million. Attaching the same probe
to `execve` and to `malloc` are not the same activity by any useful measure. His scaled
version of the table is the memorable one: if one event per second is one email per year,
then a million per second is one every thirty seconds.

The per-event side has been measured directly. Timing a `dd` workload driving over a
million reads per second, with the same trivial action attached through different
mechanisms, gives:

| Mechanism | Cost per event |
| --- | --- |
| kprobe | ~76 ns |
| tracepoint | ~93-96 ns |
| [kretprobe][kprobes] | ~212 ns |
| uprobe | ~1,287 ns |
| [uretprobe][uprobetracer] | ~1,931 ns |

Two things in that table matter here. First, a kretprobe costs roughly three times a
kprobe, because a return probe has to allocate a record to hold the hijacked return address
and free it later. Second, and much more important, **a uprobe costs about seventeen times
a kprobe, and a uretprobe about twenty-five times**. That is not an implementation defect;
it is the price of crossing the address-space boundary, and it is why the same author
reports tenfold or worse application slowdowns when probing something as hot as `malloc()`
and `free()`.

For uhook the practical consequence is direct. Evaluating a condition inside the handler
costs a register compare on top of a trap the hook has already paid for. Draining every hit
to userspace and filtering there costs the same trap, plus a copy, plus a syscall per batch.
Put the condition in the hook. The same reasoning explains why `filter_tgid` restricts
*placement* and not just the action: a hook that never gets planted in the other three
hundred processes mapping libc costs those processes nothing at all.

### Static instrumentation, and why a disabled tracepoint is free

A tracepoint is declared with `TRACE_EVENT()` in `include/trace/events/`, which expands into
a probe site, a metadata structure describing the payload, a registration function, and a
format file that later appears under `/sys/kernel/tracing/events/`. The interesting part is
the site. Expanded, it is guarded by

```c
if (static_key_false(&__tracepoint_##name.key))
        __DO_TRACE(name, TP_ARGS(args), ...);
```

`static_key_false()` is a *[jump label][static-keys]*, not a branch on a variable. The
compiler emits a NOP -- five bytes on x86_64, four on arm64, where every instruction is four
-- and records its address in a `__jump_table` section along with the branch target.
Enabling the tracepoint patches that NOP into an unconditional branch to the out-of-line
call, using the architecture's text-patching path: `text_poke_bp()` on x86_64,
`aarch64_insn_patch_text_nosync()` on arm64. Disabling patches it back.

The consequence is the row in the comparison that matters: a disabled tracepoint costs a NOP
and some bytes of metadata, not a load and a test. That is why the kernel can afford to
carry more than a thousand of them in a production build. It is also why enabling one is not
free in a different sense -- it is a live text modification, and it needs the same machinery
any other patch does.

The registration side has a discipline worth understanding, because this fork depends on it.
Probes attached to a tracepoint are held in an RCU-protected array, and the call site
iterates it under `rcu_read_lock_sched_notrace()`. Unregistering removes your callback from
the array, but a CPU may already be executing it. `tracepoint_synchronize_unregister()`,
which is `synchronize_rcu()` with tracing-specific caveats, is the barrier that says every
in-flight call has drained. Skip it and you free the module holding the callback while
another CPU is inside it. That call is exactly why `ksu_syscall_hook_manager_exit()` in this
tree unregisters the `sys_enter` handler and then waits before touching anything else -- the
teardown ordering in `kernelsu_exit()` is not defensive style, it is this contract.

The payload is the other half of the bargain. A tracepoint hands your callback the arguments
the author chose to expose, already extracted, at a point in the function the author
considered semantically meaningful. A kprobe hands you a `struct pt_regs` at an instruction
boundary and leaves you to work out the calling convention yourself. This fork pays that
difference in `hook/syscall_event_bridge.c`, where every handler must know which register
carries which syscall argument on which architecture, because a raw register frame is all a
dynamic probe can give.

### kprobes: a breakpoint, an out-of-line copy, and a resume

#### The problem

We want a callback on an arbitrary kernel instruction: when execution reaches this address,
run my function first, let it read or change the register frame, then carry on as though
nothing had happened.

The difficulty is that the kernel is already compiled. There is no source-level probe point
to enable, so the only thing available is to **replace the instruction**. And once it is
replaced, its original effect has to be delivered anyway, exactly, or the kernel dies.

Everything else in kprobes is an answer to one question: having replaced the instruction,
how do you put it back?

#### Replacing it with a breakpoint

The instruction at the target address is overwritten with a breakpoint: `int3` on x86_64,
`BRK #KPROBES_BRK_IMM` (immediate `0x004`, `arch/arm64/include/asm/brk-imm.h`) on arm64.
Concretely, on arm64:

```text
before   ffff800008123456:  d10083ff    sub  sp, sp, #0x20
after    ffff800008123456:  d4200080    brk  #0x4
```

The original four bytes are saved first. From then on, a CPU reaching this address raises a
debug exception; the handler recognises the breakpoint as one of kprobes' own and calls the
registered `pre_handler`. Unregistering writes the saved bytes back.

#### The hard part: executing the instruction you replaced

This is the piece most descriptions skip, and it is where the design earns its complexity.

After `pre_handler` returns, that `sub sp, sp, #0x20` still has to happen. The obvious
approach is to restore the original bytes, execute them, and put the breakpoint back. **On
an SMP system that is wrong.** During the window where the original instruction is in place,
a thread on another CPU can reach the same address and sail straight through, missing the
probe. Narrowing the window does not fix it.

The kernel instead executes the instruction somewhere else entirely, a technique called
**execute out of line** (XOL):

1. At registration, the original instruction is copied into a small per-probe buffer.
2. On a hit, after `pre_handler` runs, the CPU is pointed at that buffer and single-steps
   the copy there -- single-stepping meaning the processor executes exactly one instruction
   and then traps again, so the kernel regains control immediately afterwards.
3. Control then returns to the instruction following the probe site.

The breakpoint at the original address is never removed, so no other CPU can slip past. On
arm64 the buffer is `MAX_INSN_SIZE` (2) instructions wide: the copy plus a branch back.

**Not every instruction can be relocated.** A PC-relative branch computes its target from
the address it executes at, so running the copy from a different address computes a
different target. `arch_prepare_kprobe()` therefore inspects the instruction at registration
and takes one of three routes: copy it to the buffer, arrange for the kernel to *simulate*
it rather than execute it, or refuse the probe. This is why the same probe can register on
one kernel and fail on another -- usually nothing about kprobes changed, only the
instruction that happens to sit at that offset.

#### What a handler is allowed to do

`pre_handler` runs in exception context, which imposes two hard constraints.

**It cannot sleep.** Preemption is disabled while it runs, and on some paths interrupts are
too. No sleeping, no mutexes, no touching user memory that might fault.

**It has to survive reentry.** If a function that kprobes itself calls carries a probe, the
result is unbounded recursion. Two mechanisms prevent it. Per-CPU state -- `current_kprobe`
plus a `kprobe_ctlblk` holding a small state machine with `KPROBE_HIT_ACTIVE` and
`KPROBE_REENTER` -- detects a probe hit taken *inside* another probe's handler; when the
machinery cannot service the hit it increments the probe's `nmissed` counter and lets the
event go. And a blacklist keeps probes off the low-level code that would recurse, which is
what the `NOKPROBE_SYMBOL()` annotations scattered through architecture code declare;
`toggle_bp_registers()` in `arch/arm64/kernel/hw_breakpoint.c` carries one.

This is exactly why this fork's probe handlers do almost nothing. `KILLGUARD`'s handler
inspects arguments and redirects control flow. The hold breakpoint's overflow handler takes
a snapshot, takes a module reference, and queues `task_work` with `TWA_RESUME`. The real
work happens in that callback, which runs on the way back to userspace, where sleeping is
legal again.

#### Writing to kernel text at all

Replacing an instruction sounds like a memory write. It is four separate problems.

**Kernel text is read-only.** Late in boot, `mark_rodata_ro()` (`init/main.c`, under
`CONFIG_STRICT_KERNEL_RWX`) turns the kernel's text mappings read-only and executable, on
purpose: kernel code should not be writable at runtime. Patching means getting around a
defence the kernel raised against itself.

**A writable alias gets around it.** Read-only is a property of the *page table entry*, not
of the physical memory. The same physical page can be mapped twice -- once read-only and
executable, which is the mapping the CPU executes from, and once writable, which is the
mapping the patcher writes through. Writing the second changes the same bytes without ever
weakening the first. `patch_map()` in `arch/arm64/kernel/patching.c` builds that alias, and
only when one is needed:

| Target | Page obtained by | Alias needed |
| --- | --- | --- |
| kernel image text | `phys_to_page(__pa_symbol(addr))` | yes |
| module text, with `CONFIG_STRICT_MODULE_RWX` | `vmalloc_to_page(addr)` | yes |
| anything else | -- | no, the address is returned unchanged |

The alias needs a virtual address, and it cannot come from `vmalloc()` or `ioremap()`,
because those allocate and may sleep while patching may run in atomic context. It comes from
the **fixmap**: a small set of page-sized virtual slots the kernel reserves at compile time,
numbered by an `enum fixed_addresses` in `arch/arm64/include/asm/fixmap.h`, of which
`FIX_TEXT_POKE0` is one. A fixmap slot has a constant virtual address and can be pointed at
any physical page at runtime, with no allocation:

```c
waddr = set_fixmap_offset(FIX_TEXT_POKE0, page_to_phys(page) + offset_in_page);
/* waddr is now a writable alias of the instruction to patch */
```

**Two locks, at two different levels.** `__aarch64_text_write()` takes `patch_lock`, a raw
spinlock, with `raw_spin_lock_irqsave` so interrupts are off as well. That protects the
fixmap slot itself: there is one `FIX_TEXT_POKE0` in the system, and two CPUs using it at
once would corrupt each other. Interrupts are disabled because an interrupt handler can also
reach the patching path, and would otherwise deadlock against itself. Separately, the
generic layer holds `text_mutex` (`mutex_lock(&text_mutex)` in `kernel/kprobes.c`;
[ftrace][ftrace] and jump labels do the same) so that only one subsystem is modifying kernel
text at a time. The mutex decides whose turn it is to patch; the spinlock decides whose turn
it is to use the slot. The write itself is `copy_to_kernel_nofault()` rather than
`memcpy()`, so a bad address returns an error instead of panicking.

**The caches are still holding the old instruction.** On arm64 the instruction and data
caches are not coherent with each other. The patch was a *data* write, so it landed in the
D-cache, while the CPU fetches instructions through the I-cache, which may still hold the
old bytes. `caches_clean_inval_pou()` fixes this in two steps: clean the affected D-cache
range to the **Point of Unification** -- the level at which a core's instruction fetches,
data accesses and page-table walks all observe the same copy -- then invalidate the
corresponding I-cache range so the next fetch has to go back to that level. The function
name is those three things: clean, invalidate, to the PoU. This is ordinary kernel
[cache maintenance][cachetlb], just applied to code rather than data.

#### The one problem cache maintenance does not solve

Memory and caches now hold the new instruction. Another CPU may nonetheless have fetched the
*old* one into its pipeline already. Nothing you do to the caches reaches that.

There is no cheap fix, so arm64 offers two variants:

| Function | What it does |
| --- | --- |
| `aarch64_insn_patch_text_nosync()` | write, then [cache maintenance][cachetlb], ignoring other CPUs |
| `aarch64_insn_patch_text()` | the same, inside `stop_machine()` |

The `nosync` variant is safe when the caller can argue it is. kprobes plants a single
4-byte aligned breakpoint: the store is atomic at that width, so another CPU sees either the
old instruction or the `BRK`, and **both are legal things to execute**. There is no state in
which it can fetch half an instruction.

When that argument does not hold, the patch runs under `stop_machine()`, and it is worth
seeing how that converges. Every online CPU is herded into
`aarch64_insn_patch_text_cb()`. The *last* one to arrive elects itself master --
`atomic_inc_return(&pp->cpu_count) == num_online_cpus()` -- and performs the writes while
the others spin on the counter. When it bumps the counter again, each of the others executes
an `isb()`, an instruction synchronisation barrier, which discards anything already
prefetched. Atomicity here is not achieved by a clever store. It is achieved by stopping
every CPU.

x86_64 reaches the same goal from the other direction, because its instructions are
variable-length and a five-byte `jmp` cannot be stored atomically at all. `text_poke_bp()`
patches in three steps: write only the first byte as `int3`, which is a single byte and so
atomic, at which point any CPU executing there traps into `poke_int3_handler()` and has the
new instruction emulated for it; write the remaining bytes, which nobody can be executing
because the first byte is a trap; then replace the first byte with the real opcode. Between
steps it calls `text_poke_sync()`, which is `on_each_cpu(do_sync_core, NULL, 1)` -- an IPI
that forces every CPU to execute a serialising instruction and observe the current step.

x86 covers the window with a trap; arm64 closes it by stopping the machine. Both exist
because a multi-byte store is not atomic with respect to another CPU's instruction fetch.
This fork's `hook/arm64/patch_memory.c` implements the same fixmap-alias sequence, for the
same reason: the code it modifies is mapped read-only too.

#### Two optimizations

The first removes the exception. Where the kernel can prove it safe, the breakpoint is
replaced by a **jump** to a generated trampoline, so a hit costs a branch instead of a trap.
This is `CONFIG_OPTPROBES`, which is `def_bool y` with no prompt whenever the architecture
selects `HAVE_OPTPROBES` -- x86, arm and powerpc do; **arm64 does not**.

That asymmetry is load-bearing here. An optimized probe's trampoline calls
`opt_pre_handler()`, which discards the `pre_handler` return value and does not honour writes
to `pc` or `sp`. A probe that works by redirecting control flow therefore degrades silently
into a no-op on x86_64, a few jiffies after registration, once the optimizer thread runs.
The documented way to refuse optimization is to supply a non-NULL `post_handler`, which is
why `KILLGUARD` carries a dummy one. Someone who tested only on arm64 would never see this.

The second removes the breakpoint. If the probe sits exactly at a function entry that
ftrace already owns, the kprobe registers as an ftrace client instead of patching
anything, and the cost drops to the call the function already makes. That is
`KPROBES_ON_FTRACE`. On arm64 this tree compiles with `-fpatchable-function-entry=2`
(`arch/arm64/Makefile`), reserving two instruction slots at every function entry; x86_64 uses
`-mfentry`. The slots hold NOPs until something needs them. Whether a given probe takes this
path is decided by the kernel, not the caller, which is one reason measured kprobe overhead
varies between kernels.

#### kretprobes: instrumenting the return

A function has many return sites and they are not statically known, so a return probe cannot
work by patching one. It hijacks the return address instead:

1. An ordinary kprobe is planted at the function entry.
2. On a hit, the real return address is saved and replaced with the address of a trampoline.
3. When the function returns, the CPU lands in the trampoline, which runs the return handler.
4. The trampoline then jumps to the saved address.

Saving requires a record per *in-flight call*, which is where the extra ~136 ns over a plain
kprobe goes, and why a kretprobe has a `maxactive` bound and can miss hits under recursion
or deep concurrency. Linux 6.1 generalises the mechanism into `rethook`
(`kernel/trace/rethook.c`, `CONFIG_KRETPROBE_ON_RETHOOK`) so kretprobes, fprobe and
[BPF][bpf] share one return-hooking implementation.

`mem_spoof.c` in this fork is a kretprobe user: it lets `si_meminfo()` run and rewrites the
`struct sysinfo` on the way out. That is the canonical shape for a return probe -- read the
result, do not reimplement the function.

#### What actually goes wrong in practice

None of the above is the usual failure. The usual failure is frequency, exactly as the cost
model predicts. `KILLGUARD` probes `do_send_sig_info()`, so while it is armed every signal
delivery on the device takes a debug exception and a single step. That is why the module
registers it lazily on first use rather than at init, and it is visible in
`/sys/kernel/debug/kprobes/list` for as long as it is there.

### uprobes: the same idea across the address-space boundary

A uprobe applies the breakpoint trick to user text, and every difference from kprobes
follows from the fact that user text is a file mapping shared between processes.

#### Keyed by file, not by address

A uprobe is keyed by `(inode, file offset)`, not by an address. Registered probes live in a
single global red-black tree, `uprobes_tree` in `kernel/events/uprobes.c`, serialised by
`uprobes_treelock` and ordered on exactly that pair. Keying this way is what makes a uprobe
immune to ASLR -- the offset within the file does not move when the mapping does -- and what
makes it apply to every process mapping the file, including processes that have not started
yet. Registration walks every VMA currently mapping the inode (`register_for_each_vma()`)
and consults a per-consumer filter for each candidate address space; later
[`mmap()`][mmap-2] calls of the same file consult the same filter through `uprobe_mmap()`. A
hook installed while the target is not running still lands when it starts.

#### The write lands in a private copy

The write itself is `uprobe_write_opcode()`, which uses `FOLL_FORCE` and `__replace_page()`
to install `UPROBE_SWBP_INSN` -- on arm64 `BRK #UPROBES_BRK_IMM`, immediate `0x005`, four
bytes. The page it writes is a **private copy**: the file's page cache is untouched, and so
is the file on disk. This is the most important fact for anyone using uhook against a
defended target, and it cuts both ways. A checksum the process computes by re-reading its
own file sees nothing. A checksum it computes over its own mapping -- through
`/proc/self/mem`, or by simply reading its own `.text` -- sees the `BRK`.

#### Per-mm state, and what fork does to it

Because the page is private to the address space, the effect is per-mm, and the kernel
tracks it with two `mm_struct` flags: `MMF_HAS_UPROBES` says this mm may contain a patched
page, and `MMF_RECALC_UPROBES` says that belief may now be stale and should be recomputed at
the next opportunity. `MMF_HAS_UPROBES` is propagated by `uprobe_dup_mmap()` on fork, which
is why a forked child traps at a probe its parent's filter approved and its own never saw.
uhook gates the action a second time inside the handler for exactly this reason; the filter
alone is not sufficient.

#### The XOL area

Because the instruction is replaced rather than removed, it must still be executed
eventually, and the same SMP problem as kprobes applies -- with an extra wrinkle. The
kernel's own out-of-line buffer is in kernel memory, which the target thread cannot execute
from; the copy has to live somewhere the *user* process can run it. The kernel therefore
maps an **XOL area** into the target -- a dedicated VMA, one page, carved into slots tracked
by a bitmap (`UINSNS_PER_PAGE`, `xol_add_vma()`). On a hit the original instruction is
copied into a slot, `pc` is pointed at it, and the thread single-steps there before being
sent back. Some instructions never need the trip: `arch_uprobe_skip_sstep()` simulates the
ones the architecture can emulate, which on arm64 includes the common branch forms, and
returns directly. Others are refused at registration by `arch_uprobe_analyze_insn()`, which
is why a uprobe on an arbitrary offset can fail with `-EINVAL` on one build of a library and
succeed on the next.

#### Why the control-flow verbs are return-site only

That mechanism is exactly why uhook rejects the control-flow verbs at an entry site. A
handler that writes `pc` is writing a value that `arch_uprobe_pre_xol()` is about to
overwrite with the XOL slot address, and that `arch_uprobe_post_xol()` will then reset to
"probed address plus instruction length". At a simulated branch it is worse than useless:
the simulator re-reads `pc` after the handler and treats the corrupted value as the branch
base, producing a jump to `target + disp`. None of that is uhook being conservative; it is
the uprobe core doing what it has always done, and no consumer can opt out.

#### Return probes

A **uretprobe** hijacks the return address the same way a kretprobe does, into a trampoline
that also lives in the XOL area, with a per-thread stack of `return_instance` records. On
arm64 the hijack takes `x30` rather than a stack slot, which is the whole reason a
return-site hook must be placed on a function's *first* instruction: anywhere else, the link
register holds whatever the function has already done with it. The return path is also the
only place where a handler-set `pc` survives, because `handle_trampoline()` sets `pc` from
the saved `orig_ret_vaddr` before running the return handlers and never touches it again.
That single asymmetry is the entire reason uhook's action table has a "where it is allowed"
column.

#### The cost, and the tells

The last difference is the one the cost table already made: roughly 1.3 microseconds per
entry hit and 1.9 per return hit, against 76 nanoseconds for the kernel-side equivalent.
Every hit is a full exception into the kernel, a handler, and a single step back out. Two
further tells follow from the XOL area and the COW write rather than from anything this fork
does: once any probe fires, `/proc/<pid>/maps` grows a `[uprobes]` entry for that VMA, and
`/proc/<pid>/smaps` reports a non-zero `Anonymous` for an `r-xp` file mapping that on a
clean process has none. `Anonymous` only: the COW'd text page is accounted clean, measured
on android14-6.1 as 4 kB `Anonymous` and 4 kB `Private_Clean` against 0 kB `Private_Dirty`.

### User static tracing, and why it is absent here

The user-space counterpart to a tracepoint is USDT -- userland statically defined tracing,
inherited from DTrace and carried in ELF as `.note.stapsdt` records. A developer places a
`DTRACE_PROBE()` macro in the source; the compiler emits a NOP at that point and a note
describing the probe's name, its argument locations as register or memory expressions, and
optionally the address of a *semaphore*, a counter the tracer increments so the application
can cheaply skip work when nobody is watching.

For a tracer the important part is that USDT is not a separate mechanism. The note gives a
file offset; instrumenting it means placing a uprobe there. USDT buys a stable name and a
documented argument layout, not a cheaper trap -- it costs the same ~1.3 microseconds a
uprobe costs, because it *is* a uprobe. Libraries built with `--enable-systemtap` carry
useful ones; glibc, PostgreSQL, the JVM and Node all ship them.

This matters here mainly by its absence. Android system libraries are not built with SDT
notes, and neither is an app's own native code. There is nothing named to attach to, which
is why uhook's addressing is a raw file offset and why the burden of finding the right
instruction falls entirely on the caller. Anyone building tooling on top of uhook is
building the symbol resolution that USDT would otherwise have provided.

### Stack traces, and what a probe can afford to collect

A probe fires at a point; understanding why it fired usually requires the path that led
there. Collecting that path is harder than it looks, and the reasons bear on what uhook's
capture ring stores.

The classic technique is the **frame pointer** walk. By convention a register (`rbp` on
x86_64, `x29` on arm64) holds the head of a linked list of stack frames, with the return
address at a known offset. A tracer that interrupts a thread can follow that chain with a
few loads. The catch is that the convention is optional: compilers omit the frame pointer to
free a register and save prologue instructions, and gcc has defaulted to omitting it on
x86_64 for years. Gregg's assessment is that the win is usually under one percent, often
unmeasurable, and that Netflix reinstated frame pointers across many services because
profile quality was worth more.

The alternative is unwind metadata. **DWARF** `.eh_frame` and `.debug_frame` describe how to
recover the caller's state at any instruction, which is complete and correct and also
processor-intensive and stored in sections that may not be resident. That is unacceptable in
a probe handler that runs with interrupts disabled and cannot fault. The kernel therefore
grew **ORC**, a deliberately simpler format with its own `.orc_unwind` sections, used by
`perf_callchain_kernel()`; it applies to kernel stacks only. Intel's **LBR** records the last
handful of branches in hardware, which costs nothing but is bounded to something like 4 to
32 entries.

Three practical consequences. First, a kernel stack is usually obtainable in a probe; a user
stack often is not, and when it is, it costs -- the measured cost of collecting a user stack
was roughly double that of a kernel stack. Second, addresses are captured raw and symbolised
later in userspace, so a mapping that changed in between produces wrong answers. Third, and
the reason this section is here: uhook's capture record stores 34 register values and a
timestamp, and no stack. That is a deliberate trade. Registers are already in `pt_regs` and
cost nothing to copy; a user stack walk in a uprobe handler would multiply an already
expensive probe. If you need the caller, hook the caller.

### Hardware assistance: the PMU and the debug registers

Everything above modifies instructions. The processor also offers instrumentation that
modifies nothing, through two distinct facilities Linux exposes behind one API,
[`perf_event_open()`][perf-event-open-2].

The **PMU** provides programmable counters for micro-architectural events: cycles,
instructions retired, cache references and misses, branch mispredictions. Intel designates
seven as an "architectural set" discoverable through `CPUID`. They work in two modes.
Counting accumulates, and the kernel reads the counter when it likes, at essentially zero
cost. Overflow sampling programs the counter to interrupt after N events, turning a hardware
event into something you can attach a handler to. Sampling has an accuracy problem: the
instruction pointer recorded at the interrupt is not necessarily the one that caused the
event, because of interrupt latency and out-of-order execution -- a discrepancy known as
skid. Intel's PEBS uses hardware buffers to record the true pointer. Without it, profilers
choose deliberately odd frequencies such as 99 Hz to avoid sampling in lockstep with
periodic activity. Counters are also scarce: there may be hundreds of countable events but
only a handful of registers, so `perf` multiplexes and scales the results, and many cloud
hypervisors do not expose the PMU to guests at all.

**Debug registers** are the facility ptctl's hold breakpoint uses, and they are a different
thing entirely. A hardware breakpoint is an address programmed into a dedicated register,
with a control register saying whether it matches on execute, read or write, and how wide.
When the address is accessed, the processor traps. Nothing in memory changes, which is the
entire point: there is no instruction to checksum, no page to go copy-on-write, and no VMA
to appear in `maps`. The same registers serve watchpoints, which trap on data access rather
than execution and are the reason "hardware breakpoint" and "watchpoint" share an
implementation in Linux.

They are also desperately scarce and thread-local. x86_64 has four, `DR0` through `DR3`,
governed by `DR7` (`HBP_NUM` is 4 in `arch/x86/include/asm/hw_breakpoint.h`). arm64's count
is implementation-defined, read at boot from `ID_AA64DFR0_EL1` into `core_num_brps`; six is
common. The registers are per-CPU state that must be saved and restored around a context
switch: `arch/arm64/kernel/hw_breakpoint.c` keeps a per-CPU array `bp_on_reg[ARM_MAX_BRP]`
of the events currently occupying slots, installs and removes them through
`hw_breakpoint_control()`, and reconciles them on `__switch_to` via
`hw_breakpoint_thread_switch()`. Because the state is per-thread, arming an address across a
thread group means registering one perf event per thread -- which is why ptctl caps that at
640, and why arming is a loop that can partially fail.

Linux models a hardware breakpoint as a `perf_event` with an overflow handler, and that is
where the arm64 subtlety in this fork comes from. `breakpoint_handler()` performs the
step-past-the-breakpoint dance -- clear the breakpoint enable via `toggle_bp_registers()`,
set `PSTATE.SS` (`DBG_SPSR_SS`, bit 21) so the thread executes exactly one instruction,
restore afterwards -- only when the event uses the *default* overflow handler. An event with
a custom handler is assumed to be managing its own resumption, so the register stays armed,
no step is set up, and the thread returns to the same instruction and traps again,
indefinitely. Any code that installs its own breakpoint handler on arm64 must solve that,
and the two ways to do it are the two this fork uses across kernel versions: flip the gate
by planting the default handler's address in a field the check consults, or perform the
sequence by hand.

### The overflow path, and what the default handler actually does

The hardware breakpoint gate deserves one more level of detail, because the fix in this fork
looks like a hack until you see the shape of the code it is working with.

When a perf event fires -- a counter overflows, or a breakpoint matches -- the core arrives at
`__perf_event_overflow()`. That function does the bookkeeping common to all events: it
applies throttling, so an event that fires more often than the configured limit is
temporarily disabled rather than melting the CPU; it adjusts the sampling period for
frequency-targeted events; and it then calls the event's `overflow_handler`.

For an event created from userspace through `perf_event_open()`, that handler is
`perf_event_output_forward()`, which formats a sample and writes it into the ring buffer the
process has mapped. That ring is the transport for all of `perf record`: a head and tail
index in a shared page, records appended by the kernel and consumed by userspace, with lost
counts recorded when the consumer falls behind. It is also the mechanism BPF programs use to
push data out, through `bpf_perf_event_output()`.

For an event created *inside* the kernel -- `register_user_hw_breakpoint()` and friends --
the creator supplies its own handler instead, and no ring buffer exists. That is the case
ptctl is in.

`is_default_overflow_handler()` in `include/linux/perf_event.h` is nothing more than a
pointer comparison against `perf_event_output_forward`. Architecture code uses it to ask "is
anyone actually consuming this event, or is a kernel component managing it?" and arm64's
`breakpoint_handler()` uses the answer to decide whether it owns the resumption. Once you
know the test is pointer identity against a symbol that is merely the *default sink*, the
technique this fork uses on older kernels reads differently: planting that address in the
field the check consults flips the gate without changing which handler actually runs,
because the field consulted by the check and the field invoked by `__perf_event_overflow()`
are not the same field. Nothing calls the planted pointer. On v6.12 and later that second
field is gone, the test collapses to the single remaining one, and the only honest option is
to perform the resumption sequence by hand.

### ftrace, and BPF

Two facilities complete the picture even though this fork uses neither directly.

**ftrace** is the function tracer built on the compiler-reserved entry slots described
earlier. Its value is that the call site already exists: enabling function tracing patches
NOPs into calls to `ftrace_caller`, and registered `ftrace_ops` structures filter by
function. The flags on those ops describe what a client needs and what it may do --
`FTRACE_OPS_FL_SAVE_REGS` asks for a full `pt_regs`, `FTRACE_OPS_FL_IPMODIFY` declares that
the handler may change the return address, and `FTRACE_OPS_FL_DIRECT` requests a direct call
to a trampoline. The `IPMODIFY` flag is the interesting one: the kernel refuses to let two
clients that both want to redirect execution attach to the same function, because there is
no sane way to compose them. Livepatch and BPF trampolines both rely on it.

**BPF** is not an event source at all; it is a safe execution environment that attaches to
the event sources above. Its lineage is worth knowing, because it explains the shape.
Classic BPF, from the 1992 BSD packet filter paper, was a two-register 32-bit machine with
sixteen scratch slots, designed so a userspace expression could be verified and then run in
the kernel without copying every packet out. Extended BPF, merged over 2014, kept the
verification model and replaced everything else: ten 64-bit registers plus a read-only frame
pointer, 512 bytes of stack, unbounded "map" storage, and a call instruction that can reach
a restricted set of kernel helpers. The `bpf(2)` syscall arrived in 3.18; attachment to
kprobes, uprobes, tracepoints and perf events followed through the 4.x series.

The verifier is what makes it deployable. It walks the program graph and rejects anything it
cannot prove safe -- unbounded loops, unchecked pointer arithmetic, reads outside a verified
bound -- so a program either cannot run or cannot corrupt the kernel. A JIT then compiles it
to native instructions, so the "virtual machine" framing is misleading: on a modern
configuration the code runs directly on the processor. The efficiency argument is equally
important. A histogram computed in kernel context and read out as a summary transfers
kilobytes; the same histogram computed in userspace requires shipping every event, which is
often the difference between a tool that can run in production and one that cannot.

This fork does not use it, for two reasons worth stating plainly. The first is capability.
BPF is constrained by construction, and "write these bytes into another process's text" is
not something the verifier will ever permit; neither is "suppress this signal and return to
the caller". The primitives here exist precisely because they are outside what a safe
sandbox will grant. The second is footprint. A BPF program is loaded through `bpf(2)`, is
enumerable with `bpftool prog list`, holds file descriptors visible in `/proc/<pid>/fd`, and
is attached through links that are equally enumerable. For an observability tool that
transparency is the point. For code whose purpose is not to be inventoried by the
application it instruments, it is the wrong substrate, and a kernel module resolving its own
symbols through kallsyms is a better fit -- with the corresponding loss of every safety
property the verifier would have provided, which is the trade this fork is making and should
be understood as making.

### seccomp: BPF's other life, and why this fork touches it

BPF's first use outside networking was not tracing but sandboxing, and it is the reason this
fork carries `infra/seccomp_cache.c`.

A [seccomp][seccomp-filter] filter is a classic-BPF program attached to a thread and
inherited across [`fork`][fork-2] and `execve`. On every syscall the kernel runs the filter
over a small structure describing the syscall number, architecture and arguments, and the
program returns a verdict: allow, errno, trap, kill. Android installs such a filter on every
application process from zygote, which is why an app cannot simply issue whatever syscall it
likes.

Evaluating a program on every syscall is not free, so Linux added a cache: for filters whose
verdict for a given syscall number can be determined statically to be "allow" regardless of
arguments, `seccomp_cache_prepare()` precomputes a bitmap, and `seccomp_cache_check_allow()`
short-circuits the whole evaluation on a hit. The bitmaps live in `struct seccomp_filter`,
which is private to `kernel/seccomp.c`.

That cache is what this fork manipulates. The manager reaches KernelSU by calling
[`reboot(2)`][reboot-2] with two magic arguments, and Android's filter does not permit
`reboot` from an app. Rather than modifying the filter program -- which would change a
verdict the application can observe -- `ksu_seccomp_allow_cache()` sets the bit for
`__NR_reboot` in the cache bitmap of the calling thread's filter, so the syscall is admitted
without the program running. The mirror function clears it again. Because `struct
seccomp_filter` is not exported, the file shadow-declares it, with `#if LINUX_VERSION_CODE`
guards for the fields that moved -- a fragile technique whose failure mode is silent
corruption of a neighbouring field, and one of the clearest examples in the tree of what "no
stable ABI" costs.

The wider point is that BPF is a general-purpose in-kernel execution engine with several
independent consumers, of which tracing is only the most visible. Seccomp shows the same
verification-then-execute model applied to policy rather than observation, and the same
tension between the cost of running a program on a hot path and the caching added to avoid
it.

### Control-flow integrity, and why handler prototypes matter

One modern kernel hardening feature deserves mention, because it changes what a mistake in
this area costs. Android GKI kernels are built with Clang's kernel CFI, which assigns each
function pointer a type identity and checks it at every indirect call. Calling through a
pointer whose prototype does not match the callee's traps rather than silently misbehaving.

For code that registers callbacks with kernel subsystems this is a sharp edge. If the
`uprobe_consumer` handler prototypes assumed by a module disagree with the kernel's -- say
because a version added a trailing argument -- the mismatch is not a subtly wrong argument
value; it is a trap at the call. That is why the version boundaries in uhook are worth
getting exactly right rather than approximately, and why functions this module installs into
kernel tables are marked `__nocfi` where they are reached through pointers the kernel does
not know the type of. The failure mode with CFI is loud, which is better than the
alternative, but it is still a failure at runtime rather than at build time.

### Choosing a mechanism

Pulling the preceding sections together, the decision procedure for instrumenting something
in this environment is fairly mechanical.

Ask first whether a **tracepoint** covers the event. If one does, it is cheaper than the
alternatives, it hands you extracted arguments instead of a register frame, and it will
survive a kernel upgrade. The cost of finding out is one `grep` through
`include/trace/events/`.

If not, and the event is a kernel function, use a **kprobe**, and check the frequency before
you do. At tens or hundreds of events per second the overhead is unmeasurable; at hundreds
of thousands it is the dominant cost on the system. If you need the return value rather than
the arguments, a **kretprobe** costs about three times as much and can miss under
concurrency, so prefer reading state at entry when the information is available there.

If the event is in userspace, a **uprobe** is the only general answer, and it costs roughly
seventeen times a kprobe. Everything about how you use it should follow from that number:
filter by process at registration so the probe is never planted where it is not wanted,
evaluate conditions in the handler rather than shipping every hit out, and prefer a single
return-site hook that forges a result over a pair of entry and return hooks that observe
one.

If you need to observe an address without modifying anything -- because the target inspects
its own text, or because you cannot afford to touch a shared page -- the answer is a
**hardware breakpoint**, and you are limited to a handful of addresses per thread and to
arming every thread individually.

If you need the target to keep running normally while you decide, none of these are enough
on their own, which is the case ptctl's hold breakpoint exists for: a mechanism that stops
one thread, at one address, without leaving a mark, and hands you the frame while everything
else proceeds.

### Where ptctl and uhook sit

| Feature | Substrate | Consequence documented below |
| --- | --- | --- |
| `PEEK` / `POKE` | `access_process_vm()`, `FOLL_FORCE` | 64 KiB cap; a COW text write is self-visible |
| `GETREGS` / `SETREGS` | `task_call_func()` pinning | `-EBUSY` unless the target is genuinely off-CPU |
| `KILLGUARD` | kprobe on `do_send_sig_info()` | dummy `post_handler` to refuse optprobe; lazy registration; no sleeping in the handler |
| `HWBP_*` | perf event on the debug registers | nothing in memory changes; a handful of per-thread registers; 640-thread cap |
| uhook `ADD` | uprobe keyed by inode and offset | `BRK` in a COW page; `[uprobes]` VMA; ASLR immunity; applies to future processes |
| uhook `ON_RET` | uretprobe trampoline in the XOL area | must be the function's first instruction on arm64 |
| uhook actions | uprobe handler context | control-flow verbs are `ON_RET` only |
| uhook `cond` | in-handler evaluation | a register compare instead of a ~1.3 us round trip per hit |
| uhook capture | `pt_regs` copy into a ring | registers only, no stack walk |

## ptctl

### The verbs

| Op | Inputs | Output | Notes |
| --- | --- | --- | --- |
| `PEEK` (1) | `pid`, `addr`, `len` <= 64K, `uptr` | `ret` = bytes read | `FOLL_FORCE`, like ptrace |
| `POKE` (2) | `pid`, `addr`, `len` <= 64K, `uptr` | `ret` = bytes written | writes through to text |
| `GETREGS` (3) | `pid` (a tid), `uptr`, `len` = 0 | `ret` = bytes | target must be off-CPU |
| `SETREGS` (4) | `pid` (a tid), `uptr`, `len` = 0 | `ret` = bytes | sanitised like `PTRACE_SETREGSET` |
| `INFO` (5) | `pid` | `arg1` = tracer pid, `arg2` = tgid, `ret` = 1 if it exists | |
| `KILLGUARD` (6) | `pid`, `arg1` = 1 add / 0 remove | `arg2` = tgid guarded | max 32 guarded groups |
| `SIGSEND` (7) | `pid`, `arg1` = signal | | signal 0 is refused; use `INFO` |
| `DETACH_TRACER` (8) | | | reserved, returns `-ENOSYS` |
| `HWBP_SET` (9) | `pid` = tgid, `addr` | `ret` = threads armed, `arg2` = threads in group | one breakpoint, system-wide |
| `HWBP_WAIT` (10) | `arg1` = timeout ms, `uptr`, `len` = 0 | `ret` = 1 on hit, `arg2` = tid, `arg1` = parked | 0 means timeout; `-ENOENT` if nothing armed |
| `HWBP_RELEASE` (11) | | | `-ENOENT` if nothing is held |
| `HWBP_CLEAR` (12) | | `ret` = hits dropped since `SET` | disarms, releasing any held thread |

### Memory

`PEEK` and `POKE` are `access_process_vm()` with `FOLL_FORCE`, capped at 64 KiB per call by
`PTCTL_MAX_CHUNK`. A larger `len` is refused with `-EINVAL` rather than split, so chunking
is the caller's job. `access_process_vm()` reports total failure as a zero byte count rather
than an errno, which the handler translates to `-EIO`, so a short count really is a short
count.

`FOLL_FORCE` on the read side is what lets you read what you can write. `POKE` into a text
page copy-on-writes it and `copy_to_user_page()` flushes the I-cache, so the write takes
effect -- and a checksum the process computes over its own mapping sees it. A checksum that
re-reads the file from disk does not.

### Registers

`GETREGS` and `SETREGS` transfer exactly the user-visible frame: `struct user_pt_regs`
(272 bytes) on arm64, `struct pt_regs` (168 bytes) on x86_64. The kernel-private tail of
arm64's `struct pt_regs` -- `orig_x0`, `syscallno`, `pmr_save`, `stackframe` and the rest --
never crosses the boundary. Pass `len = 0` for "the whole user view"; any other value must
equal that size exactly or you get `-EINVAL`.

Both directions run under `task_call_func()`, which pins the target off-CPU. A task that is
actually running is refused with `-EBUSY`, because its `pt_regs` is rewritten by its next
kernel entry: a write would be lost and a read would be torn. A thread blocked inside a
restartable syscall is refused for writes as well, since `syscall_set_return_value()`
rewrites `x0` on resume and `do_signal()` can rewind `pc` by 4 -- fields outside the writable
view. A thread parked by `HWBP_WAIT` always qualifies, which is the intended way to get a
writable frame.

`SETREGS` sanitises the incoming frame the way the kernel's own `PTRACE_SETREGSET` path
does: `valid_user_regs()` on arm64, and pinning `cs`/`ss`/`orig_ax` plus masking `eflags` on
x86_64. Without that, a forged `SPSR` mode field and an arbitrary `pc` would be `ERET`-ed
straight out of `kernel_exit`, which is a root-to-EL1 escalation rather than a debugging
feature. The call also refuses the calling thread itself.

### KILLGUARD

`KILLGUARD` protects a whole thread group against signals that would terminate it *and* that
another task injects through `do_send_sig_info()`: [`kill(2)`][kill-2],
[`tgkill(2)`][tgkill-2], [`rt_sigqueueinfo(2)`][rt-sigqueueinfo-2],
[`pidfd_send_signal(2)`][pidfd-send-signal-2], `cgroup.kill`. That covers the watchdog case
the verb exists for -- a process whose supervisor kills it when it detects tampering.

It cannot cover, and does not try to cover:

- a synchronous fault. A real `SIGSEGV`, `SIGBUS`, `SIGILL` or `SIGFPE` is delivered through
  `force_sig_info_to_task()` and never enters the probed function;
- `execve`'s `zap_other_threads()`, or seccomp's `do_exit()`;
- the OOM killer. A send with `info == SEND_SIG_PRIV` is deliberately let through, because
  `__oom_kill_process()` ignores the return value and proceeds to `mark_oom_victim()` and
  `queue_oom_reaper()` regardless. Swallowing the signal would leave the reaper unmapping a
  live process and `oom_victims` pinned above zero, which stops the device suspending.

The guarded-signal list is deliberately narrow. bionic and ART use `SIGRTMIN` and friends
for posix timers, GC and thread suspension, so guarding a process must not stop it working.

Two operational notes. The kprobe is registered lazily on first use, because a breakpoint on
`do_send_sig_info` makes every signal send on the system take a debug exception, and it is
visible in `/sys/kernel/debug/kprobes/list`. And the guard table stores bare tgids with no
exit hook, so a guard outlives the process; remove it when you are done, or a later process
that recycles the tgid inherits protection.

### The hold breakpoint

`HWBP_SET` arms an execute breakpoint in the debug registers of every thread of a thread
group (up to `HWBP_MAX`, 640). It arms the threads that exist at that instant and never
follows new ones, and an individual thread whose four debug registers are already spoken
for is skipped, so compare its `ret` (threads armed) against its `arg2` (threads in the
group): a partial arm makes a site only the unarmed threads execute read as a site nothing
executes. Debug registers are not memory, so this is invisible to a
text checksum, to a cross-process watchdog, and to a self-ptrace lock -- perf is not ptrace.
The state is a single global: one address, one held thread, system-wide.

On a hit the thread parks and the waiter is woken. The sequence:

```
HWBP_SET   (pid = tgid, addr = the instruction, 4-byte aligned on arm64)
HWBP_WAIT  (arg1 = timeout ms)        -> ret = 1, arg2 = the tid that hit, uptr = its regs
   PEEK / GETREGS / SETREGS / POKE    on arg2, which is parked and off-CPU
HWBP_RELEASE                          -> the thread resumes past the breakpoint
HWBP_CLEAR                            when finished
```

`HWBP_WAIT` returns only once the hitting thread has genuinely left the runqueue, so the
`SETREGS` you issue next is guaranteed to find it off-CPU rather than bouncing off `-EBUSY`.

The one caveat worth planning around: the park is an interruptible sleep. A signal delivered
to the held thread -- an app's own timer or GC signal will do -- ends the hold early and
indistinguishably from a `HWBP_RELEASE`. After a long inspection, re-arm rather than assume
the thread is still parked.

## uhook

A hook is four orthogonal parts: **where** it sits, **when** it fires, **what** it does, and
what it **captures**. All four are supplied in one `KSU_UHOOK_ADD`, which returns the hook id
in `ret`.

| Op | Purpose |
| --- | --- |
| `ADD` (1) | install a hook; `ret` = new hook id |
| `DEL` (2) | remove hook `id` |
| `CLEAR` (3) | remove every hook |
| `LIST` (4) | `ret` = number of active hooks |
| `READ` (5) | drain the capture ring into `uptr`; `ret` = bytes, `arg1` = records, `lost` = dropped |

### Where

`path` is a userspace pointer to a NUL-terminated file path, resolved to the **real** inode
with `d_real_inode()`. That matters on Android: an overlay inode has no `->read_folio` and
`__uprobe_register()` refuses it with `-EIO`, which would rule out anything under an
overlayed `/system` -- including KernelSU's own module mounts. Resolving through the overlay
to the lower file is what makes those targets hookable at all. The kernel keeps the whole
`struct path` for the hook's lifetime, so the filesystem holding the file cannot be
unmounted until the hook is removed. That is the same contract `trace_uprobe.c` has always
had.

`offset` is a **file** offset -- not an ELF virtual address, and not a runtime address. For a
library mapped straight out of an uncompressed APK, use the APK path and the offset of the
instruction within the APK. On arm64 it must be 4-byte aligned.

`site` is `ON_ENTRY` or `ON_RET`. `ON_RET` additionally requires the offset to be the
function's *first* instruction, because arm64 installs a return probe by hijacking `x30`
rather than a stack slot; a mid-function offset hijacks whatever the link register happens
to hold at that moment.

`filter_tgid` names the process the hook belongs to, as a pid or a tid; the whole thread
group is used. It is **required** -- `ADD` returns `-EINVAL` for 0, and the kernel log says
why. It used to mean "every process that maps the file", and that value was a trap. The
breakpoint landed in zygote and `system_server` too, and because the patched page is
inherited across `fork`, hooking a zygote-mapped library reached every app started
afterwards. One unrelated mapper could also break the hook you wanted, because
`register_for_each_vma()` *assigns* rather than ORs `install_breakpoint()`'s result -- note
the `err |=` on the unregister branch just below it -- abandons the remaining address spaces
after the first failure, and `__uprobe_register()` then unwinds the consumer. That case is at
least loud: the error reaches `uh_add()`, which propagates it, so `ADD` fails. What is silent
is the other one. `uprobe_register()` succeeds whether it planted the breakpoint in fifty
address spaces or in none, so an unscoped hook that armed nowhere listed as active and
produced exactly the silence of a probe on a line the target never executes.
A tgid keeps the breakpoint out of other address spaces entirely through the uprobe filter,
and gates the action again in the handler, because a forked child inherits the patched page
and `MMF_HAS_UPROBES` regardless. `arg1` from `ADD` says how many address spaces currently
map the probed offset within that scope.

Be clear about what 0 was *not*, because it is easy to assume and expensive to assume wrongly:
it was not a gate that matched nothing. Both gates read `if (!h->filter_tgid) return true;`
first and did what the header promised. Measured on a Pixel 6 running 6.1.162, with the ring
drained continuously so nothing could be evicted:

* Ten hooks at once on a library only the target maps, triggered by an activity restart, gave
  **identical** results scoped and unscoped -- 36 records, the same eight of ten offsets firing,
  the same per-offset counts, zero ring drops -- repeated three times alternating between the
  two. The filter changes nothing about whether a hook fires in the process that owns it.
* One hook on `libc.so`'s `open` produced 6996 records in eight seconds unscoped against 8
  scoped. Counting *processes* from ring records is not sound -- a record carries a tid, and a
  short-lived tid can be recycled before you resolve it -- so reach was measured from the
  kernel's own bookkeeping instead: processes newly gaining a `[uprobes]` XOL area. Unscoped
  reached six previously-untouched processes in eight seconds; scoped reached none.

So unscoped fires, and it fires *in the target too*. What it also does is bury it: those 6996
records share one 512-record ring with every other hook, and it drops the oldest. Drain once at
the end, the way an operator does, and the 512 survivors came from six unrelated processes with
**not one** belonging to the target -- whose own hits were real, and evicted. A hook that "produced
nothing" is therefore not evidence about the filter, and not evidence about the target either.

The value is refused because planting a `BRK` system-wide is the wrong thing to have done, and
because it is the one setting in which "armed nowhere" is the ordinary outcome rather than an
edge case: nothing in the request had to name anything that exists. If a hook produces nothing,
check `arg1` and the `LIST` counters before concluding anything about the target -- and check
that nothing else is draining the shared ring.

### When

`cond` is `NONE`, `REG` (compare `regs[cond_reg]`) or `MEM` (compare the `cond_len`-byte
value at `regs[cond_reg] + cond_off`, where `cond_len` is 1, 2, 4 or 8). `cond_cmp` selects
`EQ`, `NE`, `LT`, `GT`, `AND` (a mask test) or the signed pair `SLT` and `SGT` -- the signed
comparisons exist because an unsigned test cannot express "this register holds a negative
value", which is the usual shape of an error return.

Register indices are uniform across the two features. On arm64: `0..30` are `x0..x30`, `31`
is `sp`, `32` is `pc`, `33` is `pstate`. On x86_64: `0` is `ax`, `1..14` are
`bx, cx, dx, si, di, bp, r8..r15`, `31` is `sp`, `32` is `ip`, `33` is `flags`. Index 0 is
the return-value register on both.

### What

| Action | Effect | Where it is allowed |
| --- | --- | --- |
| `OBSERVE` (0) | record registers into the capture ring | anywhere |
| `SETREG` (1) | `regs[act_reg] = act_val` | `pc` (32) only at `ON_RET` |
| `FORCE_RET` (2) | -- | rejected everywhere with `-EOPNOTSUPP` |
| `JUMP` (3) | `pc = act_val` | `ON_RET` only |
| `SKIP` (4) | `pc += act_val` | `ON_RET` only |
| `POKE` (5) | write the ADD-supplied bytes to `*(regs[act_reg]) + act_off` | anywhere |

The site restrictions are enforced at `ADD` time rather than silently misbehaving at
runtime, and they come from the uprobe core, not from this code. At an entry site a
handler-set `pc` is discarded twice -- `arch_uprobe_pre_xol()` points `pc` at the
execute-out-of-line slot, and `arch_uprobe_post_xol()` resets it to the probed address plus
the instruction length. At a simulated arm64 branch it is worse than a no-op:
`arch_uprobe_skip_sstep()` re-reads `pc` after the handler and hands the corrupted value to
the simulator as the branch base, so `simulate_b_bl()` lands at `target + disp`. At a return
site the opposite holds -- `handle_trampoline()` sets `pc` from `orig_ret_vaddr` before
running the return handlers and never touches it again -- so `JUMP` and `SKIP` are reliable
there. `FORCE_RET` is refused everywhere, because at a return site `x30` still holds the
trampoline address and honouring it re-enters a `return_instance` that has already been
freed, ending in `force_sig(SIGILL)`.

The useful action is `SETREG` of index 0 at `ON_RET`: it forges a function's return value,
which is how you make a check report success without touching its body. It covers integer
and pointer returns only. `pt_regs` carries no v-registers, so a floating-point or SIMD
result is out of reach, and a large struct is written through the `x8` indirect pointer, so
`x0` is not what the caller reads.

### Capture

`OBSERVE` writes a `struct ksu_uhook_record` -- hook id, tid, monotonic timestamp and 34
register slots -- into a ring of `UHOOK_RING` (512) records. `cap_regs` limits how many
leading registers are meaningful; 0 means all 34.

`READ` drains it: `ret` is bytes copied, `arg1` the record count, and `lost` the cumulative
count of records dropped to ring overrun. The drain has to release the ring lock across
`copy_to_user()`, so the ring is sequenced by monotonic counters rather than a bare index --
an index alone cannot distinguish "nobody touched the ring" from "the producer advanced by
an exact multiple of the depth and wrapped". Records are copied out before they are
consumed, and a fault on the first record is an error rather than a silent empty drain.

The ring is global and shared by all hooks, and `READ` is an unfiltered destructive drain.
Two consumers steal each other's records; `lost` is an upper bound, not an exact count.

## Working with both

The two compose into one workflow for understanding and then changing an obfuscated routine:

1. **Learn the shape.** `UHOOK_ADD` with `site = ON_ENTRY`, `action = OBSERVE` and
   `filter_tgid` set to the target, on the suspicious function. Drain with `UHOOK_READ`
   until you know what the arguments look like.
2. **Stop on one hit.** `PTCTL_HWBP_SET` on the instruction, then `HWBP_WAIT`. Nothing in
   the target's text changes, so a self-checksum stays quiet. Inspect with `PEEK` and
   `GETREGS`, adjust with `SETREGS` and `POKE`, then `HWBP_RELEASE`.
3. **Make it permanent.** Replace the observation hook with `site = ON_RET`,
   `action = SETREG`, `act_reg = 0`, pinning the check's return value, and add a `cond` so
   it only fires for the arguments you care about.
4. **Survive the watchdog.** If the target or its supervisor kills the process on tamper,
   `PTCTL_KILLGUARD` the thread group first.

Do not drive both mechanisms at the same address on the same thread. A ptctl `POKE` and a
uhook probe at one address clobber each other, and a hold breakpoint on a thread that also
hits a uprobe collides over the single-step machinery.

## What the target can still see

| Mechanism | Visible as |
| --- | --- |
| ptctl `PEEK` / `GETREGS` | nothing |
| ptctl `SETREGS` | nothing directly; a thread's own state changed under it |
| ptctl `POKE` into text | a checksum over the process's own mapping; not one that re-reads the file |
| ptctl `KILLGUARD` | an entry in `/sys/kernel/debug/kprobes/list`; every signal send costs a debug exception |
| ptctl HWBP | debug registers only, so nothing a target can read; `TracerPid` stays 0 |
| uhook | a `BRK` in the private COW copy of its own text; a `[uprobes]` line in `/proc/<pid>/maps` once any probe fires; non-zero `Anonymous` in `smaps` for an `r-xp` file mapping that normally has none |

Neither mechanism sets `TracerPid`, and neither is defeated by a target that ptraces itself.

## Limits

| | |
| --- | --- |
| ptctl `PEEK`/`POKE` | 64 KiB per call, no automatic chunking |
| ptctl `KILLGUARD` | 32 guarded thread groups; entries have no exit hook, so tgids can be recycled into a guard |
| ptctl HWBP | one breakpoint and one held thread system-wide; up to 640 threads armed; the park is an interruptible sleep |
| ptctl `DETACH_TRACER` | reserved, `-ENOSYS` |
| uhook | 32 simultaneous hooks, a 512-record ring shared by all of them, 256-byte `POKE` payload |
| uhook | the hooked filesystem cannot be unmounted while a hook exists |
| uhook | a handler that faults on user memory sleeps holding `uprobe->register_rwsem`, so a stalled fault wedges `DEL` and every other thread at the same probe |
| both | `only_root` is a bare uid-0 test; there is no per-caller policy |
| both | `POKE` writes to an unvalidated address; that is the point of the primitive, but nothing sanity-checks the target |

## Kernel version behaviour

Both features resolve their kernel entry points through kallsyms, so a symbol that is
missing disables the affected operation rather than failing the build or the load. The
version-sensitive points, all of which the module compiles across `android12-5.10` through
`android17-6.18`:

- **v5.14 / v5.16** -- `task_struct::state` became `::__state`, `task_is_running()` appeared,
  and `task_call_func()` arrived with its callback typedef. On a kernel without
  `task_call_func`, `GETREGS` and `SETREGS` fall back to serving only tasks that are already
  off-CPU.
- **v6.12** -- `perf_event::orig_overflow_handler` and `uses_default_overflow_handler()` were
  removed when the BPF overflow handler started being called directly. The trick that makes
  arm64's `breakpoint_handler()` run its step-past-the-breakpoint dance is unavailable
  there, so `hwbp_prepare_step()` performs the same sequence by hand.
- **v6.12** -- `uprobe_register()` absorbed the `ref_ctr_offset` argument and returns a
  `struct uprobe *`; `uprobe_unregister()` split into `_nosync` and `_sync`; the filter lost
  its `uprobe_filter_ctx` argument.
- **v6.13** -- `handler` and `ret_handler` gained a trailing `__u64 *data` session cookie. A
  hook installs exactly one of the two and `uprobes.c` takes
  `session = uc->handler && uc->ret_handler`, so a hook is never a session consumer and the
  cookie is unused.

## See also

- [`uapi/README.md`](../uapi/README.md) -- the ABI contract and the rules for extending it
- [`uapi/supercall.h`](../uapi/supercall.h) -- the normative per-field documentation
- [`kernel/feature/README.md`](../kernel/feature/README.md) -- how these sit beside the
  other features
- [`kernel/supercall/README.md`](../kernel/supercall/README.md) -- the dispatch table and
  the permission classes
- [`docs/architecture.md`](architecture.md) -- the repository-wide hub
- `Documentation/trace/kprobes.rst` and `Documentation/trace/uprobetracer.rst` in the kernel
  source -- the primary references for the two dynamic mechanisms
- Brendan Gregg, *BPF Performance Tools* (Addison-Wesley, 2019), chapter 2 -- the clearest
  survey of the event sources, and the source of the overhead figures quoted above

<!-- reference links: kernel documentation and man pages -->
[bpf]: https://docs.kernel.org/bpf/index.html
[cachetlb]: https://docs.kernel.org/core-api/cachetlb.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[fork-2]: https://man7.org/linux/man-pages/man2/fork.2.html
[ftrace]: https://docs.kernel.org/trace/ftrace.html
[kill-2]: https://man7.org/linux/man-pages/man2/kill.2.html
[kprobes]: https://docs.kernel.org/trace/kprobes.html
[mmap-2]: https://man7.org/linux/man-pages/man2/mmap.2.html
[perf-event-open-2]: https://man7.org/linux/man-pages/man2/perf_event_open.2.html
[pidfd-send-signal-2]: https://man7.org/linux/man-pages/man2/pidfd_send_signal.2.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[rt-sigqueueinfo-2]: https://man7.org/linux/man-pages/man2/rt_sigqueueinfo.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[static-keys]: https://docs.kernel.org/staging/static-keys.html
[tgkill-2]: https://man7.org/linux/man-pages/man2/tgkill.2.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
[uprobetracer]: https://docs.kernel.org/trace/uprobetracer.html
