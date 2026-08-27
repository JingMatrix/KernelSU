// SPDX-License-Identifier: GPL-2.0
/*
 * ptctl - general process control / debug primitives exposed via KSU_IOCTL_PTCTL.
 *
 * These are kernel-unique capabilities userspace root cannot do:
 *   - read/write/inspect an arbitrary task's memory & registers WITHOUT ptrace
 *     (invisible to self-ptrace anti-debug; leaves TracerPid == 0),
 *   - block another process's lethal signals (neuter a watchdog's SIGKILL).
 *
 * Visibility, stated precisely: PEEK, GETREGS and the HWBP hold change no code
 * bytes, so a .text self-checksum sees nothing -- HWBP uses debug registers
 * only. POKE is the exception and is deliberately built to rewrite .text
 * (FOLL_WRITE|FOLL_FORCE, and access_process_vm's copy_to_user_page() flushes
 * the I-cache), so a checksum computed over the process's own mapping WILL see
 * a POKE. Only a checksum that re-reads the file from disk will not, because
 * the write COWs a private anonymous page.
 *
 * All ops are root-gated by the supercall perm check. Symbols are resolved via
 * kallsyms so this links whether built-in or as an LKM.
 */
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/kprobes.h>
#include <linux/signal.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/task_work.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/thread_info.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <asm/ptrace.h>
#include <asm/processor.h>
#if defined(CONFIG_ARM64)
#include <asm/debug-monitors.h> /* DBG_SPSR_SS */
#endif
#if defined(CONFIG_X86_64)
#include <asm/processor-flags.h> /* X86_EFLAGS_RF */
#endif

#include <linux/printk.h>
#include "uapi/supercall.h"
#include "feature/ptctl.h"
#include "infra/symbol_resolver.h"

#define PTCTL_MAX_CHUNK (64 * 1024)
#define PTCTL_MAX_GUARD 32

/* The user-visible register frame. On arm64 struct pt_regs carries 64 bytes of
 * kernel-private tail (orig_x0, syscallno, sdei_ttbr1, pmr_save, stackframe,
 * lockdep_hardirqs, exit_rcu) beyond struct user_pt_regs; only the latter may
 * cross the ioctl boundary in either direction. On x86_64 struct pt_regs IS the
 * user frame. */
#if defined(CONFIG_ARM64)
typedef struct user_pt_regs ksu_uregs_t;
#else
typedef struct pt_regs ksu_uregs_t;
#endif
#define KSU_UREGS_SZ (sizeof(ksu_uregs_t))

/* Three sched.h spellings this file needs are newer than the oldest GKI branch it
 * is built for. v5.14 renamed task_struct::state to ::__state and added
 * task_is_running(); v5.16 added task_call_func() together with the task_call_f
 * typedef its callback argument uses. android12-5.10 has none of the three, so
 * name them here rather than spelling the difference out at each use site. The
 * callback type gets our own name so it can never collide with the kernel's on a
 * version that declares one. */
typedef int (*ksu_task_call_f)(struct task_struct *p, void *arg);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
#define ksu_task_state(p) READ_ONCE((p)->state)
#define ksu_task_is_running(p) (ksu_task_state(p) == TASK_RUNNING)
#else
#define ksu_task_state(p) READ_ONCE((p)->__state)
#define ksu_task_is_running(p) task_is_running(p)
#endif

/* --- resolved kernel symbols --- */
typedef struct task_struct *(*find_task_by_vpid_t)(pid_t nr);
typedef int (*access_process_vm_t)(struct task_struct *tsk, unsigned long addr, void *buf, int len,
                                   unsigned int gup_flags);
/* NB: this is send_sig_info(), a different symbol from the 4-argument
 * do_send_sig_info() that the killguard kprobe is attached to. */
typedef int (*send_sig_info_t)(int sig, struct kernel_siginfo *info, struct task_struct *p);
typedef int (*register_kprobe_t)(struct kprobe *p);
typedef void (*unregister_kprobe_t)(struct kprobe *p);
typedef void (*put_task_stack_t)(struct task_struct *tsk);
typedef int (*task_call_func_t)(struct task_struct *p, ksu_task_call_f func, void *arg);
#if defined(CONFIG_ARM64)
typedef int (*valid_user_regs_t)(struct user_pt_regs *regs, struct task_struct *task);
#endif

static find_task_by_vpid_t p_find_task_by_vpid;
static access_process_vm_t p_access_process_vm;
static send_sig_info_t p_send_sig_info;
static register_kprobe_t p_register_kprobe;
static unregister_kprobe_t p_unregister_kprobe;
static put_task_stack_t p_put_task_stack;
static task_call_func_t p_task_call_func;
#if defined(CONFIG_ARM64)
static valid_user_regs_t p_valid_user_regs;
#endif

/* --- killguard state: protected tgids --- */
static DEFINE_SPINLOCK(guard_lock);
static pid_t guard_tgids[PTCTL_MAX_GUARD];
static int guard_count;

