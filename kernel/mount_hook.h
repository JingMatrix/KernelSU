#ifndef KSU_MOUNT_HOOK_H_
#define KSU_MOUNT_HOOK_H_

#include <linux/types.h>

/**
 * @file mount_hook.h
 * @brief Public interface for the KernelSU mount propagation hook.
 *
 * This header declares the functions necessary to initialize, control, and
 * tear down the kprobe-based hook on the `attach_recursive_mnt` kernel
 * function. By including this header, other parts of the KernelSU kernel
 * module can manage the mount propagation pause feature.
 */

/**
 * @brief Initializes and registers the mount propagation hook.
 *
 * This function must be called during the KernelSU module's main
 * initialization routine. It resolves the address of the non-exported
 * `attach_recursive_mnt` symbol using kallsyms and registers a kprobe
 * to hook it.
 *
 * @return 0 on success, or a negative error code on failure (e.g., if the
 *         symbol cannot be found or the kprobe cannot be registered).
 */
int ksu_mount_hook_init(void);

/**
 * @brief Unregisters the mount propagation hook.
 *
 * This function must be called during the KernelSU module's main exit or
 * cleanup routine. It safely unregisters the kprobe from the kernel.
 */
void ksu_mount_hook_exit(void);

/**
 * @brief Control function to dynamically enable or disable the mount
 *        propagation pause feature.
 *
 * This is the main control interface for the feature. It is designed to be
 * called from the KernelSU syscall/ioctl handler in response to a request
 * from the userspace `ksud` daemon.
 *
 * @param pause Set to `true` to pause shared mount propagation (forcing new
 *              mounts to be private). Set to `false` to restore the
 *              kernel's default behavior.
 */
void ksu_pause_mount_propagation(bool pause);

#endif /* KSU_MOUNT_HOOK_H_ */
