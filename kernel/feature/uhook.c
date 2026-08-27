// SPDX-License-Identifier: GPL-2.0
/*
 * uhook - general kernel-mediated userspace instrumentation via uprobes,
 * exposed through KSU_IOCTL_UHOOK (see uapi/supercall.h).
 *
 * A hook is keyed by (file inode, file-offset), so it applies to every thread
 * and process that maps the file and is immune to ASLR. This is the
 * persistent/automatic counterpart to the interactive HWBP-hold in ptctl.
 *
 * A hook = where (entry or uretprobe return) + when (condition) + what (action),
 * with an optional register capture drained via KSU_UHOOK_READ.
 *
 * VISIBILITY -- read this before believing a hook is invisible. A uprobe's
 * mechanism IS a modified instruction byte: uprobe_write_opcode() writes BRK #5
 * into a private COW copy of the target's own text page. It does not touch the
 * file on disk and it does not set TracerPid, but the target reading its own
 * .text (directly, via /proc/self/mem, or via PEEKTEXT) sees the BRK, so a
 * self-checksum over its own mapping WILL fire. Two further tells: once any
 * probe hits, /proc/<pid>/maps grows a `[uprobes]` line, and /proc/<pid>/smaps
 * reports a non-zero Anonymous: inside an r-xp file mapping that normally has
 * none. (Anonymous:, not Private_Dirty: -- the COW'd text page is accounted
 * clean on android14-6.1, measured at 4 kB Anonymous / 0 kB Private_Dirty.)
 *
 * SCOPE -- filter_tgid is mandatory and ADD refuses 0. It used to be documented
 * as "0 = every process that maps the file", which is exactly what both gates
 * below implemented, and an unscoped hook is wrong in three separate ways. It
 * plants the BRK in every mapper, so the tells above appear in zygote,
 * system_server and anything else holding the file open, and the patched page is
 * then inherited across fork. It lets an unrelated mapper break the hook you
 * actually wanted: register_for_each_vma() assigns rather than ORs
 * install_breakpoint()'s result, abandons the remaining mms on the first error,
 * and __uprobe_register() then unwinds the whole consumer -- with one approved
 * mm that cannot happen, with every mm approved it is one exiting process away.
 * (That one is at least loud: the error reaches uh_add(), which fails the ADD.)
 * The third is not. uprobe_register() reports success whether it planted the
 * breakpoint in fifty address spaces or in none, so a hook armed nowhere lists
 * as active and reads exactly like a probe site the target never reaches.
 * Naming the process fixes the first two by construction; uh_mapped_count()
 * answers the third.
 *
 * What 0 was NOT is a gate that matched nothing: both gates below implemented it
 * as documented and an unscoped hook does fire -- measured, ten hooks on a
 * target-only library gave identical results scoped and unscoped, and one
 * unscoped hook on libc's open() fired in 16 processes including the target.
 * It fires and then buries itself: 6996 records in eight seconds against the
 * target's own 8, through one 512-record ring that drops the oldest, so a drain
 * afterwards showed none of the target's. An empty ring was never evidence
 * about the filter.
 *
 * CONTROL FLOW -- on the uprobe core a handler-set pc does NOT survive an ENTRY
 * site. After handler_chain(), handle_swbp() single-steps the probed
 * instruction out of line and arch_uprobe_post_xol() unconditionally resets pc
 * to (probed address + insn length); at an arm64 site the decoder simulates
 * instead, arch_uprobe_skip_sstep() re-reads pc AFTER the handler and feeds the
 * corrupted value to the simulator as the branch base, which is worse -- the
 * thread lands at a wild address. FORCE_RET/JUMP/SKIP and SETREG-of-pc are
 * therefore REJECTED at an entry site (-EOPNOTSUPP) instead of silently
 * misbehaving.
 *
 * At a RETURN site the picture is the opposite: handle_swbp() recognises the
 * trampoline and tail-calls handle_trampoline(), which sets pc =
 * ri->orig_ret_vaddr and only then runs the ret_handlers, with no single step
 * afterwards. JUMP and SKIP are reliable there, and SETREG of x0 genuinely
 * forges the function's integer/pointer return value -- the canonical "make a
 * check report success" bypass. (It cannot forge an FP/SIMD return: pt_regs
 * carries no v-registers. A large struct returns through the x8 indirect
 * pointer, so x0 is not what the caller reads.) FORCE_RET is rejected at a
 * return site too: x30 still holds the trampoline address there, so honouring
 * it re-enters a return_instance that has already been freed and ends in
 * force_sig(SIGILL).
 *
 * ON_RET also requires `offset` to be the function's FIRST instruction: arm64's
 * arch_uretprobe_hijack_return_addr() steals x30, not a stack slot, so a
 * mid-function offset hijacks whatever LR happens to hold.
 *
 * Portability. Register access is implemented for arm64 and x86_64 (the module
 * is built for both); other arches fall back to no-ops so it still links. The
 * uprobe ABI moved twice. v6.12 merged the ref_ctr_offset argument into
 * uprobe_register(), which now returns a struct uprobe *, split
 * uprobe_unregister() into _nosync/_sync, and dropped the uprobe_filter_ctx
 * argument from the filter. v6.13 added the session-consumer cookie, so handler
 * and ret_handler each gained a trailing __u64 *data. Each era is handled below.
 * The cookie is unused here: a hook installs exactly one of the two handlers and
 * uprobes.c takes `session = uc->handler && uc->ret_handler`, so a hook is never
 * a session consumer, and a ret_handler-only consumer still gets its return
 * instance prepared. All ops are root-gated by the supercall perm check; kernel
 * entry points are resolved via kallsyms so this links whether built in or as an
 * LKM.
 */
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/uprobes.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/err.h>
#include <linux/printk.h>
#include <asm/ptrace.h>
#if defined(CONFIG_ARM64) || defined(__aarch64__)
#include <asm/debug-monitors.h> /* DBG_SPSR_SS */
#endif
#if defined(CONFIG_X86_64) || defined(__x86_64__)
#include <asm/processor-flags.h> /* X86_EFLAGS_TF */
#endif

