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
 * @brief Signals that the Zygote process has started.
 *
 */
void ksu_set_zygote_started(void);

#endif /* KSU_MOUNT_HOOK_H_ */
