#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/uidgid.h>

#include <asm/ptrace.h>

#include "klog.h" // IWYU pragma: keep
#include "infra/samsung_compat.h"
#include "infra/samsung_defex.h"
#include "infra/symbol_resolver.h"
#include "supercall/supercall.h"

/*
 * DEFEX features gated by a runtime status byte (defex_get_features() reads
 * these on a build where the feature is DEFEX_PERMISSIVE) are turned off by
 * zeroing the byte -- a plain data store to a resolved symbol, no kprobe and no
 * indirect call. A feature gated by a compile-time constant instead (e.g.
 * safeplace / privilege-escalation on a non-permissive build) ignores its
 * global, so the store is simply inert there. No-op where the symbol is absent.
 */
static void ksu_samsung_defex_zero_status_globals(void)
{
    static const char *const status_syms[] = {
        "global_privesc_status", "global_safeplace_status", "global_integrity_status", "global_immutable_status", NULL,
    };
    const char *const *sym;

    for (sym = status_syms; *sym; sym++) {
        unsigned char *p = (unsigned char *)ksu_resolve_symbol_for_functable_hook(*sym);
        if (p)
            WRITE_ONCE(*p, 0);
    }
}

/*
 * The features the stores above cannot reach -- safeplace and immutable-root,
 * which a non-permissive build gates on a compile-time constant rather than a
 * status byte -- are all decided inside task_defex_enforce(). Every DEFEX
 * syscall hook funnels through that one function, and it returns 0 to allow,
 * so a single kprobe at its entry that reports "allow" and returns straight to
 * the caller clears all of them at once.
 *
 * The pre-handler must return 1, not 0. Returning 0 leaves the kprobe
 * framework to single-step the saved original instruction out of line, and on
 * a platform that protects kernel text from outside Linux that slot is not
 * executable -- the probe arms, and the fault arrives on its first trigger.
 * Returning 1 declares the instruction already handled, so the slot is never
 * entered. The handler therefore has to complete the call itself, which it
 * can: the probe sits on the function's first instruction, before any
 * prologue, so x0 still holds the first argument (and is where the return
 * value belongs) and x30 still holds the return address.
 *
 * Scoped to root callers, which costs nothing it needs: DEFEX evaluates
 * immutable-root only for root credentials in the first place, so a non-root
 * caller is left to run the real function and every unprivileged task on the
 * device keeps being enforced as before.
 *
 * Armed on evidence rather than on a build flag or a guess, and on two
 * pieces of it at once: DEFEX has to be present, and a kprobe has to have
 * demonstrably armed on this kernel already.
 *
 * The first is what the gate needs clearing for. The second is what the
 * reboot handshake leaves behind: ksu_reboot_kprobe_armed() is true only where
 * the sanctioned write for that handshake was refused AND its kprobe then
 * registered, so it reports a probe this kernel accepted rather than a
 * prediction that one would be accepted. A platform whose table write
 * succeeds never attempts that probe, so it never claims the evidence and
 * never arms this one either -- which is what keeps a kernel that has no need
 * of this from taking its risk.
 *
 * It is deliberately NOT gated on ksu_kprobe_text_patch_unsafe() instead:
 * that predicate answers a question about two named targets elsewhere in the
 * kernel (hook/kprobe_patch_compat.h, which says in as many words to treat a
 * newly-considered target as unverified rather than unsafe), and this is a
 * third target it says nothing about -- using it here would read as evidence
 * where there is none, in either direction.
 */
#ifdef CONFIG_ARM64

#define KSU_DEFEX_ALLOW 0 /* DEFEX_ALLOW, defex_internal.h */

static int ksu_samsung_defex_enforce_handler(struct kprobe *p, struct pt_regs *regs)
{
    if (__kuid_val(current_cred()->euid) != 0)
        return 0;

    regs->regs[0] = KSU_DEFEX_ALLOW;
    regs->pc = procedure_link_pointer(regs);
    return 1;
}

static struct kprobe ksu_samsung_defex_enforce_kp = {
    .symbol_name = "task_defex_enforce",
    .pre_handler = ksu_samsung_defex_enforce_handler,
};

static bool ksu_samsung_defex_enforce_hooked;

static void ksu_samsung_defex_install_enforce_bypass(void)
{
    int rc = register_kprobe(&ksu_samsung_defex_enforce_kp);

    if (rc) {
        pr_err("Samsung DEFEX enforce bypass failed: %d\n", rc);
        return;
    }

    ksu_samsung_defex_enforce_hooked = true;
    pr_info("Samsung DEFEX enforce bypass installed\n");
}

static void ksu_samsung_defex_remove_enforce_bypass(void)
{
    if (ksu_samsung_defex_enforce_hooked) {
        unregister_kprobe(&ksu_samsung_defex_enforce_kp);
        ksu_samsung_defex_enforce_hooked = false;
    }
}

#else /* the handler is arm64 register surgery, so it is arch-gated too */

static void ksu_samsung_defex_install_enforce_bypass(void)
{
    pr_info("Samsung DEFEX enforce bypass not built for this arch\n");
}

static void ksu_samsung_defex_remove_enforce_bypass(void)
{
}

#endif

int ksu_samsung_defex_init(void)
{
    if (!ksu_samsung_defex_present()) {
        pr_info("Samsung DEFEX not present; nothing to sync\n");
        return 0;
    }

    ksu_samsung_defex_zero_status_globals();

    pr_info("Samsung DEFEX credential sync enabled\n");
    return 0;
}

void ksu_samsung_defex_arm_enforce_bypass(void)
{
    if (!ksu_samsung_defex_present()) {
        return;
    }
    if (!ksu_reboot_kprobe_armed()) {
        pr_info("Samsung DEFEX enforce bypass not armed: no kprobe has armed on this kernel\n");
        return;
    }
    ksu_samsung_defex_install_enforce_bypass();
}

void ksu_samsung_defex_exit(void)
{
    ksu_samsung_defex_remove_enforce_bypass();
}

bool ksu_samsung_defex_present(void)
{
    return ksu_samsung_defex_ops() != NULL;
}

/*
 * __nocfi: the DEFEX ops resolved by samsung_compat.c are kallsyms lookups,
 * so calling through them trips kernel CFI ("CFI failure (target:
 * get_task_creds+0x0/...)") -- same reason ksu_cred_commit_worker in
 * cred_compat.c is __nocfi.
 */
void __nocfi ksu_samsung_defex_sync_current(void)
{
    const struct ksu_samsung_defex_ops *defex = ksu_samsung_defex_ops();
    const struct cred *cred;
    unsigned int stored_uid;
    unsigned int stored_fsuid;
    unsigned int stored_egid;
    unsigned short cred_flags;

    if (!defex)
        return;

    cred = current_cred();
    defex->get_task_creds(current, &stored_uid, &stored_fsuid, &stored_egid, &cred_flags);

    if (__kuid_val(cred->euid) == 0 && __kuid_val(cred->fsuid) == 0 && __kgid_val(cred->egid) == 0) {
        /* DEFEX's own sanctioned-root marker: defex_main.c stores (1,1,1) for a
         * legitimately-rooted task, which task_defex_enforce() then accepts. */
        stored_uid = 1;
        stored_fsuid = 1;
        stored_egid = 1;
    } else {
        stored_uid = __kuid_val(cred->euid);
        stored_fsuid = __kuid_val(cred->fsuid);
        stored_egid = __kgid_val(cred->egid);
    }

    if (defex->set_task_creds(current, stored_uid, stored_fsuid, stored_egid, cred_flags))
        pr_err("Samsung DEFEX credential sync failed\n");
}
