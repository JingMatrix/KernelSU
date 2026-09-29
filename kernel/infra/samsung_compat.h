#ifndef __KSU_H_SAMSUNG_COMPAT
#define __KSU_H_SAMSUNG_COMPAT

#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/types.h>

/*
 * Samsung ships (at least) two separate protection subsystems that KernelSU
 * has to work around, both discovered on the same reference device (a52s,
 * qgki-5.4, SM7325) but expected to appear -- with the same kallsyms names,
 * since they come from the same upstream Samsung driver tree -- on any
 * Samsung target that carries them:
 *
 *  - "uh" (RKP + KDP, drivers/uh/{rkp,kdp}.c upstream): a hypervisor-backed
 *    layer that (a) makes specific kernel text addresses fault on a kprobe's
 *    BRK even though the page tables look ordinary
 *    (hook/kprobe_patch_compat.h), and (b) replaces every task's struct cred
 *    with one it manages itself, rejecting anything installed by plain
 *    commit_creds() (infra/cred_compat.h). RKP and KDP are always built
 *    together as one driver, so one presence check covers both.
 *  - DEFEX (security/samsung/defex_lsm upstream): an LSM-adjacent layer that
 *    SIGKILLs an escalated task over a stale credential snapshot or a
 *    non-whitelisted exec (infra/samsung_defex.h).
 *
 * Both are detected the same way for the same reason: there is no safe way
 * to probe either directly (the probe would trigger the exact fault or kill
 * being tested for), so presence is inferred from a small set of kallsyms
 * names that exist only where the corresponding subsystem is built in. This
 * file is the single place that scans kallsyms for all of them -- one pass,
 * cached -- so that supporting a new Samsung device is "add its symbol
 * names to the table in samsung_compat.c," never "add another independent
 * probe with its own copy of the same signature list."
 *
 * ksu_samsung_compat_probe() must run once, early -- after
 * ksu_init_symbol_resolver() and before anything below queries presence or
 * ops (core/init.c calls it in that order). Every query function is a cheap
 * read of the cached result; none of them re-scans kallsyms.
 */
void ksu_samsung_compat_probe(void);

/* True if the "uh" hypervisor layer (RKP/KDP) is present on this boot --
 * kprobe_patch_compat.c's signal that a kprobe's BRK patch is unsafe on
 * specific text addresses. */
bool ksu_samsung_uh_present(void);

/* True if a DEFEX build marker was found, whether or not its credential-sync
 * API (below) actually resolved -- also a kprobe_patch_compat.c unsafe
 * signal, since task_defex_enforce shares the same unusable out-of-line
 * single-step slot as the RKP/KDP targets. */
bool ksu_samsung_defex_build_present(void);

/*
 * KDP's own struct-cred conversion API, resolved once and cached. Returns
 * NULL when KDP is not present, or when it is present but the expected
 * symbols failed to resolve (logged once during the probe) -- either way,
 * callers fall back to the plain path.
 */
struct ksu_samsung_kdp_ops {
    struct cred *(*prepare_ro_creds)(struct cred *cred, int command, u64 task);
    void (*assign_pgd)(struct task_struct *task);
};
const struct ksu_samsung_kdp_ops *ksu_samsung_kdp_ops(void);

/*
 * DEFEX's per-task credential snapshot API, resolved once and cached.
 * Returns NULL when DEFEX's credential-sync symbols did not resolve, which
 * is also exactly what "DEFEX credential sync is usable" means to every
 * caller (infra/samsung_defex.c, feature/sucompat.c).
 */
struct ksu_samsung_defex_ops {
    void (*get_task_creds)(struct task_struct *task, unsigned int *uid, unsigned int *fsuid, unsigned int *egid,
                           unsigned short *cred_flags);
    int (*set_task_creds)(struct task_struct *task, unsigned int uid, unsigned int fsuid, unsigned int egid,
                          unsigned short cred_flags);
};
const struct ksu_samsung_defex_ops *ksu_samsung_defex_ops(void);

#endif
