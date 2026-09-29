#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/uidgid.h>

#include "klog.h" // IWYU pragma: keep
#include "infra/samsung_compat.h"
#include "infra/samsung_defex.h"
#include "infra/symbol_resolver.h"

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

void ksu_samsung_defex_exit(void)
{
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