#include "uapi/supercall.h"
#include "feature/uhook.h"
#include "infra/symbol_resolver.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define UHOOK_NEW_UPROBE 1
#else
#define UHOOK_NEW_UPROBE 0
#endif

/* v6.13 gave handler and ret_handler a trailing session cookie. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#define UHOOK_SESSION_COOKIE 1
#else
#define UHOOK_SESSION_COOKIE 0
#endif

#define UHOOK_MAX 32 /* max simultaneous hooks */
#define UHOOK_RING 512 /* capture ring depth (records) */
#define UHOOK_POKE_MAX 256 /* max POKE payload bytes */
#define UHOOK_NREG 34 /* x0..x30, sp, pc, pstate */
#define UHOOK_REG_SP 31
#define UHOOK_REG_PC 32
#define UHOOK_REG_STATE 33

/* --- resolved kernel symbols (the uprobe ABI differs across versions) --- */
#if UHOOK_NEW_UPROBE
typedef struct uprobe *(*uprobe_register_t)(struct inode *inode, loff_t offset, loff_t ref_ctr_offset,
                                            struct uprobe_consumer *uc);
typedef void (*uprobe_unregister_nosync_t)(struct uprobe *uprobe, struct uprobe_consumer *uc);
typedef void (*uprobe_unregister_sync_t)(void);
static uprobe_register_t p_uprobe_register;
static uprobe_unregister_nosync_t p_uprobe_unregister_nosync;
static uprobe_unregister_sync_t p_uprobe_unregister_sync;
#else
typedef int (*uprobe_register_t)(struct inode *inode, loff_t offset, struct uprobe_consumer *uc);
typedef void (*uprobe_unregister_t)(struct inode *inode, loff_t offset, struct uprobe_consumer *uc);
static uprobe_register_t p_uprobe_register;
static uprobe_unregister_t p_uprobe_unregister;
#endif

typedef struct task_struct *(*find_task_by_vpid_t)(pid_t nr);
static find_task_by_vpid_t p_find_task_by_vpid;

/* Both register AND unregister must be resolvable. A registration that can
 * never be retired becomes a use-after-free the moment the module is unloaded,
 * so the feature stays off rather than half-working. */
static bool uhook_ready;

/* --- one installed hook --- */
struct uhook {
    struct uprobe_consumer uc; /* recovered via container_of() in the handlers */
    bool active;
    u32 id;
    /* uprobe_register() requires the caller to keep BOTH the inode and the
     * containing mount referenced for the hook's lifetime, so the whole path is
     * held, not just an igrab'd inode. Note this pins the vfsmount: a hooked
     * filesystem cannot be unmounted until the hook is removed. */
    struct path path;
    struct inode *inode;
    loff_t offset;
#if UHOOK_NEW_UPROBE
    struct uprobe *uprobe; /* handle needed to unregister on 6.12+ */
#endif
    u32 site; /* enum ksu_uhook_site */
    /* Scope. Always set: uh_validate() refuses filter_tgid 0, so there is no
     * unscoped mode left for these two fields to encode.
     *
     * The placement filter is anchored on the address space, NOT on a task:
     * ->mm is cleared by exit_mm() when that THREAD exits, so a task anchor
     * silently disarms when the caller passed a tid that exits, or when the
     * group leader calls pthread_exit() and becomes a zombie while the process
     * keeps running. An mm_count reference keeps the struct alive for the
     * pointer comparison without pinning the address space itself. */
    struct mm_struct *filter_mm;
    pid_t filter_tgid;
    /* condition */
    u32 cond, cond_reg, cond_cmp, cond_len;
    s64 cond_off;
    u64 cond_val;
    /* action */
    u32 action, act_reg;
    s64 act_off;
    u64 act_val;
    void *poke_data;
    u32 poke_len;
    /* capture */
    u32 cap_regs;
    /* Diagnostics, read back through KSU_UHOOK_LIST. Deliberately three
     * counters and not one: "the probe was never reached", "it was reached and
     * the scope or the condition rejected every hit" and "it fired but the
     * action could not touch the target's memory" are three different answers
     * that all look identical from the capture ring, which stays empty in every
     * one of them. An instrumentation tool that cannot tell them apart makes
     * its user blame the target. Plain increments from probe context, so they
     * are approximate when several threads hit at once; a lock on this path
     * would cost more than the counters are worth. */
    u64 traps; /* probe reached, before any filtering */
    u64 hits; /* scope + condition passed, action applied */
    u64 fails; /* action or memory condition could not reach user memory */
};

static struct uhook hooks[UHOOK_MAX];
static DEFINE_MUTEX(hooks_lock);
static u32 next_id = 1;

/* --- capture ring (producer: handlers; consumer: KSU_UHOOK_READ) --- */
static struct ksu_uhook_record *ring;
/* Monotonic 64-bit sequence counters rather than a wrapping head/count pair:
 * the drain has to drop ring_lock across copy_to_user(), and a bare index
 * cannot distinguish "nobody touched the ring" from "the producer advanced by
 * an exact multiple of UHOOK_RING and wrapped back to the same index", which
 * would silently discard a live record without counting it. */
static u64 ring_prod; /* records produced */
static u64 ring_cons; /* records consumed or dropped */
/* Upper bound on the records userspace did not see. A record can be counted
 * here and still be delivered, if the producer dropped it while the drain had
 * already copied it out with ring_lock released; over-counting is the safe
 * direction. */
static u64 ring_lost;
static DEFINE_SPINLOCK(ring_lock);
static DEFINE_MUTEX(ring_read_lock); /* serialises KSU_UHOOK_READ consumers */

/* FLAG_MASK from arch/x86/kernel/ptrace.c: the EFLAGS bits ptrace lets
 * userspace own. */
#define UHOOK_X86_FLAG_MASK (0x54dd5UL & ~(unsigned long)X86_EFLAGS_TF)

