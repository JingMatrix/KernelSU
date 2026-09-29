#include <linux/bitops.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "infra/samsung_compat.h"
#include "infra/symbol_resolver.h"
#include "klog.h" // IWYU pragma: keep

enum ksu_samsung_subsystem {
    KSU_SAMSUNG_UH = BIT(0),
    KSU_SAMSUNG_DEFEX = BIT(1),
};

/*
 * One entry per kallsyms name that, on its own, proves a subsystem is built
 * into this kernel. Add a newly-discovered Samsung device's symbol names
 * here -- an alternate spelling for an existing subsystem is just another
 * row with the same bit, not a new table or a new probe function.
 */
struct ksu_samsung_signature {
    const char *symbol;
    enum ksu_samsung_subsystem subsystem;
};

static const struct ksu_samsung_signature ksu_samsung_signatures[] = {
    /* RKP/KDP ("uh"): rkp_ro_alloc/uh_call/is_rkp_ro_buffer are RKP's own;
     * kdp_assign_pgd/is_kdp_kmem_cache are KDP's; prepare_ro_creds is KDP's
     * conversion entry point but lives in the same driver and is as
     * reliable a signal as either half on its own. */
    { "rkp_ro_alloc", KSU_SAMSUNG_UH },
    { "uh_call", KSU_SAMSUNG_UH },
    { "is_rkp_ro_buffer", KSU_SAMSUNG_UH },
    { "kdp_assign_pgd", KSU_SAMSUNG_UH },
    { "is_kdp_kmem_cache", KSU_SAMSUNG_UH },
    { "prepare_ro_creds", KSU_SAMSUNG_UH },
    /* DEFEX: task_defex_enforce belongs to a distinct subsystem that ships
     * alongside RKP/KDP on every Samsung target seen so far, so its
     * presence is as reliable a signal and catches a build that strips the
     * RKP-specific symbols but keeps DEFEX. */
    { "task_defex_enforce", KSU_SAMSUNG_DEFEX },
};

static unsigned int ksu_samsung_detected;
static struct ksu_samsung_kdp_ops ksu_samsung_kdp;
static struct ksu_samsung_defex_ops ksu_samsung_defex;
static bool ksu_samsung_kdp_ops_valid;
static bool ksu_samsung_defex_ops_valid;

void ksu_samsung_compat_probe(void)
{
    unsigned int i;
    void *prepare_ro_creds_fn, *kdp_assign_pgd_fn;
    void *defex_get_fn, *defex_set_fn;

    ksu_samsung_detected = 0;
    for (i = 0; i < ARRAY_SIZE(ksu_samsung_signatures); i++) {
        const struct ksu_samsung_signature *sig = &ksu_samsung_signatures[i];

        if (ksu_samsung_detected & sig->subsystem)
            continue;
        if (ksu_resolve_symbol_for_functable_hook(sig->symbol)) {
            pr_info("samsung_compat: %s present\n", sig->symbol);
            ksu_samsung_detected |= sig->subsystem;
        }
    }

    if (ksu_samsung_detected & KSU_SAMSUNG_UH) {
        prepare_ro_creds_fn = ksu_resolve_symbol_for_functable_hook("prepare_ro_creds");
        kdp_assign_pgd_fn = ksu_resolve_symbol_for_functable_hook("kdp_assign_pgd");
        if (prepare_ro_creds_fn && kdp_assign_pgd_fn) {
            ksu_samsung_kdp.prepare_ro_creds = prepare_ro_creds_fn;
            ksu_samsung_kdp.assign_pgd = kdp_assign_pgd_fn;
            ksu_samsung_kdp_ops_valid = true;
            pr_info("samsung_compat: KDP conversion API resolved\n");
        } else {
            pr_warn("samsung_compat: uh present but prepare_ro_creds/kdp_assign_pgd unresolved, "
                    "falling back to plain commit_creds\n");
        }
    }

    /* get_task_creds/set_task_creds (security/samsung/defex_lsm/include/
     * defex_internal.h) are resolved independently of task_defex_enforce
     * above: a build can strip one set of symbols and keep the other, and
     * this is the API infra/samsung_defex.c actually needs, not just a
     * presence marker. */
    defex_get_fn = ksu_resolve_symbol_for_functable_hook("get_task_creds");
    defex_set_fn = ksu_resolve_symbol_for_functable_hook("set_task_creds");
    if (defex_get_fn && defex_set_fn) {
        ksu_samsung_defex.get_task_creds = defex_get_fn;
        ksu_samsung_defex.set_task_creds = defex_set_fn;
        ksu_samsung_defex_ops_valid = true;
        ksu_samsung_detected |= KSU_SAMSUNG_DEFEX;
        pr_info("samsung_compat: DEFEX credential-sync API resolved\n");
    }
}

bool ksu_samsung_uh_present(void)
{
    return (ksu_samsung_detected & KSU_SAMSUNG_UH) != 0;
}

bool ksu_samsung_defex_build_present(void)
{
    return (ksu_samsung_detected & KSU_SAMSUNG_DEFEX) != 0;
}

const struct ksu_samsung_kdp_ops *ksu_samsung_kdp_ops(void)
{
    return ksu_samsung_kdp_ops_valid ? &ksu_samsung_kdp : NULL;
}

const struct ksu_samsung_defex_ops *ksu_samsung_defex_ops(void)
{
    return ksu_samsung_defex_ops_valid ? &ksu_samsung_defex : NULL;
}
