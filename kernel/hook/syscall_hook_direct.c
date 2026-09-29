#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
#include <linux/sched/task_stack.h>
#endif
#include <asm/syscall.h>

#include "arch.h"
#include "hook/syscall_event_bridge.h"
#include "hook/syscall_hook.h"
#include "hook/syscall_hook_direct.h"
#include "klog.h" // IWYU pragma: keep

/*
 * One kprobe per hooked syscall, target address read from sys_call_table
 * (never written), redirecting into the same ksu_hook_* implementation the
 * dispatcher path uses. Each ksu_hook_* already calls the real
 * ksu_syscall_table[nr](regs) itself when it decides not to intercept -- in
 * the dispatcher backend that is always safe, since the dispatcher only ever
 * patches its own unused slot and every real syscall's table entry stays
 * untouched, but here the kprobe's own .addr *is* that entry's value, and
 * ksu_hook_stat_sucompat() also calls back into it directly for its own
 * passthrough. Either path lands right back on this same breakpoint, which
 * redirects into the trampoline, which calls the hook, which passes through
 * again -- unbounded recursion without the guard below.
 *
 * The guard: PT_REGS_ORIG_SYSCALL(task_pt_regs(current)) (x8 in the task's
 * own, persistent syscall-entry pt_regs -- not the transient pt_regs a
 * kprobe handler receives as its own parameter, which is a fresh struct
 * constructed per exception and differs on every hit, including a recursive
 * one, so a sentinel written there would never be visible to the recursive
 * hit's own handler). task_pt_regs(current) is the one place that stays the
 * same struct across nested traps within a task, since it lives at a fixed
 * offset from the top of the task's own kernel stack. x8 there is never read
 * by any of the real __arm64_sys_*(regs) bodies below it -- they take their
 * own arguments via PT_REGS_PARM1..6, not this field -- so it is free to
 * borrow as a one-shot reentry flag, restored before the trampoline returns
 * so nothing downstream (audit, seccomp, ptrace) ever sees the borrowed
 * value. Set it to a sentinel no real syscall number can equal (all are >=
 * 0) right before the passthrough-capable call; the recursive hit's own
 * pre_handler sees the sentinel and returns 0 instead of redirecting again,
 * which lets arm64's kprobe core single-step the real original instruction
 * and continue directly into the real function body -- the same reentrancy
 * technique KernelSU-Next's Samsung sucompat kprobes use
 * (task_pt_regs(current)->syscallno, the identical idea), kept here because
 * it is a correct, general answer to "a kprobe placed exactly where its own
 * passthrough calls," not anything platform-specific.
 */
#define KSU_DIRECT_HOOK_REENTRY_MARKER ((long)-2)

struct ksu_direct_hook {
    int nr;
    const char *name;
    long (*trampoline)(const struct pt_regs *regs);
    struct kprobe kp;
};

#define KSU_DIRECT_HOOK(ident, nr_const, fn)                                                                           \
    static long ksu_direct_##ident##_trampoline(const struct pt_regs *regs)                                            \
    {                                                                                                                  \
        struct pt_regs *task_regs = task_pt_regs(current);                                                             \
        long saved = PT_REGS_ORIG_SYSCALL(task_regs);                                                                  \
        long ret;                                                                                                      \
        PT_REGS_ORIG_SYSCALL(task_regs) = KSU_DIRECT_HOOK_REENTRY_MARKER;                                              \
        ret = fn((nr_const), regs);                                                                                    \
        PT_REGS_ORIG_SYSCALL(task_regs) = saved;                                                                       \
        return ret;                                                                                                    \
    }                                                                                                                  \
    static int ksu_direct_##ident##_pre_handler(struct kprobe *p, struct pt_regs *regs)                                \
    {                                                                                                                  \
        if (unlikely(PT_REGS_ORIG_SYSCALL(task_pt_regs(current)) == KSU_DIRECT_HOOK_REENTRY_MARKER))                   \
            return 0;                                                                                                  \
        instruction_pointer_set(regs, (unsigned long)ksu_direct_##ident##_trampoline);                                 \
        return 1;                                                                                                      \
    }                                                                                                                  \
    static struct ksu_direct_hook ksu_direct_##ident##_hook = {                                                        \
        .nr = (nr_const),                                                                                              \
        .name = #ident,                                                                                                \
        .trampoline = ksu_direct_##ident##_trampoline,                                                                 \
        .kp = { .pre_handler = ksu_direct_##ident##_pre_handler },                                                     \
    }

KSU_DIRECT_HOOK(execve, __NR_execve, ksu_hook_execve);
KSU_DIRECT_HOOK(execveat, __NR_execveat, ksu_hook_execveat);
KSU_DIRECT_HOOK(newfstatat, __NR_newfstatat, ksu_hook_newfstatat);
KSU_DIRECT_HOOK(faccessat, __NR_faccessat, ksu_hook_faccessat);
KSU_DIRECT_HOOK(setresuid, __NR_setresuid, ksu_hook_setresuid);

static struct ksu_direct_hook *const ksu_direct_hooks[] = {
    &ksu_direct_execve_hook,    &ksu_direct_execveat_hook,  &ksu_direct_newfstatat_hook,
    &ksu_direct_faccessat_hook, &ksu_direct_setresuid_hook,
};

int ksu_syscall_hook_direct_init(void)
{
    int i, ret, installed = 0;

    if (!ksu_syscall_table)
        return -ENOENT;

    for (i = 0; i < ARRAY_SIZE(ksu_direct_hooks); i++) {
        struct ksu_direct_hook *h = ksu_direct_hooks[i];

        if (h->nr < 0 || h->nr >= __NR_syscalls || !READ_ONCE(ksu_syscall_table[h->nr])) {
            pr_warn("syscall_hook_direct: %s (nr=%d) not present in sys_call_table, skipping\n", h->name, h->nr);
            continue;
        }
        h->kp.addr = (kprobe_opcode_t *)READ_ONCE(ksu_syscall_table[h->nr]);
        ret = register_kprobe(&h->kp);
        if (ret) {
            pr_warn("syscall_hook_direct: register_kprobe(%s) failed: %d\n", h->name, ret);
            h->kp.addr = NULL;
            continue;
        }
        installed++;
        pr_info("syscall_hook_direct: %s hooked directly at 0x%px\n", h->name, h->kp.addr);
    }

    if (!installed)
        return -ENOENT;
    return 0;
}

void ksu_syscall_hook_direct_exit(void)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(ksu_direct_hooks); i++) {
        struct ksu_direct_hook *h = ksu_direct_hooks[i];

        if (h->kp.addr) {
            unregister_kprobe(&h->kp);
            h->kp.addr = NULL;
        }
    }
}