/* A forged processor-state word is ERET'd/IRET'd straight back to userspace, so
 * it is checked at ADD time rather than trusted at fire time.
 *
 * arm64 rejects, because SPSR has no safe default to fall back to. x86_64
 * accepts anything and merges under FLAG_MASK at fire time instead, exactly as
 * set_flags() does -- rejecting would be wrong there, since EFLAGS bit 1 is a
 * fixed-1 reserved bit that lies outside FLAG_MASK, so every realistic value
 * has a bit set that the mask does not cover. */
static bool uh_state_val_ok(u64 v)
{
#if defined(CONFIG_ARM64) || defined(__aarch64__)
    /* verbatim from arch/arm64/kernel/ptrace.c valid_native_regs() */
#define UHOOK_SPSR_RES0                                                                                                \
    (GENMASK_ULL(63, 32) | GENMASK_ULL(27, 26) | GENMASK_ULL(23, 22) | GENMASK_ULL(20, 13) | GENMASK_ULL(5, 5))
    if (v & UHOOK_SPSR_RES0)
        return false;
    if ((v & PSR_MODE_MASK) != PSR_MODE_EL0t)
        return false;
    if (v & PSR_MODE32_BIT)
        return false;
    if (v & (PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT))
        return false;
    /* the software-step bit is kernel state, never the caller's to set:
     * clobbering it strands a thread the HW-breakpoint hold armed for a step */
    if (v & DBG_SPSR_SS)
        return false;
    return true;
#elif defined(CONFIG_X86_64) || defined(__x86_64__)
    (void)v;
    return true;
#else
    (void)v;
    return false;
#endif
}

/* SETREG of the state register. arm64 takes the already-validated value; x86_64
 * merges only the caller-owned EFLAGS bits over the live ones. */
static void uh_set_state_reg(struct pt_regs *regs, u64 v)
{
#if defined(CONFIG_ARM64) || defined(__aarch64__)
    /* Open-coded user_regs_reset_single_step(): the software-step bit is
     * kernel state in BOTH directions. uh_state_val_ok() already refuses a
     * value that sets it, but a plain write would still CLEAR it -- and
     * user_enable_single_step() only writes SPSR.SS on the 0->1 transition of
     * TIF_SINGLESTEP, so nothing would ever put it back. That would strand a
     * thread mid-step: neither the HW-breakpoint hold nor the uprobe's own
     * out-of-line step would retire, and the thread runs off the end of its
     * XOL slot into a UDF. Force the bit to match the thread flag instead. */
    if (test_thread_flag(TIF_SINGLESTEP))
        v |= DBG_SPSR_SS;
    else
        v &= ~DBG_SPSR_SS;
    regs->pstate = v;
#elif defined(CONFIG_X86_64) || defined(__x86_64__)
    regs->flags = (regs->flags & ~UHOOK_X86_FLAG_MASK) | (v & UHOOK_X86_FLAG_MASK);
#else
    (void)regs;
    (void)v;
#endif
}

/* --- register access by index. arm64: 0..30 = x0..x30, 31 = sp, 32 = pc,
 * 33 = pstate. x86_64: 0 = ax (return value, so SETREG index 0 forges a return
 * on both arches), 1..14 = bx,cx,dx,si,di,bp,r8..r15, 31 = sp, 32 = ip,
 * 33 = flags. Other arches return 0 so the module still links. --- */
static u64 uh_get_reg(struct pt_regs *regs, u32 i)
{
#if defined(CONFIG_ARM64) || defined(__aarch64__)
    if (i < 31)
        return regs->regs[i];
    if (i == UHOOK_REG_SP)
        return regs->sp;
    if (i == UHOOK_REG_PC)
        return regs->pc;
    if (i == UHOOK_REG_STATE)
        return regs->pstate;
    return 0;
#elif defined(CONFIG_X86_64) || defined(__x86_64__)
    switch (i) {
    case 0:
        return regs->ax;
    case 1:
        return regs->bx;
    case 2:
        return regs->cx;
    case 3:
        return regs->dx;
    case 4:
        return regs->si;
    case 5:
        return regs->di;
    case 6:
        return regs->bp;
    case 7:
        return regs->r8;
    case 8:
        return regs->r9;
    case 9:
        return regs->r10;
    case 10:
        return regs->r11;
    case 11:
        return regs->r12;
    case 12:
        return regs->r13;
    case 13:
        return regs->r14;
    case 14:
        return regs->r15;
    case UHOOK_REG_SP:
        return regs->sp;
    case UHOOK_REG_PC:
        return regs->ip;
    case UHOOK_REG_STATE:
        return regs->flags;
    default:
        return 0;
    }
#else
    (void)regs;
    (void)i;
    return 0;
#endif
}

static void uh_set_reg(struct pt_regs *regs, u32 i, u64 v)
{
#if defined(CONFIG_ARM64) || defined(__aarch64__)
    if (i < 31)
        regs->regs[i] = v;
    else if (i == UHOOK_REG_SP)
        regs->sp = v;
    else if (i == UHOOK_REG_PC)
        regs->pc = v;
    else if (i == UHOOK_REG_STATE)
        uh_set_state_reg(regs, v);
#elif defined(CONFIG_X86_64) || defined(__x86_64__)
    switch (i) {
    case 0:
        regs->ax = v;
        break;
    case 1:
        regs->bx = v;
        break;
    case 2:
        regs->cx = v;
        break;
    case 3:
        regs->dx = v;
        break;
    case 4:
        regs->si = v;
        break;
    case 5:
        regs->di = v;
        break;
    case 6:
        regs->bp = v;
        break;
    case 7:
        regs->r8 = v;
        break;
    case 8:
        regs->r9 = v;
        break;
    case 9:
        regs->r10 = v;
        break;
    case 10:
        regs->r11 = v;
        break;
    case 11:
        regs->r12 = v;
        break;
    case 12:
        regs->r13 = v;
        break;
    case 13:
        regs->r14 = v;
        break;
    case 14:
        regs->r15 = v;
        break;
    case UHOOK_REG_SP:
        regs->sp = v;
        break;
    case UHOOK_REG_PC:
        regs->ip = v;
        break;
    case UHOOK_REG_STATE:
        uh_set_state_reg(regs, v);
        break;
    default:
        break;
    }
#else
    (void)regs;
    (void)i;
    (void)v;
#endif
}

