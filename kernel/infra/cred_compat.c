#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/completion.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/workqueue.h>

#include "infra/cred_compat.h"
#include "infra/samsung_compat.h"
#include "infra/samsung_defex.h"
#include "klog.h" // IWYU pragma: keep

/*
 * Whatever prepare_ro_creds()/kdp_assign_pgd() actually are on the other
 * side of the vendor's own call (a hypervisor call in KDP's case, same
 * family as RKP's uh_call()), running them from a plain, freshly scheduled
 * work item guarantees an ordinary process context to do it in, rather than
 * whatever context happened to call ksu_commit_creds() -- cheap insurance
 * against a constraint this side of the boundary cannot see, matching the
 * one thing KernelSU-Next's own version of this got right structurally even
 * if the rest of that file was not trusted wholesale for this port.
 */
struct ksu_cred_commit_work {
    struct work_struct work;
    struct completion done;
    struct task_struct *target;
    const struct cred *old_cred;
    struct cred *new_cred;
    const struct ksu_samsung_kdp_ops *kdp;
    int result;
};

#define KDP_COPY_CREDS 0

/*
 * __nocfi: kdp->prepare_ro_creds/assign_pgd are resolved from kallsyms at
 * runtime, not linked normally, so the compiler cannot see a matching kCFI
 * type hash for the indirect call -- confirmed on hardware ("CFI failure
 * (target: prepare_ro_creds+0x0/0x4c0)") the first time this ran without
 * it. Every ksu_hook_* in hook/syscall_event_bridge.c that calls through a
 * resolved function pointer the same way is already marked this way; this
 * file just missed it.
 */
static void __nocfi ksu_cred_commit_worker(struct work_struct *work)
{
    struct ksu_cred_commit_work *w = container_of(work, struct ksu_cred_commit_work, work);
    struct task_struct *target = w->target;
    const struct cred *old_cred = w->old_cred;
    struct cred *ro_cred;

    if (rcu_access_pointer(target->cred) != old_cred || rcu_access_pointer(target->real_cred) != old_cred) {
        w->result = -EBUSY;
        goto out;
    }

    ro_cred = w->kdp->prepare_ro_creds(w->new_cred, KDP_COPY_CREDS, (u64)target);
    if (!ro_cred) {
        w->result = -EIO;
        goto out;
    }

    rcu_assign_pointer(target->real_cred, ro_cred);
    rcu_assign_pointer(target->cred, ro_cred);
    w->kdp->assign_pgd(target);
    w->result = 0;
out:
    complete(&w->done);
}

static int ksu_commit_creds_protected(struct cred *new, const struct ksu_samsung_kdp_ops *kdp)
{
    struct ksu_cred_commit_work w;

    w.target = current;
    w.old_cred = current_cred();
    w.new_cred = new;
    w.kdp = kdp;
    w.result = -EIO;
    INIT_WORK(&w.work, ksu_cred_commit_worker);
    init_completion(&w.done);

    get_task_struct(w.target);
    if (!schedule_work(&w.work)) {
        put_task_struct(w.target);
        abort_creds(new);
        return -EBUSY;
    }
    wait_for_completion(&w.done);
    put_task_struct(w.target);

    if (w.result)
        abort_creds(new);
    return w.result;
}

int ksu_commit_creds(struct cred *new)
{
    const struct ksu_samsung_kdp_ops *kdp;
    int ret;

    if (!new)
        return -EINVAL;

    kdp = ksu_samsung_kdp_ops();
    ret = kdp ? ksu_commit_creds_protected(new, kdp) : commit_creds(new);
    /* current now holds the escalated creds; re-point Samsung DEFEX's stored
     * snapshot at them so its task_defex_enforce() does not SIGKILL us for a
     * credential violation. No-op on kernels without DEFEX. */
    ksu_samsung_defex_sync_current();
    return ret;
}
