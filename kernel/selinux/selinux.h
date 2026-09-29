#ifndef __KSU_H_SELINUX
#define __KSU_H_SELINUX

#include <linux/types.h>
#include <linux/version.h>
#include <linux/cred.h>

/* selinux_state kept its policy directly inline (struct selinux_ss *ss,
 * holding policydb/sidtab/latest_granting/status_page/status_lock/
 * policy_rwlock -- confirmed against a qgki-5.4 tree's own security.h)
 * before it was replaced by an RCU-swappable struct selinux_policy *policy
 * (same fields, plus a policy_mutex serializing reloads) that this file's
 * own commit history has used 5.10 as the boundary for once already
 * (JingMatrix/KernelSU@3580796, "Add back support for GKI-1.0 (Linux
 * 5.4)"). Consumers (selinux/rules.c, selinux/sepolicy.c,
 * feature/selinux_hide.c) branch on this rather than repeating the version
 * check, since the pre-split side of every one of those branches also has
 * no clone/RCU-swap primitive at all -- old-kernel code mutates
 * selinux_state.ss->policydb directly, the same way upstream KernelSU
 * always did before the clone architecture existed. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#define SELINUX_POLICY_INSTEAD_SELINUX_SS
#endif

#define KERNEL_SU_DOMAIN "ksu"
#define KERNEL_SU_FILE "ksu_file"

#define KERNEL_SU_CONTEXT "u:r:" KERNEL_SU_DOMAIN ":s0"
#define KSU_FILE_CONTEXT "u:object_r:" KERNEL_SU_FILE ":s0"
#define ZYGOTE_CONTEXT "u:r:zygote:s0"
#define INIT_CONTEXT "u:r:init:s0"

void setup_selinux(const char *, struct cred *);

void setenforce(bool);

bool getenforce();

void cache_sid(void);

bool is_task_ksu_domain(const struct cred *cred);

bool is_ksu_domain();

bool is_zygote(const struct cred *cred);

bool is_init(const struct cred *cred);

void apply_kernelsu_rules();

int handle_sepolicy(void __user *user_data, u64 data_len);

void setup_ksu_cred();

void escape_to_root_for_adb_root();

extern u32 ksu_file_sid;

#endif