static bool uh_cmp(u64 a, u32 op, u64 b)
{
    switch (op) {
    case KSU_UHOOK_EQ:
        return a == b;
    case KSU_UHOOK_NE:
        return a != b;
    case KSU_UHOOK_LT:
        return a < b;
    case KSU_UHOOK_GT:
        return a > b;
    case KSU_UHOOK_AND:
        return (a & b) != 0;
    case KSU_UHOOK_SLT:
        return (s64)a < (s64)b;
    case KSU_UHOOK_SGT:
        return (s64)a > (s64)b;
    }
    return false;
}

/* Scope gate, evaluated in the hitting thread.
 *
 * This is mandatory even though uc.filter already refuses out-of-scope mms: a
 * fork()ed child of an in-scope process inherits the patched text page through
 * COW and uprobe_dup_mmap() propagates MMF_HAS_UPROBES, so it traps in an mm the
 * filter never approved. Measured: with a hook scoped to a parent, all three of
 * its forked children grew a [uprobes] XOL area -- proof they executed the
 * breakpoint -- while contributing zero records. Without this gate those hits
 * would have been attributed to the scoped hook.
 *
 * A bare comparison, with no "0 matches everything" case. filter_tgid is never 0
 * on a live hook, so the only way to arrive here with 0 is a half-torn-down one,
 * and answering false for that is the direction that does nothing. */
static bool uh_in_scope(struct uhook *h)
{
    return current->tgid == h->filter_tgid;
}

/* Handlers run in the context of the hitting thread, so its userspace is
 * addressable directly via copy_from_user()/copy_to_user(). */
static bool uh_cond_ok(struct uhook *h, struct pt_regs *regs)
{
    u64 v = 0;

    switch (h->cond) {
    case KSU_UHOOK_COND_NONE:
        return true;
    case KSU_UHOOK_COND_REG:
        if (h->cond_reg >= UHOOK_NREG)
            return false;
        return uh_cmp(uh_get_reg(regs, h->cond_reg), h->cond_cmp, h->cond_val);
    case KSU_UHOOK_COND_MEM: {
        unsigned long addr;
        u32 len = h->cond_len ? min_t(u32, h->cond_len, 8u) : 8u;

        if (h->cond_reg >= UHOOK_NREG)
            return false;
        addr = (unsigned long)(uh_get_reg(regs, h->cond_reg) + h->cond_off);
        /* v is zero-initialised, so a narrow read is zero-extended; the value
         * is interpreted little-endian, matching every arch this builds for */
        if (copy_from_user(&v, (void __user *)addr, len)) {
            /* Unreadable -> do not fire, but count it: a condition aimed at an
             * address the target never populates otherwise looks exactly like a
             * condition that is simply never true. */
            h->fails++;
            return false;
        }
        return uh_cmp(v, h->cond_cmp, h->cond_val);
    }
    }
    return false;
}

static void uh_capture(struct uhook *h, struct pt_regs *regs)
{
    struct ksu_uhook_record *rec;
    unsigned long flags;
    u32 idx, i, n;

    if (!ring)
        return;
    spin_lock_irqsave(&ring_lock, flags);
    if (ring_prod - ring_cons >= UHOOK_RING) {
        /* full: drop the oldest to make room, and account for it so
         * KSU_UHOOK_READ can report the gap instead of hiding it */
        ring_cons++;
        ring_lost++;
    }
    idx = (u32)(ring_prod % UHOOK_RING);
    rec = &ring[idx];
    rec->id = h->id;
    rec->tid = task_pid_nr(current);
    rec->ts_ns = ktime_get_ns();
    n = h->cap_regs ? min_t(u32, h->cap_regs, (u32)UHOOK_NREG) : (u32)UHOOK_NREG;
    for (i = 0; i < UHOOK_NREG; i++)
        rec->regs[i] = i < n ? uh_get_reg(regs, i) : 0;
    ring_prod++;
    spin_unlock_irqrestore(&ring_lock, flags);
}

static void uh_apply(struct uhook *h, struct pt_regs *regs)
{
    /* Counted before the gates: this is the only evidence that the probed
     * instruction executed at all, and it is the one fact a caller staring at
     * an empty capture ring cannot otherwise obtain. */
    h->traps++;
    if (!uh_in_scope(h))
        return;
    if (!uh_cond_ok(h, regs))
        return;
    h->hits++;

    switch (h->action) {
    case KSU_UHOOK_OBSERVE:
        uh_capture(h, regs);
        break;
    case KSU_UHOOK_SETREG:
        if (h->act_reg < UHOOK_NREG)
            uh_set_reg(regs, h->act_reg, h->act_val);
        break;
    case KSU_UHOOK_JUMP:
        /* only reachable at a return site; uh_validate() rejects entry sites */
        instruction_pointer_set(regs, h->act_val);
        break;
    case KSU_UHOOK_SKIP:
        instruction_pointer_set(regs, instruction_pointer(regs) + h->act_val);
        break;
    case KSU_UHOOK_POKE:
        if (h->poke_data && h->act_reg < UHOOK_NREG) {
            unsigned long addr = (unsigned long)(uh_get_reg(regs, h->act_reg) + h->act_off);

            /* A poke into a page the target has not faulted in, or into a
             * read-only mapping, writes nothing. Ignoring that would leave the
             * hook reporting hits forever while the patch never lands -- the
             * caller sees a working hook and a target that "ignores" the
             * patch. */
            if (copy_to_user((void __user *)addr, h->poke_data, h->poke_len))
                h->fails++;
        }
        break;
    }
}

#if UHOOK_SESSION_COOKIE
static int uh_entry_handler(struct uprobe_consumer *uc, struct pt_regs *regs, __u64 *data)
#else
static int uh_entry_handler(struct uprobe_consumer *uc, struct pt_regs *regs)
#endif
{
    uh_apply(container_of(uc, struct uhook, uc), regs);
    return 0; /* never UPROBE_HANDLER_REMOVE: that would unapply the breakpoint */
}

