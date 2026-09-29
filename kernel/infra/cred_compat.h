#ifndef __KSU_H_CRED_COMPAT
#define __KSU_H_CRED_COMPAT

#include <linux/cred.h>

/*
 * Commit a prepared cred to current -- the same contract as commit_creds():
 * consumes @new either way, the caller must not touch it again after this
 * call, success or failure.
 *
 * Some vendor kernels manage every task's struct cred through a protected
 * allocator of their own, tied to a per-task page-table association
 * (Samsung KDP, drivers/uh/kdp.c upstream), and do not recognize a cred
 * installed by any other path. Presence is detected by
 * infra/samsung_compat.h's shared kallsyms probe (the same one
 * hook/kprobe_patch_compat.h checks for its own unsafe kprobe targets) and
 * routes the commit through the vendor's own conversion API instead of the
 * plain commit_creds(); add a newly-discovered platform's signatures to
 * samsung_compat.c rather than special-casing it here or at any call site.
 */
int ksu_commit_creds(struct cred *new);

#endif
