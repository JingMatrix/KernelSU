#ifndef __KSU_H_CPU_SPOOF
#define __KSU_H_CPU_SPOOF

/* Entry point for the KSU_IOCTL_SET_SPOOF_CPU supercall (see uapi/supercall.h). */
struct ksu_set_spoof_cpu_cmd;
int ksu_set_spoof_cpu(struct ksu_set_spoof_cpu_cmd *cmd);

#endif