#if UHOOK_SESSION_COOKIE
static int uh_ret_handler(struct uprobe_consumer *uc, unsigned long func, struct pt_regs *regs, __u64 *data)
#else
static int uh_ret_handler(struct uprobe_consumer *uc, unsigned long func, struct pt_regs *regs)
#endif
{
    (void)func;
    uh_apply(container_of(uc, struct uhook, uc), regs);
    return 0;
}

/*
 * Scope the breakpoint insertion itself. Runs under mmap_write_lock() on a
 * foreign mm and from inside uprobe_register(), so it must not lock, allocate
 * or fault. It reads only fields that are immutable between uh_add() and
 * uh_free(), plus one READ_ONCE of the target's ->mm.
 *
 * The same predicate is applied to every ctx, including UNREGISTER: answering
 * false there would let another consumer's unregister rip out our breakpoint.
 */
#if !UHOOK_NEW_UPROBE
static bool uh_filter(struct uprobe_consumer *uc, enum uprobe_filter_ctx ctx, struct mm_struct *mm)
{
    struct uhook *h = container_of(uc, struct uhook, uc);

    (void)ctx;
    /* uprobe_mmap() calls the filter with current->mm, so the tgid is
     * authoritative there -- and it keeps working across execve(), which
     * replaces the mm and would otherwise turn the hook into a silent no-op
     * forever. register_for_each_vma() walks foreign mms, where the pinned
     * anchor is all we have. */
    if (mm == current->mm)
        return current->tgid == h->filter_tgid;
    return h->filter_mm && h->filter_mm == mm;
}
#else
/* 6.12+ dropped the ctx argument. UNVERIFIED against that tree -- confirm the
 * prototype before shipping, because a mismatch is a kCFI trap, not a warning. */
static bool uh_filter(struct uprobe_consumer *uc, struct mm_struct *mm)
{
    struct uhook *h = container_of(uc, struct uhook, uc);

    if (mm == current->mm)
        return current->tgid == h->filter_tgid;
    return h->filter_mm && h->filter_mm == mm;
}
#endif

/* --- register/unregister across the two uprobe ABIs --- */
static int uh_uprobe_register(struct uhook *h)
{
#if UHOOK_NEW_UPROBE
    h->uprobe = p_uprobe_register(h->inode, h->offset, 0, &h->uc);
    if (IS_ERR_OR_NULL(h->uprobe)) {
        int ret = IS_ERR(h->uprobe) ? PTR_ERR(h->uprobe) : -ENODEV;

        h->uprobe = NULL;
        return ret;
    }
    return 0;
#else
    return p_uprobe_register(h->inode, h->offset, &h->uc);
#endif
}

static void uh_uprobe_unregister(struct uhook *h)
{
#if UHOOK_NEW_UPROBE
    if (h->uprobe) {
        p_uprobe_unregister_nosync(h->uprobe, &h->uc);
        /* Not optional: this is the barrier that waits out in-flight handlers.
         * ksu_uhook_init() refuses to arm the feature without it. */
        p_uprobe_unregister_sync();
        h->uprobe = NULL;
    }
#else
    /* uprobe_unregister() runs consumer_del() + register_for_each_vma() under
     * down_write(&uprobe->register_rwsem), and handler_chain() holds that same
     * rwsem for read, so no handler is still running when this returns. That
     * -- not anything in this file -- is what makes the teardown below safe. */
    p_uprobe_unregister(h->inode, h->offset, &h->uc);
#endif
}

/* --- hook table helpers (hold hooks_lock) --- */
static struct uhook *uh_find(u32 id)
{
    int i;

    for (i = 0; i < UHOOK_MAX; i++)
        if (hooks[i].active && hooks[i].id == id)
            return &hooks[i];
    return NULL;
}

/* Never issues 0: userspace uses it as "no hook", and a capture record that
 * carried it could not be attributed. Bounded because the caller already holds
 * a free slot, so at most UHOOK_MAX - 1 ids are live. */
static u32 uh_alloc_id(void)
{
    u32 tries;

    for (tries = 0; tries <= UHOOK_MAX + 1; tries++) {
        u32 id = next_id++;

        if (!id)
            continue;
        if (!uh_find(id))
            return id;
    }
    return 0;
}

static void uh_free(struct uhook *h)
{
    /* Order matters: the uprobe must be retired (which quiesces the handlers)
     * before anything it can reach is released. */
    if (h->inode)
        uh_uprobe_unregister(h);
    if (h->filter_mm)
        mmdrop(h->filter_mm);
    if (h->inode)
        iput(h->inode);
    path_put(&h->path); /* NULL-safe on a zeroed struct path */
    kfree(h->poke_data);
    memset(h, 0, sizeof(*h));
}

static struct task_struct *uh_get_task(pid_t pid)
{
    struct task_struct *task = NULL;

    if (!p_find_task_by_vpid)
        return NULL;
    rcu_read_lock();
    task = p_find_task_by_vpid(pid);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();
    return task;
}

/* Reject verb/site combinations that cannot work, rather than letting them fail
 * silently or land the target at a wild address. See the file header. */
