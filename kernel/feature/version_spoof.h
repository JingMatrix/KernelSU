#ifndef __KSU_H_VERSION_SPOOF
#define __KSU_H_VERSION_SPOOF

/* Entry point for the KSU_IOCTL_SET_SPOOF_VERSION supercall (see uapi/supercall.h). */
struct ksu_set_spoof_version_cmd;
int ksu_set_spoof_version(struct ksu_set_spoof_version_cmd *cmd);

#endif
