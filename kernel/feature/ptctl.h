#ifndef __KSU_H_PTCTL
#define __KSU_H_PTCTL

#include <linux/types.h>

/* Entry point for the KSU_IOCTL_PTCTL supercall (see uapi/supercall.h). */
struct ksu_ptctl_cmd;
int ksu_ptctl(struct ksu_ptctl_cmd *cmd);

/* Registers the signal kprobe used by KILLGUARD. Safe to call once at init. */
void ksu_ptctl_init(void);
void ksu_ptctl_exit(void);

#endif