static int uh_validate(struct ksu_uhook_cmd *cmd)
{
    bool at_ret = (cmd->site == KSU_UHOOK_ON_RET);

    if (cmd->site != KSU_UHOOK_ON_ENTRY && cmd->site != KSU_UHOOK_ON_RET)
        return -EINVAL;
    /* A hook must name the process it belongs to; see the SCOPE paragraph in the
     * file header for the three reasons 0 is not a usable value. Note what this
     * is NOT: 0 was never a gate that matched nothing. It did what it said, and
     * an unscoped hook does fire. It is refused because planting a BRK in every
     * mapper is the wrong thing to have done, and because it is the one setting
     * under which "armed nowhere" is the ordinary outcome rather than an edge
     * case, nothing in the request having had to name anything that exists. */
    if (cmd->filter_tgid <= 0) {
        pr_warn("uhook: filter_tgid=%d refused -- name the target's pid or tid. 0 meant every "
                "process mapping the file, zygote included, and nothing in the request had to "
                "name a process that exists\n",
                cmd->filter_tgid);
        return -EINVAL;
    }
    if (cmd->cond > KSU_UHOOK_COND_MEM || cmd->cond_cmp > KSU_UHOOK_SGT)
        return -EINVAL;
    if (cmd->cond != KSU_UHOOK_COND_NONE && cmd->cond_reg >= UHOOK_NREG)
        return -EINVAL;
    if (cmd->cond == KSU_UHOOK_COND_MEM) {
        switch (cmd->cond_len) {
        case 0: /* 0 means the full 8 bytes */
        case 1:
        case 2:
        case 4:
        case 8:
            break;
        default:
            return -EINVAL;
        }
    }
    if (cmd->cap_regs > UHOOK_NREG)
        return -EINVAL;

    switch (cmd->action) {
    case KSU_UHOOK_OBSERVE:
        break;
    case KSU_UHOOK_POKE:
        if (cmd->act_reg >= UHOOK_NREG)
            return -EINVAL;
        /* Checked on the full u64. Narrowing first and clamping second would
         * let any multiple of 2^32 survive as poke_len == 0, and
         * memdup_user(ptr, 0) returns ZERO_SIZE_PTR, which IS_ERR() does not
         * catch -- leaving a non-NULL bogus pointer parked in the hook. */
        if (!cmd->len || !cmd->uptr || cmd->len > UHOOK_POKE_MAX)
            return -EINVAL;
        break;
    case KSU_UHOOK_SETREG:
        if (cmd->act_reg >= UHOOK_NREG)
            return -EINVAL;
        if (cmd->act_reg == UHOOK_REG_STATE && !uh_state_val_ok(cmd->act_val))
            return -EINVAL;
        /* writing pc is a JUMP by another name and inherits its restriction */
        if (cmd->act_reg == UHOOK_REG_PC && !at_ret)
            return -EOPNOTSUPP;
        break;
    case KSU_UHOOK_JUMP:
    case KSU_UHOOK_SKIP:
        if (!at_ret)
            return -EOPNOTSUPP; /* pc is reset by the out-of-line single step */
        break;
    case KSU_UHOOK_FORCE_RET:
        /* At an entry site the single step discards pc. At a return site pc IS
         * honoured, but x30 holds the trampoline address, so returning through
         * it re-enters a freed return_instance and ends in SIGILL. Use
         * SETREG of x0 at ON_RET, or a ptctl POKE, instead. */
        return -EOPNOTSUPP;
    default:
        return -EINVAL;
    }
    return 0;
}

/* How many address spaces currently have the probed offset mapped in a form
 * uprobes will accept, counting only those the hook's scope would act on.
 *
 * uprobe_register() returns 0 whether it planted the breakpoint in fifty
 * processes or in none: register_for_each_vma() over an empty map list is a
 * successful no-op. So a hook naming the wrong file, an offset in a region
 * nobody maps executable, or a target that has not started yet installs exactly
 * like a correct one, and the caller then attributes the silence to the target
 * rather than to the request. This is the number that separates them.
 *
 * It is a snapshot, so 0 is not an error -- a process that maps the file later
 * is armed through uprobe_mmap(). It mirrors build_map_info()'s walk and
 * open-codes valid_vma(vma, true), which is static in uprobes.c. */
static u64 uh_mapped_count(struct uhook *h)
{
    struct address_space *mapping = h->inode->i_mapping;
    struct vm_area_struct *vma;
    pgoff_t pgoff = (pgoff_t)(h->offset >> PAGE_SHIFT);
    u64 n = 0;

    i_mmap_lock_read(mapping);
    vma_interval_tree_foreach (vma, &mapping->i_mmap, pgoff, pgoff) {
        if (!vma->vm_file)
            continue;
        if ((vma->vm_flags & (VM_HUGETLB | VM_MAYEXEC | VM_MAYSHARE | VM_WRITE)) != VM_MAYEXEC)
            continue;
        /* filter_mm is the mm the named process had a moment ago, which is the
         * right answer for a snapshot taken right after registration. It goes
         * stale across an execve(); uh_filter() covers that case at run time by
         * falling back to the tgid, this does not. */
        if (vma->vm_mm != h->filter_mm)
            continue;
        n++;
    }
    i_mmap_unlock_read(mapping);
    return n;
}

