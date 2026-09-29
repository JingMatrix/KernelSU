#ifndef __KSU_SAMSUNG_DEFEX_H
#define __KSU_SAMSUNG_DEFEX_H

#include <linux/types.h>

#include "infra/samsung_compat.h"

/*
 * Samsung DEFEX SIGKILLs KSU's escalated tasks through several features -- PED
 * (credential escalation past DEFEX's stored snapshot), safeplace (execve of a
 * non-whitelisted path by a root task, e.g. /data/adb/ksud), integrity and
 * immutable. On a non-DEFEX_PERMISSIVE build none of these can be turned off via
 * a status global.
 *
 * Presence is detected by infra/samsung_compat.h's shared kallsyms probe;
 * ksu_samsung_defex_present() below is true exactly when that probe
 * resolved DEFEX's credential-sync API, which is also everything this file
 * needs to act.
 *
 * ksu_samsung_defex_init() zeroes the writable per-feature status globals
 * (disables the features DEFEX reads them for -- integrity, immutable on a
 * permissive build) and enables the PED credential sync below.
 *
 * ksu_samsung_defex_sync_current() re-points DEFEX's stored PED snapshot at the
 * escalated creds so its credential check accepts them; no-op without DEFEX.
 *
 * Safeplace (execve of a non-whitelisted path by a root task) has no runtime
 * global on a non-permissive build and cannot be reached by a kprobe here (the
 * task_defex_enforce probe faults on trigger under Samsung RKP -- its
 * out-of-line single-step slot is non-executable). It is handled instead on the
 * su path by escalating AFTER the redirected execve rather than before, so the
 * exec-time check sees the unprivileged caller; ksu_samsung_defex_present()
 * gates that so only a DEFEX kernel changes escalation ordering.
 */
int ksu_samsung_defex_init(void);
void ksu_samsung_defex_exit(void);
void ksu_samsung_defex_sync_current(void);
bool ksu_samsung_defex_present(void);

#endif
