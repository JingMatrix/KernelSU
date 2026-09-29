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
 * permissive build), installs the task_defex_enforce bypass described below
 * where the build asked for it, and enables the PED credential sync.
 *
 * ksu_samsung_defex_sync_current() re-points DEFEX's stored PED snapshot at the
 * escalated creds so its credential check accepts them; no-op without DEFEX.
 *
 * The features a non-permissive build gates on a compile-time constant --
 * safeplace (execve of a non-whitelisted path by a root task) and
 * immutable-root (a root open under /data outside DEFEX's own
 * executable/prefix table, which is what stops a root task reaching
 * /data/adb) -- ignore those globals entirely. Both are decided inside
 * task_defex_enforce(), so a kprobe at its entry reaching the root callers is
 * what turns them off. Whether a given platform tolerates a probe on that
 * address is a property of the platform rather than of this code, so it is
 * armed only where a kprobe has demonstrably registered on this kernel
 * already; samsung_defex.c carries the rest of the reasoning, including what
 * supplies that evidence and why the pre-handler must return 1.
 *
 * Independently of that probe, the su path escalates AFTER the redirected
 * execve rather than before, so the exec-time check sees the unprivileged
 * caller; ksu_samsung_defex_present() gates that so only a DEFEX kernel
 * changes escalation ordering. That ordering stands on its own and still
 * covers the platforms where the probe is not armed at all.
 */
int ksu_samsung_defex_init(void);
/* Arms the enforce bypass, which is separate from init() because it waits on
 * evidence init() cannot have yet: it must run after ksu_supercalls_init(),
 * whose own outcome is what says whether a kprobe arms on this kernel. */
void ksu_samsung_defex_arm_enforce_bypass(void);
void ksu_samsung_defex_exit(void);
void ksu_samsung_defex_sync_current(void);
bool ksu_samsung_defex_present(void);

#endif