static int uh_add(struct ksu_uhook_cmd *cmd)
{
    struct uhook *h = NULL;
    struct path path = {};
    struct inode *inode = NULL;
    struct task_struct *ftask = NULL;
    struct mm_struct *fmm = NULL;
    pid_t ftgid = 0;
    void *poke = NULL;
    u32 poke_len = 0;
    char *kpath;
    int i, ret;
    u32 id;

    if (!uhook_ready)
        return -ENOSYS;

    ret = uh_validate(cmd);
    if (ret)
        return ret;

    /* strndup_user() force-terminates and returns -EINVAL past PATH_MAX;
     * strncpy_from_user() returns PATH_MAX without an error and without a NUL,
     * which kern_path() would then read past. */
    kpath = strndup_user((const char __user *)(uintptr_t)cmd->path, PATH_MAX);
    if (IS_ERR(kpath))
        return PTR_ERR(kpath);
    ret = kern_path(kpath, LOOKUP_FOLLOW, &path);
    kfree(kpath);
    if (ret)
        return ret;

    if (!d_is_reg(path.dentry)) {
        ret = -EINVAL;
        goto err_path;
    }
    /* d_real_inode(), not d_inode(): on overlayfs the overlay inode has no
     * ->read_folio, so uprobe_register() would refuse it with -EIO. The lower
     * inode is also the one uprobes keys vmas against. Android mounts /system
     * and KernelSU's own modules through overlayfs, so this is what makes the
     * common targets hookable at all. */
    inode = igrab(d_real_inode(path.dentry));
    if (!inode) {
        ret = -ENOENT;
        goto err_path;
    }

    if (cmd->action == KSU_UHOOK_POKE) {
        poke_len = (u32)cmd->len; /* uh_validate() bounded this to 1..UHOOK_POKE_MAX */
        poke = memdup_user((void __user *)(uintptr_t)cmd->uptr, poke_len);
        if (IS_ERR(poke)) {
            ret = PTR_ERR(poke);
            poke = NULL;
            goto err_inode;
        }
    }

    /* Not conditional any more: uh_validate() has already refused filter_tgid 0,
     * so every hook is anchored to a process that exists at ADD time. That is
     * also what keeps one mapper's install_breakpoint() failure from aborting
     * the registration for another's -- the filter approves exactly one mm. */
    ftask = uh_get_task((pid_t)cmd->filter_tgid);
    if (!ftask) {
        ret = -ESRCH;
        goto err_poke;
    }
    if (ftask->flags & PF_KTHREAD) {
        ret = -EINVAL;
        goto err_task;
    }
    ftgid = ftask->tgid; /* the caller may have passed a tid */
    /* mm_count, not mm_users: we only ever compare the pointer, and an
     * mm_users reference would keep the whole address space alive. */
    task_lock(ftask);
    fmm = ftask->mm;
    if (fmm)
        mmgrab(fmm);
    task_unlock(ftask);
    put_task_struct(ftask);
    ftask = NULL;
    if (!fmm) {
        ret = -ESRCH; /* exiting, or already lost its address space */
        goto err_poke;
    }

    for (i = 0; i < UHOOK_MAX; i++)
        if (!hooks[i].active) {
            h = &hooks[i];
            break;
        }
    if (!h) {
        ret = -ENOSPC;
        goto err_mm;
    }
    id = uh_alloc_id();
    if (!id) {
        ret = -EAGAIN;
        goto err_mm;
    }

    memset(h, 0, sizeof(*h));
    h->path = path;
    h->inode = inode;
    h->poke_data = poke;
    h->poke_len = poke_len;
    h->filter_mm = fmm;
    h->filter_tgid = ftgid;
    h->offset = cmd->offset;
    h->site = cmd->site;
    h->cond = cmd->cond;
    h->cond_reg = cmd->cond_reg;
    h->cond_cmp = cmd->cond_cmp;
    h->cond_len = cmd->cond_len;
    h->cond_off = cmd->cond_off;
    h->cond_val = cmd->cond_val;
    h->action = cmd->action;
    h->act_reg = cmd->act_reg;
    h->act_off = cmd->act_off;
    h->act_val = cmd->act_val;
    h->cap_regs = cmd->cap_regs;

    if (h->site == KSU_UHOOK_ON_RET)
        h->uc.ret_handler = uh_ret_handler;
    else
        h->uc.handler = uh_entry_handler;
    h->uc.filter = uh_filter;

    /* Fully initialised, id assigned, BEFORE the consumer is published:
     * uprobe_register() can start firing handlers before it returns, and a
     * capture record carrying id 0 could never be attributed. */
    h->id = id;
    h->active = true;

    ret = uh_uprobe_register(h);
    if (ret) {
        memset(h, 0, sizeof(*h));
        goto err_mm;
    }

    cmd->ret = h->id;
    cmd->arg1 = uh_mapped_count(h);
    pr_info("uhook: add id=%u off=0x%llx site=%u action=%u tgid=%d live_mms=%llu\n", h->id, (u64)h->offset, h->site,
            h->action, h->filter_tgid, cmd->arg1);
    if (!cmd->arg1)
        pr_warn("uhook: id=%u is armed but nothing maps that offset yet -- check the path and the offset\n", h->id);
    return 0;

err_task:
    if (ftask)
        put_task_struct(ftask);
err_mm:
    if (fmm)
        mmdrop(fmm);
err_poke:
    kfree(poke);
err_inode:
    iput(inode);
err_path:
    path_put(&path);
    return ret;
}

static int uh_read(struct ksu_uhook_cmd *cmd)
{
    void __user *ubuf = (void __user *)(uintptr_t)cmd->uptr;
    u32 want = cmd->len / sizeof(struct ksu_uhook_record);
    u32 done = 0;
    bool fault = false;
    unsigned long flags;

    if (!want)
        return -EINVAL;
    mutex_lock(&ring_read_lock);
    if (!ring) {
        mutex_unlock(&ring_read_lock);
        return -EINVAL;
    }
    while (done < want) {
        struct ksu_uhook_record rec;
        u64 seq;

        spin_lock_irqsave(&ring_lock, flags);
        if (ring_prod == ring_cons) {
            spin_unlock_irqrestore(&ring_lock, flags);
            break;
        }
        seq = ring_cons;
        rec = ring[seq % UHOOK_RING];
        spin_unlock_irqrestore(&ring_lock, flags);

        /* Copy out BEFORE consuming: a faulting copy must not destroy the
         * record. */
        if (copy_to_user(ubuf + done * sizeof(rec), &rec, sizeof(rec))) {
            if (!done)
                fault = true;
            break;
        }

        spin_lock_irqsave(&ring_lock, flags);
        /* Consume only if a producer overrun has not already accounted this
         * record as lost while the lock was dropped. Sequence numbers are
         * monotonic, so this cannot be fooled by a wrap. */
        if (ring_cons == seq)
            ring_cons++;
        spin_unlock_irqrestore(&ring_lock, flags);
        done++;
    }
    spin_lock_irqsave(&ring_lock, flags);
    cmd->lost = ring_lost;
    spin_unlock_irqrestore(&ring_lock, flags);
    mutex_unlock(&ring_read_lock);
    cmd->arg1 = done;
    cmd->ret = (s64)done * sizeof(struct ksu_uhook_record);
    /* A fault on the very first record is an error, not an empty drain: the
     * caller must be able to tell "your buffer is bad" from "nothing queued". */
    return fault ? -EFAULT : 0;
}