static bool is_guarded(pid_t tgid)
{
    int i;
    bool found = false;
    unsigned long flags;
    if (!READ_ONCE(guard_count))
        return false;
    spin_lock_irqsave(&guard_lock, flags);
    for (i = 0; i < guard_count; i++) {
        if (guard_tgids[i] == tgid) {
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&guard_lock, flags);
    return found;
}

static int guard_add(pid_t tgid)
{
    int i;
    unsigned long flags;
    spin_lock_irqsave(&guard_lock, flags);
    for (i = 0; i < guard_count; i++)
        if (guard_tgids[i] == tgid) {
            spin_unlock_irqrestore(&guard_lock, flags);
            return 0;
        }
    if (guard_count >= PTCTL_MAX_GUARD) {
        spin_unlock_irqrestore(&guard_lock, flags);
        return -ENOSPC;
    }
    guard_tgids[guard_count] = tgid;
    WRITE_ONCE(guard_count, guard_count + 1);
    spin_unlock_irqrestore(&guard_lock, flags);
    return 0;
}

static int guard_del(pid_t tgid)
{
    int i;
    unsigned long flags;
    spin_lock_irqsave(&guard_lock, flags);
    for (i = 0; i < guard_count; i++) {
        if (guard_tgids[i] == tgid) {
            guard_tgids[i] = guard_tgids[guard_count - 1];
            WRITE_ONCE(guard_count, guard_count - 1);
            spin_unlock_irqrestore(&guard_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&guard_lock, flags);
    return -ENOENT;
}

static void guard_clear(void)
{
    unsigned long flags;
    spin_lock_irqsave(&guard_lock, flags);
    WRITE_ONCE(guard_count, 0);
    spin_unlock_irqrestore(&guard_lock, flags);
}

/* Signals that a killer or watchdog actually uses to terminate a process and
 * that can be INJECTED from another task, i.e. that reach do_send_sig_info().
 *
 * Deliberately conservative. Every signal in this list is swallowed outright
 * for a guarded process, so widening it to "everything whose default action is
 * Term" would break the target's own runtime: bionic and ART rely on SIGRTMIN
 * and friends for posix timers, GC and thread suspension, and on SIGPIPE /
 * SIGALRM / SIGIO / SIGPROF during normal operation. Guarding a process must
 * not make it stop working.
 *
 * Two consequences the caller must understand:
 *   - a determined killer can still use any signal not listed here, and
 *   - the fault signals below are only blocked in their INJECTED form
 *     (kill(2)/tgkill(2)); a genuine SIGSEGV/SIGBUS/SIGILL/SIGFPE raised by the
 *     CPU is delivered through force_sig_info_to_task(), which never enters
 *     do_send_sig_info(), so it cannot be intercepted here at all.
 */
static inline bool sig_is_lethal(int sig)
{
    switch (sig) {
    case SIGHUP:
    case SIGINT:
    case SIGQUIT:
    case SIGABRT:
    case SIGTERM:
    case SIGKILL:
    case SIGILL:
    case SIGSEGV:
    case SIGBUS:
    case SIGFPE:
        return true;
    default:
        return false;
    }
}

/*
 * Abort the probed function from its kprobe pre_handler: hand @retval back to
 * the caller and resume at the caller's next instruction without executing one
 * instruction of the probed body.
 *
 * Both arches implement the same contract on the BREAKPOINT path --
 * arch/arm64/kernel/probes/kprobes.c kprobe_handler() and
 * arch/x86/kernel/kprobes/core.c kprobe_int3_handler():
 *
 *     if (!p->pre_handler || !p->pre_handler(p, regs))
 *             setup_singlestep(p, regs, kcb, 0);
 *     else
 *             reset_current_kprobe();
 *
 * A non-zero return means "the handler already redirected execution": the core
 * does not single-step and does not touch pc/ip, so the handler MUST have
 * changed it or the trap re-executes forever. This helper therefore returns 1
 * only on the paths that actually redirected.
 *
 * The x86_64 OPTPROBE path is a different story: opt_pre_handler() throws the
 * return value away and the trampoline discards pc/sp, so this helper would be
 * a silent no-op there. sig_kp carries a dummy .post_handler purely to keep
 * optimize_kprobe() from ever taking that path.
 *
 * Requires the probe to sit at function offset 0 (kprobe.symbol_name with no
 * .offset), so the prologue has not yet spilled the return address, and to be a
 * real breakpoint kprobe rather than an ftrace one -- both enforced at
 * registration by killguard_register().
 */
static __always_inline bool ptctl_is_kernel_va(unsigned long addr)
{
    /* kernel VAs are sign-extended on arm64 and x86_64; user VAs are not */
    return (long)addr < 0;
}

static int ptctl_kprobe_skip_function(struct pt_regs *regs, unsigned long retval)
{
#if defined(CONFIG_ARM64)
    /* x30 still holds the caller's raw return address: the BRK replaced the
     * very first instruction, so neither paciasp nor the frame spill has run
     * and the matching autiasp never will. Returning through raw x30 is
     * balanced. This is what arch/arm64/lib/error-inject.c does. */
    unsigned long lr = procedure_link_pointer(regs);

    if (!ptctl_is_kernel_va(lr))
        return 0;
    regs->regs[0] = retval;
    instruction_pointer_set(regs, lr);
    return 1;
#elif defined(CONFIG_X86_64)
    /* int3 is a plain interrupt gate here (no IST), so regs->sp is the
     * interrupted RSP, which at function entry points at the return address
     * pushed by `call`. Emulate `ret`: pop it into ip. iretq reloads RSP from
     * the frame, so the adjusted regs->sp is honoured. */
    unsigned long sp = regs->sp;
    unsigned long ra;

    if (!ptctl_is_kernel_va(sp) || (sp & (sizeof(unsigned long) - 1)))
        return 0;
    ra = *(unsigned long *)sp;
    if (!ptctl_is_kernel_va(ra))
        return 0;
    regs->ax = retval;
    regs->sp = sp + sizeof(unsigned long);
    instruction_pointer_set(regs, ra);
    return 1;
#else
    (void)regs;
    (void)retval;
    return 0; /* never claim to have redirected */
#endif
}

/*
 * KILLGUARD: drop a lethal signal aimed at a guarded process.
 *
 * This must SKIP do_send_sig_info() entirely; rewriting its `sig` argument to 0
 * does not work. do_send_sig_info() has no sig == 0 guard (the "no-op existence
 * check" belongs to kill(2)'s callers, which test `if (!ret && sig)` BEFORE
 * calling in), so a zero reaches sigaddset(&pending->signal, 0), which computes
 * 1UL << (0 - 1); AArch64 truncates the shift to 63, marking SIGRTMAX pending.
 * The target then dies from signal 64 instead of 9. It also makes
 * sig_task_ignored() read sighand->action[-1].
 */
static int sig_kp_pre(struct kprobe *p, struct pt_regs *regs)
{
    int sig;
    struct kernel_siginfo *info;
    struct task_struct *t;

    if (!READ_ONCE(guard_count))
        return 0;
#if defined(CONFIG_ARM64)
    /* do_send_sig_info(int sig, struct kernel_siginfo *info,
     *                  struct task_struct *p, enum pid_type type) */
    sig = (int)regs->regs[0];
    info = (struct kernel_siginfo *)regs->regs[1];
    t = (struct task_struct *)regs->regs[2];
#elif defined(CONFIG_X86_64)
    sig = (int)regs->di;
    info = (struct kernel_siginfo *)regs->si;
    t = (struct task_struct *)regs->dx;
#else
    return 0;
#endif
    /*
     * Never intercept a PRIVILEGED kernel-internal send. SEND_SIG_PRIV marks
     * the kills the kernel issues on its own behalf -- the OOM killer
     * (mm/oom_kill.c) and pid-namespace teardown
     * (zap_pid_ns_processes) -- as opposed to a signal injected for userspace,
     * which arrives with a real siginfo (kill(2), tgkill(2),
     * rt_sigqueueinfo(2), pidfd_send_signal(2)) or with SEND_SIG_NOINFO
     * (send_sig(), which is what cgroup.kill and Android's libprocessgroup
     * use).
     *
     * This is not politeness, it is the only safe rule. __oom_kill_process()
     * discards our return value and goes on to mark_oom_victim() +
     * queue_oom_reaper() unconditionally, so swallowing its SIGKILL would let
     * the reaper unmap a still-running process's anonymous memory -- every
     * later fault becomes a SIGBUS delivered through force_sig_info_to_task(),
     * which this probe cannot see -- and would pin oom_victims above zero so
     * that freeze_processes() -> oom_killer_disable() fails and the device can
     * no longer suspend. Blocking an OOM kill produces a strictly worse
     * outcome than allowing it. Guarding against a userspace watchdog, which
     * is what this feature is for, is unaffected.
     */
    if (info == SEND_SIG_PRIV)
        return 0;
    if (!t || !sig_is_lethal(sig))
        return 0;
    if (!is_guarded(t->tgid))
        return 0;

    /* Return 0 == "signal delivered", which is also the stealthy answer for
     * kill(2). group_send_sig_info() then runs trace_android_vh_killed_process()
     * and only reaps if a vendor hook sets reap AND task_will_free_mem() holds,
     * which requires SIGNAL_GROUP_EXIT or PF_EXITING -- false for a guarded,
     * healthy task. */
    return ptctl_kprobe_skip_function(regs, 0);
}

/*
 * Present ONLY to stop the probe from being jump-optimized. x86_64 selects
 * HAVE_OPTPROBES and CONFIG_OPTPROBES is def_bool y with no prompt, so
 * register_kprobe() -> try_to_optimize_kprobe() would replace the int3 with a
 * jmp into the optprobe trampoline a few jiffies after registration. On that
 * path opt_pre_handler() DISCARDS the pre_handler's return value and the
 * trampoline discards our regs->ip and regs->sp writes, so the redirect would
 * silently do nothing and KILLGUARD would report success while protecting
 * nothing. optimize_kprobe() refuses any probe that has a post_handler, which
 * pins us to the breakpoint path whose contract ptctl_kprobe_skip_function()
 * is written against. It is never invoked on the redirect path, because both
 * arches take the reset_current_kprobe() branch when pre_handler returns
 * non-zero, so it costs nothing.
 *
 * (KPROBE_FLAG_OPTIMIZED cannot be asserted instead: it is set on the aggr
 * kprobe and explicitly stripped from the copy of our flags, so it would read
 * clear forever.)
 */
static void sig_kp_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
}

static struct kprobe sig_kp = {
    .symbol_name = "do_send_sig_info",
    .pre_handler = sig_kp_pre,
    .post_handler = sig_kp_post,
};
static bool sig_kp_registered;

/*
 * Registered lazily, on the first KILLGUARD request. A BRK kprobe on
 * do_send_sig_info() makes EVERY signal send on the system take a debug
 * exception plus a single step, and it shows up in
 * /sys/kernel/debug/kprobes/list with the function's entry bytes no longer
 * matching the on-disk image. None of that should be imposed on a device whose
 * owner never asks for the feature.
 */
static DEFINE_MUTEX(killguard_lock);

static void killguard_register(void)
{
    if (!p_register_kprobe || !p_unregister_kprobe)
        return;
    if (p_register_kprobe(&sig_kp) != 0) {
        pr_err("ptctl: killguard kprobe on do_send_sig_info failed\n");
        return;
    }
    /* arm64 never selects HAVE_KPROBES_ON_FTRACE and GKI builds without
     * FUNCTION_TRACER, so this is normally impossible. But on an ftrace-based
     * kprobe the probe sits at __fentry__, where neither x30 nor *(regs->sp)
     * describes the caller's frame -- refuse rather than corrupt the stack. */
    if (sig_kp.flags & KPROBE_FLAG_FTRACE) {
        p_unregister_kprobe(&sig_kp);
        pr_err("ptctl: killguard disabled (do_send_sig_info is ftrace-probed)\n");
        return;
    }
    sig_kp_registered = true;
}

static bool killguard_ensure(void)
{
    if (READ_ONCE(sig_kp_registered))
        return true;
    mutex_lock(&killguard_lock);
    if (!sig_kp_registered)
        killguard_register();
    mutex_unlock(&killguard_lock);
    return READ_ONCE(sig_kp_registered);
}

/* --- task lookup helper: returns a referenced task or NULL --- */
static struct task_struct *get_task(pid_t pid)
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

/* --- memory read/write via access_process_vm (chunked by userspace) --- */
static long do_peek(struct ksu_ptctl_cmd *c, bool write)
{
    struct task_struct *task;
    void *kbuf;
    size_t len = c->len;
    long n;

    if (!p_access_process_vm)
        return -ENOSYS;
    if (!len || len > PTCTL_MAX_CHUNK)
        return -EINVAL;
    task = get_task(c->pid);
    if (!task)
        return -ESRCH;
    kbuf = kvmalloc(len, GFP_KERNEL);
    if (!kbuf) {
        put_task_struct(task);
        return -ENOMEM;
    }

    if (write) {
        if (copy_from_user(kbuf, (void __user *)(uintptr_t)c->uptr, len)) {
            n = -EFAULT;
            goto out;
        }
        n = p_access_process_vm(task, (unsigned long)c->addr, kbuf, (int)len, FOLL_WRITE | FOLL_FORCE);
    } else {
        /* FOLL_FORCE on the read side too, so PEEK and POKE cover the same
         * pages -- ptrace does the same. Without it a PROT_NONE guard page or
         * an execute-only mapping is writable but not readable. */
        n = p_access_process_vm(task, (unsigned long)c->addr, kbuf, (int)len, FOLL_FORCE);
        if (n > 0 && copy_to_user((void __user *)(uintptr_t)c->uptr, kbuf, n)) {
            n = -EFAULT;
            goto out;
        }
    }
    if (n == 0) {
        /* access_process_vm never returns a negative errno; it reports total
         * failure (no mm, unmapped, refused) as a zero byte count. Surface
         * that as an error instead of a successful zero-length transfer. */
        n = -EIO;
        goto out;
    }
    c->ret = n;
out:
    kvfree(kbuf);
    put_task_struct(task);
    return (n < 0) ? n : 0;
}

/* ------------------------------------------------------------------ *
 * User register access.
 *
 * Only the user-visible frame crosses the boundary, and an incoming frame is
 * sanitised before it is committed -- the kernel's own PTRACE_SETREGSET path
 * runs valid_user_regs() unconditionally, and skipping it turns a root ioctl
 * into an EL1 escalation (a forged SPSR mode field plus an arbitrary pc is
 * ERET'd straight out of kernel_exit).
 * ------------------------------------------------------------------ */
#if defined(CONFIG_ARM64)
/* Verbatim from arch/arm64/kernel/ptrace.c; those macros are .c-local. */
#define KSU_SPSR_EL1_AARCH64_RES0_BITS                                                                                 \
    (GENMASK_ULL(63, 32) | GENMASK_ULL(27, 26) | GENMASK_ULL(23, 22) | GENMASK_ULL(20, 13) | GENMASK_ULL(5, 5))

static int ksu_valid_native_regs_fallback(struct user_pt_regs *regs)
{
    regs->pstate &= ~KSU_SPSR_EL1_AARCH64_RES0_BITS;

    if (user_mode(regs) && !(regs->pstate & PSR_MODE32_BIT) && (regs->pstate & PSR_D_BIT) == 0 &&
        (regs->pstate & PSR_A_BIT) == 0 && (regs->pstate & PSR_I_BIT) == 0 && (regs->pstate & PSR_F_BIT) == 0)
        return 1;

    /* Force PSR to a valid 64-bit EL0t */
    regs->pstate &= PSR_N_BIT | PSR_Z_BIT | PSR_C_BIT | PSR_V_BIT;
    return 0;
}
#endif

/* Returns 0 if @n is (or was made) a legal user frame, -EINVAL if the caller
 * supplied something the kernel would have rejected. */
static int __nocfi ksu_sanitise_user_regs(ksu_uregs_t *n, const struct pt_regs *live, struct task_struct *task)
{
#if defined(CONFIG_ARM64)
    (void)live;
    if (p_valid_user_regs)
        return p_valid_user_regs(n, task) ? 0 : -EINVAL;

    /* Fallback: refuse compat tasks rather than reimplement the AArch32
     * variant, then apply the native check verbatim. */
    if (test_tsk_thread_flag(task, TIF_32BIT))
        return -EOPNOTSUPP;
    /* open-coded user_regs_reset_single_step(): the SS bit is kernel state,
     * never the caller's to set, and clobbering it strands a thread that the
     * HW-breakpoint hold armed for a single step. */
    if (test_tsk_thread_flag(task, TIF_SINGLESTEP))
        n->pstate |= DBG_SPSR_SS;
    else
        n->pstate &= ~DBG_SPSR_SS;
    return ksu_valid_native_regs_fallback(n) ? 0 : -EINVAL;
#elif defined(CONFIG_X86_64)
    /* Nothing to do here: on x86_64 the segment selectors, the syscall number
     * and the non-user EFLAGS bits are pinned from the live frame inside
     * ksu_regs_xfer_cb(), under the task_call_func() pin. */
    (void)n;
    (void)live;
    (void)task;
    return 0;
#else
    (void)n;
    (void)live;
    (void)task;
    return -EOPNOTSUPP;
#endif
}

/* FLAG_MASK from arch/x86/kernel/ptrace.c: the EFLAGS bits ptrace lets
 * userspace own, minus TF -- ptrace couples TF with TIF_FORCED_TF and a bulk
 * write cannot express that. */
#define KSU_X86_FLAG_MASK (0x54dd5UL & ~(unsigned long)X86_EFLAGS_TF)

struct ksu_regs_xfer {
    struct pt_regs *regs;
    ksu_uregs_t *buf;
    bool write;
};

/*
 * Runs under task_call_func(), i.e. with the target's pi_lock held and IRQs
 * disabled. It must not sleep; a 272-byte struct copy is fine.
 */
static int ksu_regs_xfer_cb(struct task_struct *p, void *arg)
{
    struct ksu_regs_xfer *a = arg;
    unsigned long st = ksu_task_state(p);

    /* task_call_func() pins the task but explicitly does NOT stop one that is
     * running -- it only holds off de-schedule. Refuse unless the task is
     * genuinely off-CPU: otherwise a concurrent kernel_entry overwrites the
     * same frame, so a write is lost and a read is torn. */
    if (st == TASK_RUNNING || st == TASK_WAKING || p->on_rq)
        return -EBUSY;
#if defined(CONFIG_ARM64)
    /* Off-CPU is necessary but not sufficient for a WRITE: the usual off-CPU
     * state for an Android thread is blocked inside a restartable syscall,
     * where the frame is not final. On resume syscall_set_return_value()
     * rewrites x0, and do_signal() can rewind pc by 4 and restore x0 from
     * orig_x0. Both of those fields live outside the 272-byte user view, so
     * the caller cannot suppress the rewrite the way a full-pt_regs write
     * once could. Refuse rather than hand back a frame the kernel will undo. */
    if (a->write && in_syscall(a->regs))
        return -EBUSY;
#elif defined(CONFIG_X86_64)
    /* x86's equivalent: syscall_get_nr() is regs->orig_ax and a syscall frame
     * is the one with a non-negative value. */
    if (a->write && (long)a->regs->orig_ax >= 0)
        return -EBUSY;
#endif
    if (a->write) {
#if defined(CONFIG_ARM64)
        a->regs->user_regs = *a->buf;
#else
        /* Pin the fields the caller does not own from the LIVE frame, here
         * rather than in the sanitiser: sampling them before the task was
         * pinned would copy a frame that could still change underneath. */
        a->buf->cs = a->regs->cs;
        a->buf->ss = a->regs->ss;
        a->buf->orig_ax = a->regs->orig_ax;
        a->buf->flags = (a->regs->flags & ~KSU_X86_FLAG_MASK) | (a->buf->flags & KSU_X86_FLAG_MASK);
        *a->regs = *a->buf;
#endif
    } else {
#if defined(CONFIG_ARM64)
        *a->buf = a->regs->user_regs;
#else
        *a->buf = *a->regs;
#endif
    }
    return 0;
}

static long __nocfi do_regs(struct ksu_ptctl_cmd *c, bool write)
{
    struct task_struct *task;
    struct pt_regs *regs;
    ksu_uregs_t buf;
    struct ksu_regs_xfer a;
    long ret = 0;

    /* len 0 means "the whole user frame"; anything else must match exactly,
     * so a caller sized for the old struct pt_regs gets a hard error rather
     * than a silent 64-byte overrun of the kernel-private tail. */
    if (c->len && c->len != KSU_UREGS_SZ)
        return -EINVAL;
    /* Without task_call_func() there is no way to pin the target off-CPU, and
     * an unpinned frame is neither safe to write nor meaningful to read. */
    if (!p_task_call_func)
        return -ENOSYS;

    task = get_task(c->pid);
    if (!task)
        return -ESRCH;
    if (task->flags & PF_KTHREAD) {
        put_task_struct(task);
        return -EINVAL;
    }
    /* Rewriting our own frame from inside the ioctl is the self-escalation
     * vector; x86's ptrace refuses the same thing. */
    if (write && task == current) {
        put_task_struct(task);
        return -EINVAL;
    }
    /* get_task_struct() pins the task_struct but NOT its kernel stack, which
     * is separately refcounted under THREAD_INFO_IN_TASK and released from
     * finish_task_switch(). task_pt_regs() points into that stack. */
#ifdef CONFIG_THREAD_INFO_IN_TASK
    /* try_get_task_stack()/put_task_stack() only refcount under this option;
     * elsewhere they are no-op inlines and the symbol does not exist. */
    if (!p_put_task_stack) {
        put_task_struct(task);
        return -ENOSYS;
    }
#endif
    if (!try_get_task_stack(task)) {
        put_task_struct(task);
        return -ESRCH;
    }
    regs = task_pt_regs(task);
    a.regs = regs;
    a.buf = &buf;
    a.write = write;

    if (write) {
        if (copy_from_user(&buf, (void __user *)(uintptr_t)c->uptr, KSU_UREGS_SZ)) {
            ret = -EFAULT;
            goto out;
        }
        /* Sanitise into the local copy first: a faulting or rejected write
         * must leave the target's frame untouched. */
        ret = ksu_sanitise_user_regs(&buf, regs, task);
        if (ret)
            goto out;
        ret = p_task_call_func(task, ksu_regs_xfer_cb, &a);
        if (ret)
            goto out;
    } else if (task == current) {
        /* Our own entry frame is stable from our own point of view, and
         * task_call_func() would always refuse it (current is on_rq). */
#if defined(CONFIG_ARM64)
        buf = regs->user_regs;
#else
        buf = *regs;
#endif
        if (copy_to_user((void __user *)(uintptr_t)c->uptr, &buf, KSU_UREGS_SZ)) {
            ret = -EFAULT;
            goto out;
        }
    } else {
        /* Read under the same pin, so the caller gets -EBUSY instead of a
         * torn or stale frame from a thread that is still running. */
        ret = p_task_call_func(task, ksu_regs_xfer_cb, &a);
        if (ret)
            goto out;
        if (copy_to_user((void __user *)(uintptr_t)c->uptr, &buf, KSU_UREGS_SZ)) {
            ret = -EFAULT;
            goto out;
        }
    }
    c->ret = KSU_UREGS_SZ;
out:
#ifdef CONFIG_THREAD_INFO_IN_TASK
    p_put_task_stack(task);
#else
    put_task_stack(task);
#endif
    put_task_struct(task);
    return ret;
}

static long do_info(struct ksu_ptctl_cmd *c)
{
    struct task_struct *task = get_task(c->pid);
    if (!task) {
        c->ret = 0;
        return 0;
    }
    c->arg2 = task->tgid;
    rcu_read_lock();
    /* ->parent is the tracer while ->ptrace is set (this is what
     * ptrace_parent() does); the field is __rcu, so dereference it as such. */
    c->arg1 = task->ptrace ? task_pid_nr(rcu_dereference(task->parent)) : 0;
    rcu_read_unlock();
    c->ret = 1;
    put_task_struct(task);
    return 0;
}

static long do_sigsend(struct ksu_ptctl_cmd *c)
{
    struct task_struct *task;
    int sig = (int)c->arg1;
    int ret;
    if (!p_send_sig_info)
        return -ENOSYS;
    /* Signal 0 is a no-op only in kill(2), which checks for it BEFORE calling
     * into the signal core. Passed down here it reaches
     * sigaddset(&pending->signal, 0), i.e. 1UL << (0 - 1), which sets bit 63
     * and marks SIGRTMAX pending -- it would kill the target. Use
     * KSU_PTCTL_INFO for an existence check instead. */
    if (sig <= 0 || sig >= _NSIG)
        return -EINVAL;
    task = get_task(c->pid);
    if (!task)
        return -ESRCH;
    ret = p_send_sig_info(sig, SEND_SIG_PRIV, task);
    c->ret = ret;
    put_task_struct(task);
    return 0;
}

/* KILLGUARD takes a pid or tid and guards the whole thread group. */
static long do_killguard(struct ksu_ptctl_cmd *c)
{
    struct task_struct *task;
    pid_t tgid;

    if (!killguard_ensure())
        return -ENOSYS;
    task = get_task(c->pid);
    if (task) {
        tgid = task->tgid;
        put_task_struct(task);
    } else if (c->arg1) {
        return -ESRCH; /* adding requires a live task */
    } else {
        /* Removal by raw tgid still works after the process is gone. Note the
         * table holds bare tgids with no exit hook, so an entry left behind by
         * a process that died can be inherited by a later process that
         * recycles the same tgid -- remove a guard when you are done with it. */
        tgid = (pid_t)c->pid;
    }
    c->arg2 = tgid;
    return c->arg1 ? guard_add(tgid) : guard_del(tgid);
}

/* ------------------------------------------------------------------ *
 * HWBP: a kernel HW breakpoint that PAUSES the hitting thread so peek/  *
 * poke/regs can inspect and step obfuscated code, then release. The     *
 * overflow handler runs in debug-exception context (IRQs masked and     *
 * preemption disabled, though not NMI), so it defers the actual pause   *
 * to task_work (return-to-userspace, sleepable). Because task_work runs *
 * inside the same EL0 exception, before the ERET, task_pt_regs() of the *
 * held thread IS the live frame it is about to resume with.             *
 * ------------------------------------------------------------------ */
typedef struct perf_event *(*reg_hwbp_t)(struct perf_event_attr *, perf_overflow_handler_t, void *,
                                         struct task_struct *);
typedef void (*unreg_hwbp_t)(struct perf_event *);
typedef void (*perf_event_enable_t)(struct perf_event *);
static reg_hwbp_t p_reg_hwbp;
static unreg_hwbp_t p_unreg_hwbp;
static perf_overflow_handler_t p_perf_event_output_forward; /* stored, never called */
static perf_event_enable_t p_perf_event_enable;

#define HWBP_MAX 640
static struct perf_event **hwbp_ev;
static int hwbp_n;
static DEFINE_MUTEX(hwbp_lock);
static DECLARE_COMPLETION(hwbp_hit);
static struct completion hwbp_release;
static struct completion hwbp_gone; /* hwbp_hold_fn() has left module text */
static struct pt_regs hwbp_regs;
static pid_t hwbp_tid;
static struct task_struct *hwbp_held;
static atomic_t hwbp_busy = ATOMIC_INIT(0);
static struct callback_head hwbp_work;
/* Hits thrown away because a hold was already in progress, or because the
 * hitting thread was too far into exit() to take task_work. Only one thread can
 * be parked at a time, so on a multi-threaded target this is normal and often
 * large -- but a caller who does not know it happened will read one hit per
 * WAIT and conclude the site is single-threaded. Reported by HWBP_CLEAR.
 * Not printed: the drop happens in debug-exception context. */
static atomic_t hwbp_missed = ATOMIC_INIT(0);

/*
 * Make arm64's breakpoint_handler() run its step-past-the-breakpoint machinery
 * for an event that carries OUR overflow handler.
 *
 * arch/arm64/kernel/hw_breakpoint.c gates the whole dance (bps_disabled = 1,
 * toggle_bp_registers(BCR, EL0, 0), user_enable_single_step(current)) on
 * uses_default_overflow_handler(bp), which with CONFIG_BPF_SYSCALL is
 *
 *     is_default_overflow_handler(event) ||
 *     __is_default_overflow_handler(event->orig_overflow_handler)
 *
 * and __is_default_overflow_handler() is a pure pointer compare against
 * perf_event_output_forward / _backward. Planting perf_event_output_forward in
 * ->orig_overflow_handler flips that gate to true while ->overflow_handler
 * stays hwbp_handler, which is what __perf_event_overflow() actually invokes.
 * Without this the BCR is left enabled and no single step is armed, so the
 * thread ERETs onto the same instruction and re-traps forever: HWBP_RELEASE
 * could never make it advance.
 *
 * The stored pointer is never CALLED. Its only call site is
 * bpf_overflow_handler(), which runs only when installed as ->overflow_handler
 * by perf_event_set_bpf_handler() -- reachable only through a perf fd, which a
 * kernel counter does not have. Teardown is safe: perf_event_free_bpf_handler()
 * early-returns on event->prog == NULL, and perf_event_alloc() kzallocs.
 *
 * The field only exists where a bpf_prog can displace ->overflow_handler, so it
 * is under CONFIG_BPF_SYSCALL, and only up to v6.11: v6.12 calls the BPF handler
 * directly and dropped both ->orig_overflow_handler and
 * uses_default_overflow_handler(). Where the field is absent the gate cannot be
 * flipped from here and hwbp_prepare_step() does the dance by hand instead.
 */
#if defined(CONFIG_BPF_SYSCALL) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
#define KSU_HAVE_ORIG_OVERFLOW_HANDLER 1
#endif

static void hwbp_mark_default_step(struct perf_event *bp)
{
#ifdef KSU_HAVE_ORIG_OVERFLOW_HANDLER
    if (p_perf_event_output_forward)
        WRITE_ONCE(bp->orig_overflow_handler, p_perf_event_output_forward);
#else
    (void)bp;
#endif
}

/*
 * Normally a no-op: breakpoint_handler() already disabled the breakpoint and
 * armed the step because hwbp_mark_default_step() flipped the gate. The branch
 * fires on the two kinds of kernel where that write cannot happen -- one whose
 * perf_event.h predates uses_default_overflow_handler(), and v6.12 and later,
 * which removed ->orig_overflow_handler -- and there we mirror hw_breakpoint.c
 * by hand. toggle_bp_registers() is static and
 * unreachable, but hw_breakpoint_control() recomputes the BCR enable bit from
 * debug_info->bps_disabled on every install and hw_breakpoint_thread_switch()
 * does the same on __switch_to, so one guaranteed sleep turns the register off.
 */
static bool hwbp_prepare_step(void)
{
#if defined(CONFIG_ARM64)
    struct debug_info *di = &current->thread.debug;

    if (di->bps_disabled)
        return false; /* the kernel did the dance for us */

    di->bps_disabled = 1;
    if (test_thread_flag(TIF_SINGLESTEP))
        di->suspended_step = 1;
    else
        set_thread_flag(TIF_SINGLESTEP);
    return true;
#else
    return false;
#endif
}

/*
 * The last thing before the thread resumes onto the trapped instruction:
 * re-assert the single bit that makes it advance. breakpoint_handler() sets it
 * AFTER perf_bp_event(), i.e. after our snapshot and before our task_work, so a
 * KSU_PTCTL_SETREGS during the hold would otherwise clobber it and reintroduce
 * the re-trap loop. This is user_rewind_single_step() open-coded, so no symbol
 * lookup is needed.
 */
static void hwbp_resume_step(void)
{
    struct pt_regs *regs = task_pt_regs(current);

#if defined(CONFIG_ARM64)
    if (test_thread_flag(TIF_SINGLESTEP))
        regs->pstate |= DBG_SPSR_SS;
#elif defined(CONFIG_X86_64)
    regs->flags |= X86_EFLAGS_RF;
#else
    (void)regs;
#endif
}

static void hwbp_hold_fn(struct callback_head *cb)
{
    bool manual = hwbp_prepare_step(); /* before parking */

    /* Publish the hit HERE, not from the overflow handler: only now is the
     * thread about to become genuinely off-CPU, which is what do_regs()'s
     * commit gate requires. Completing it from the debug exception would let
     * HWBP_WAIT return while the target is still TASK_RUNNING, and the
     * SETREGS that the caller is documented to issue next would race it. */
    complete(&hwbp_hit);

    wait_for_completion_interruptible(&hwbp_release); /* pause here until released */

    if (manual) {
        /* Force at least one sched-out/sched-in so hw_breakpoint_control()
         * reprograms the BCR with the enable bit clear; the wait above can
         * return without ever having slept. */
        schedule_timeout_uninterruptible(1);
    }
    hwbp_resume_step();

    WRITE_ONCE(hwbp_held, NULL);
    atomic_set(&hwbp_busy, 0);
    complete(&hwbp_gone);
    /* Must be last: the module reference taken in hwbp_handler() is what keeps
     * rmmod from freeing this function while a thread is parked in it. */
    module_put(THIS_MODULE);
}

static void hwbp_handler(struct perf_event *bp, struct perf_sample_data *data, struct pt_regs *regs)
{
    if (atomic_cmpxchg(&hwbp_busy, 0, 1) != 0) {
        atomic_inc(&hwbp_missed); /* one hold at a time */
        return;
    }
    /* Pin the module for the whole hold: hwbp_hold_fn() is module text and a
     * thread can sit in it indefinitely, so an outstanding hold must make
     * rmmod fail rather than race a completion against free_module(). */
    if (!try_module_get(THIS_MODULE)) {
        atomic_inc(&hwbp_missed);
        atomic_set(&hwbp_busy, 0);
        return;
    }
    /* the true hit state: the step bits are not set until after we return */
    memcpy(&hwbp_regs, regs, sizeof(hwbp_regs));
    hwbp_tid = current->pid;
    /* Re-arm the completions BEFORE publishing hwbp_held, or a HWBP_RELEASE
     * that observes hwbp_held can have its permit erased by the reinit. */
    reinit_completion(&hwbp_release);
    reinit_completion(&hwbp_gone);
    init_task_work(&hwbp_work, hwbp_hold_fn);
    WRITE_ONCE(hwbp_held, current);
    if (task_work_add(current, &hwbp_work, TWA_RESUME) != 0) {
        atomic_inc(&hwbp_missed);
        WRITE_ONCE(hwbp_held, NULL);
        atomic_set(&hwbp_busy, 0);
        module_put(THIS_MODULE);
        return;
    }
    /* hwbp_hit is completed from hwbp_hold_fn(), once the thread has parked. */
}

static long hwbp_set(struct ksu_ptctl_cmd *c, u64 addr, pid_t tgid)
{
    struct task_struct *leader, *t, **tasks;
    struct perf_event_attr attr;
    int cnt = 0, total = 0, armed, i;

    /* unregister must be resolvable too: arming events we cannot retire is a
     * use-after-free across module unload */
    if (!p_reg_hwbp || !p_unreg_hwbp)
        return -ENOSYS;
#if defined(CONFIG_ARM64)
    /* An unaligned aarch64 execute breakpoint is not rejected by the arch code:
     * hw_breakpoint_arch_parse() masks the address down and shifts BAS, after
     * which breakpoint_handler()'s own match test can never succeed. That is a
     * breakpoint which registers cleanly and then silently never fires. */
    if (addr & 3)
        return -EINVAL;
#endif
    mutex_lock(&hwbp_lock);
    /* hwbp_held is checked as well as hwbp_n: a previous HWBP_CLEAR may have
     * disarmed while a thread was still parked, and clearing the busy
     * interlock underneath it would allow a second concurrent hold. */
    if (hwbp_n || READ_ONCE(hwbp_held) || atomic_read(&hwbp_busy)) {
        mutex_unlock(&hwbp_lock);
        return -EBUSY;
    }
    leader = get_task(tgid);
    if (!leader) {
        mutex_unlock(&hwbp_lock);
        return -ESRCH;
    }

    tasks = kcalloc(HWBP_MAX, sizeof(*tasks), GFP_KERNEL);
    if (!tasks) {
        put_task_struct(leader);
        mutex_unlock(&hwbp_lock);
        return -ENOMEM;
    }
    /* The whole group is counted even past HWBP_MAX, because `total` is what
     * tells the caller the arm was incomplete; stopping the count at the cap
     * would make a truncated arm look like a complete one. */
    rcu_read_lock();
    for_each_thread (leader, t) {
        total++;
        if (cnt >= HWBP_MAX)
            continue;
        get_task_struct(t);
        tasks[cnt++] = t;
    }
    rcu_read_unlock();
    put_task_struct(leader);

    hwbp_ev = kcalloc(cnt, sizeof(*hwbp_ev), GFP_KERNEL);
    if (!hwbp_ev) {
        for (i = 0; i < cnt; i++)
            put_task_struct(tasks[i]);
        kfree(tasks);
        mutex_unlock(&hwbp_lock);
        return -ENOMEM;
    }

    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_addr = addr;
    attr.bp_type = HW_BREAKPOINT_X;
#if defined(CONFIG_ARM64)
    attr.bp_len = HW_BREAKPOINT_LEN_4;
#else
    /* x86's arch_build_bp_info() accepts an execute breakpoint only when
     * bp_len == sizeof(long); anything else falls through to -EINVAL. */
    attr.bp_len = sizeof(long);
#endif
    attr.sample_period = 1;
    attr.exclude_kernel = 1;
    /* Register disabled so ->orig_overflow_handler is planted before the first
     * possible hit. If perf_event_enable could not be resolved, register live
     * and accept a window in which one early hit re-traps once. */
    attr.disabled = p_perf_event_enable ? 1 : 0;

    /* Reset the hold state BEFORE anything is armed. Doing it after the loop
     * (as this used to) races every hit taken while the remaining threads are
     * still being registered -- each iteration does sleepable work with
     * cross-CPU IPIs, so that window is milliseconds wide on a big app -- and
     * clearing hwbp_busy under a live hold is what lets the single static
     * hwbp_work be linked into two task_work lists at once. Safe here because
     * hwbp_lock is held, hwbp_n == 0 and hwbp_held == NULL. */
    reinit_completion(&hwbp_hit);
    atomic_set(&hwbp_busy, 0);
    atomic_set(&hwbp_missed, 0);

    hwbp_n = 0;
    for (i = 0; i < cnt; i++) {
        struct perf_event *ev = p_reg_hwbp(&attr, hwbp_handler, NULL, tasks[i]);
        if (!IS_ERR(ev)) {
            hwbp_mark_default_step(ev);
            if (attr.disabled)
                p_perf_event_enable(ev);
            hwbp_ev[hwbp_n++] = ev;
        }
        put_task_struct(tasks[i]);
    }
    kfree(tasks);
    if (!hwbp_n) {
        /* nothing armed: drop the array instead of leaking it into the next
         * HWBP_SET, which passes the `if (hwbp_n)` busy check and overwrites */
        kfree(hwbp_ev);
        hwbp_ev = NULL;
        mutex_unlock(&hwbp_lock);
        return -EINVAL;
    }
    /* Report the arm to the CALLER, not just to dmesg. Two ways this comes back
     * short of `total`, both of which used to be invisible: an individual
     * register_user_hw_breakpoint() can fail (a thread already using its four
     * debug registers, or exiting) and is skipped above, and the group can be
     * larger than HWBP_MAX. Either way the breakpoint is live on some threads
     * and absent on others, so a site that only the unarmed threads execute
     * reads back as a site nothing executes. */
    armed = hwbp_n;
    c->ret = armed;
    c->arg2 = total;
    mutex_unlock(&hwbp_lock);
    if (armed < total)
        pr_warn("ptctl: hwbp armed %d of %d thread(s) @0x%llx; the rest are unwatched\n", armed, total, addr);
    else
        pr_info("ptctl: hwbp armed on %d thread(s) @0x%llx\n", armed, addr);
    return 0;
}

static void hwbp_clear(void)
{
    int i;

    /* Disarm FIRST. Draining the hold while the breakpoints are still live
     * lets another thread trap and start a fresh hold behind our back, so the
     * drain below would never converge. */
    mutex_lock(&hwbp_lock);
    for (i = 0; i < hwbp_n; i++)
        if (hwbp_ev[i] && p_unreg_hwbp)
            p_unreg_hwbp(hwbp_ev[i]);
    kfree(hwbp_ev);
    hwbp_ev = NULL;
    hwbp_n = 0;
    mutex_unlock(&hwbp_lock);

    /* Then release whoever is already parked. hwbp_hold_fn() never takes
     * hwbp_lock, so waiting with it dropped cannot deadlock. The module
     * reference the holder owns is what actually prevents rmmod from freeing
     * hwbp_hold_fn(); this wait only makes HWBP_CLEAR synchronous for the
     * caller, so a timeout here is a diagnostic, not a safety hole. */
    if (READ_ONCE(hwbp_held)) {
        complete(&hwbp_release);
        if (!wait_for_completion_timeout(&hwbp_gone, 5 * HZ))
            pr_warn("ptctl: held thread has not resumed yet; module stays pinned\n");
    }
}

static long hwbp_wait(struct ksu_ptctl_cmd *c)
{
    long jit, r;

    /* Refuse to wait on nothing. A WAIT with no breakpoint armed -- because
     * HWBP_SET failed, or a HWBP_CLEAR got here first -- would block for the
     * whole timeout and then report a clean ret = 0, which is byte for byte
     * what "the target never executes that address" looks like. That is the
     * single most misleading answer this file can give, so it is an error
     * instead. (A CLEAR racing an already-running WAIT still times out; only
     * the state at entry is checked.) */
    if (!READ_ONCE(hwbp_n) && !READ_ONCE(hwbp_held))
        return -ENOENT;

    jit = c->arg1 ? msecs_to_jiffies((unsigned int)min_t(u64, c->arg1, UINT_MAX)) : MAX_SCHEDULE_TIMEOUT;
    r = wait_for_completion_interruptible_timeout(&hwbp_hit, jit);

    if (r <= 0) {
        c->ret = 0;
        return r < 0 ? r : 0;
    }
    /* Report the hit before attempting the register copy: the completion has
     * already been consumed, so failing here without telling the caller which
     * thread is parked would strand it. */
    c->arg2 = hwbp_tid;
    c->ret = 1;

    /* hwbp_hold_fn() completes hwbp_hit immediately before it blocks, so the
     * waiter can be running here while the holder is still TASK_RUNNING on its
     * way into wait_for_completion(). do_regs()'s commit gate would refuse it
     * with -EBUSY, breaking the documented "a parked thread always qualifies".
     * Wait, briefly and by reference, for it to actually leave the runqueue. */
    {
        struct task_struct *held = get_task(hwbp_tid);
        int i;

        if (held) {
            for (i = 0; i < 200; i++) { /* ~20ms worst case */
                if (!READ_ONCE(hwbp_held))
                    break; /* released or aborted already */
                if (!ksu_task_is_running(held) && !held->on_rq)
                    break;
                usleep_range(50, 150);
            }
            put_task_struct(held);
        }
        /* The park is an interruptible sleep and hwbp_hold_fn() publishes the
         * hit just before blocking, so a signal already pending on the hitting
         * thread ends the hold before it ever parks. Report that rather than
         * letting the caller assume a thread it can inspect. */
        c->arg1 = READ_ONCE(hwbp_held) ? 1 : 0;
    }
    if (c->uptr) {
        /* Same length contract as GETREGS. This used to clamp to c->len, so a
         * caller sized for the old struct pt_regs must get a hard error rather
         * than a 272-byte write into a smaller buffer. */
        if (c->len && c->len != KSU_UREGS_SZ)
            return -EINVAL;
        /* only the user-visible frame, never the kernel-private tail */
        if (copy_to_user((void __user *)(uintptr_t)c->uptr,
#if defined(CONFIG_ARM64)
                         &hwbp_regs.user_regs,
#else
                         &hwbp_regs,
#endif
                         KSU_UREGS_SZ))
            return -EFAULT;
    }
    return 0;
}

static long hwbp_release_held(void)
{
    /* Releasing with nobody parked would bank a completion and make the NEXT
     * hold return instantly -- the breakpoint would look like it was ignored. */
    if (!READ_ONCE(hwbp_held))
        return -ENOENT;
    complete(&hwbp_release);
    return 0;
}

int ksu_ptctl(struct ksu_ptctl_cmd *c)
{
    /* The whole struct is copied back, so an output field an op does not write
     * would hand the caller its own input back as if it were a result. arg1 is
     * left alone: it is an INPUT for KILLGUARD, SIGSEND and HWBP_WAIT. */
    c->ret = 0;
    c->arg2 = 0;

    switch (c->op) {
    case KSU_PTCTL_PEEK:
        return do_peek(c, false);
    case KSU_PTCTL_POKE:
        return do_peek(c, true);
    case KSU_PTCTL_GETREGS:
        return do_regs(c, false);
    case KSU_PTCTL_SETREGS:
        return do_regs(c, true);
    case KSU_PTCTL_INFO:
        return do_info(c);
    case KSU_PTCTL_SIGSEND:
        return do_sigsend(c);
    case KSU_PTCTL_KILLGUARD:
        return do_killguard(c);
    case KSU_PTCTL_HWBP_SET:
        return hwbp_set(c, c->addr, (pid_t)c->pid);
    case KSU_PTCTL_HWBP_WAIT:
        return hwbp_wait(c);
    case KSU_PTCTL_HWBP_RELEASE:
        return hwbp_release_held();
    case KSU_PTCTL_HWBP_CLEAR:
        /* Read before the clear: this is the caller's only chance to learn how
         * many hits the single-hold interlock dropped during the session. */
        c->ret = atomic_read(&hwbp_missed);
        hwbp_clear();
        return 0;
    case KSU_PTCTL_DETACH_TRACER:
        return -ENOSYS; /* reserved: see V2 */
    default:
        return -EINVAL;
    }
}

void ksu_ptctl_init(void)
{
    p_find_task_by_vpid = (find_task_by_vpid_t)find_kernel_symbol_exact("find_task_by_vpid");
    p_access_process_vm = (access_process_vm_t)find_kernel_symbol_exact("access_process_vm");
    p_send_sig_info = (send_sig_info_t)find_kernel_symbol_exact("send_sig_info");
    p_register_kprobe = (register_kprobe_t)find_kernel_symbol_exact("register_kprobe");
    p_unregister_kprobe = (unregister_kprobe_t)find_kernel_symbol_exact("unregister_kprobe");
    p_put_task_stack = (put_task_stack_t)find_kernel_symbol_exact("put_task_stack");
    p_task_call_func = (task_call_func_t)find_kernel_symbol_exact("task_call_func");
    p_reg_hwbp = (reg_hwbp_t)find_kernel_symbol_exact("register_user_hw_breakpoint");
    p_unreg_hwbp = (unreg_hwbp_t)find_kernel_symbol_exact("unregister_hw_breakpoint");
    p_perf_event_output_forward = (perf_overflow_handler_t)find_kernel_symbol_exact("perf_event_output_forward");
    p_perf_event_enable = (perf_event_enable_t)find_kernel_symbol_exact("perf_event_enable");
#if defined(CONFIG_ARM64)
    p_valid_user_regs = (valid_user_regs_t)find_kernel_symbol_exact("valid_user_regs");
    if (!p_valid_user_regs)
        pr_info("ptctl: valid_user_regs unavailable, using the in-module copy\n");
#endif
    if (!p_task_call_func)
        pr_info("ptctl: task_call_func unavailable, SETREGS restricted to off-CPU tasks\n");
    if (!p_perf_event_output_forward)
        pr_info("ptctl: perf_event_output_forward unavailable, hwbp uses the manual step path\n");

    init_completion(&hwbp_release);
    init_completion(&hwbp_gone);

    /* killguard's kprobe is installed on first use, not here. */
    pr_info("ptctl: init (find_task=%d apvm=%d kprobe=%d hwbp=%d regs=%d)\n", !!p_find_task_by_vpid,
            !!p_access_process_vm, !!p_register_kprobe, !!p_reg_hwbp, !!p_put_task_stack);
}

void ksu_ptctl_exit(void)
{
    hwbp_clear();
    guard_clear();
    mutex_lock(&killguard_lock);
    if (sig_kp_registered && p_unregister_kprobe) {
        p_unregister_kprobe(&sig_kp);
        sig_kp_registered = false;
    }
    mutex_unlock(&killguard_lock);
}