/* Serve KSU_UHOOK_LIST: count the active hooks, and fill the caller's optional
 * status buffer. Returns the number of rows written, or a negative errno; the
 * count goes to *active. */
static int uh_list_status(struct ksu_uhook_cmd *cmd, u32 *active)
{
    void __user *ubuf = (void __user *)(uintptr_t)cmd->uptr;
    struct ksu_uhook_status *st = NULL;
    unsigned long flags;
    u32 want, n = 0, cnt = 0;
    int i, ret = 0;

    want = ubuf ? (u32)min_t(u64, cmd->len / sizeof(*st), (u64)UHOOK_MAX) : 0;
    /* Snapshot into kernel memory first: copy_to_user() can fault, and holding
     * hooks_lock across a fault would let a bad user buffer stall every other
     * op on the table. */
    if (want) {
        st = kcalloc(want, sizeof(*st), GFP_KERNEL);
        if (!st)
            return -ENOMEM;
    }

    /* One critical section for both the rows and the count. Gathered under two
     * separate acquisitions they can disagree, and a caller comparing arg1
     * against ret to detect a truncated readback would see a difference that
     * means nothing. */
    mutex_lock(&hooks_lock);
    for (i = 0; i < UHOOK_MAX; i++) {
        if (!hooks[i].active)
            continue;
        cnt++;
        if (n >= want)
            continue;
        st[n].id = hooks[i].id;
        st[n].site = hooks[i].site;
        st[n].action = hooks[i].action;
        st[n].filter_tgid = hooks[i].filter_tgid;
        st[n].offset = (u64)hooks[i].offset;
        st[n].traps = hooks[i].traps;
        st[n].hits = hooks[i].hits;
        st[n].fails = hooks[i].fails;
        n++;
    }
    mutex_unlock(&hooks_lock);

    if (n && copy_to_user(ubuf, st, (size_t)n * sizeof(*st)))
        ret = -EFAULT;
    kfree(st);
    if (ret)
        return ret;

    *active = cnt;
    spin_lock_irqsave(&ring_lock, flags);
    cmd->lost = ring_lost;
    spin_unlock_irqrestore(&ring_lock, flags);
    return (int)n;
}

int ksu_uhook(struct ksu_uhook_cmd *cmd)
{
    int ret = 0, i;
    struct uhook *h;

    /* Every op copies the whole struct back to userspace, so an output field an
     * op never writes hands the caller its own input back dressed as a result.
     * Clear them here rather than relying on each op to remember. */
    cmd->ret = 0;
    cmd->arg1 = 0;
    cmd->lost = 0;

    switch (cmd->op) {
    case KSU_UHOOK_ADD:
        mutex_lock(&hooks_lock);
        ret = uh_add(cmd);
        mutex_unlock(&hooks_lock);
        break;
    case KSU_UHOOK_DEL:
        mutex_lock(&hooks_lock);
        h = uh_find(cmd->id);
        if (h)
            uh_free(h);
        else
            ret = -ENOENT;
        mutex_unlock(&hooks_lock);
        break;
    case KSU_UHOOK_CLEAR:
        mutex_lock(&hooks_lock);
        for (i = 0; i < UHOOK_MAX; i++)
            if (hooks[i].active) {
                uh_free(&hooks[i]);
                cmd->ret++; /* so "cleared nothing" is not reported as success */
            }
        mutex_unlock(&hooks_lock);
        break;
    case KSU_UHOOK_LIST: {
        /* The optional status rows are what make an empty capture ring
         * interpretable, so they are gathered before anything is reported: a
         * -EFAULT on the caller's buffer must not read back as an empty table. */
        u32 active = 0;

        ret = uh_list_status(cmd, &active);
        if (ret < 0)
            break;
        cmd->arg1 = (u64)ret; /* rows written */
        cmd->ret = active;
        ret = 0;
        break;
    }
    case KSU_UHOOK_READ:
        ret = uh_read(cmd);
        break;
    default:
        ret = -EINVAL;
    }
    return ret;
}

void ksu_uhook_init(void)
{
    p_find_task_by_vpid = (find_task_by_vpid_t)find_kernel_symbol_exact("find_task_by_vpid");
    p_uprobe_register = (uprobe_register_t)find_kernel_symbol_exact("uprobe_register");
#if UHOOK_NEW_UPROBE
    p_uprobe_unregister_nosync = (uprobe_unregister_nosync_t)find_kernel_symbol_exact("uprobe_unregister_nosync");
    p_uprobe_unregister_sync = (uprobe_unregister_sync_t)find_kernel_symbol_exact("uprobe_unregister_sync");
    uhook_ready = p_uprobe_register && p_uprobe_unregister_nosync && p_uprobe_unregister_sync;
#else
    p_uprobe_unregister = (uprobe_unregister_t)find_kernel_symbol_exact("uprobe_unregister");
    uhook_ready = p_uprobe_register && p_uprobe_unregister;
#endif
    ring = kvzalloc(sizeof(struct ksu_uhook_record) * UHOOK_RING, GFP_KERNEL);
    if (!ring)
        uhook_ready = false;
    if (!uhook_ready)
        pr_warn("uhook: disabled (uprobe register/unregister unavailable or no ring)\n");
    pr_info("uhook: init (ready=%d ring=%d)\n", uhook_ready, !!ring);
}

void ksu_uhook_exit(void)
{
    int i;

    mutex_lock(&hooks_lock);
    for (i = 0; i < UHOOK_MAX; i++)
        if (hooks[i].active)
            uh_free(&hooks[i]);
    mutex_unlock(&hooks_lock);
    uhook_ready = false;

    /* KSU_UHOOK_READ does not take hooks_lock, only ring_read_lock, so the ring
     * must be retired under that lock or an in-flight drain walks freed memory. */
    mutex_lock(&ring_read_lock);
    spin_lock_irq(&ring_lock);
    ring_prod = 0;
    ring_cons = 0;
    ring_lost = 0;
    spin_unlock_irq(&ring_lock);
    kvfree(ring);
    ring = NULL;
    mutex_unlock(&ring_read_lock);
}
