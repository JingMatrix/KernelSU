/**
 * @file mount_hook.c
 * @brief KernelSU hook to dynamically pause shared mount propagation.
 *
 * This file implements a feature to temporarily prevent new mounts from
 * inheriting the "shared" property from their destination using a kretprobe.
 *
 * The mechanism uses a kretprobe on the internal VFS function
 * `attach_recursive_mnt`. When the "pause" feature is active, an entry
 * handler temporarily clears the MNT_SHARED flag on the destination mount.
 * A return handler then restores the original flags, ensuring consistency.
 */

#include <linux/kprobes.h>
#include <linux/atomic.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/kallsyms.h>
#include <linux/version.h>

#include "../../fs/mount.h"
#include "mount_hook.h"
#include "ksu.h"
#include "arch.h"
#include "klog.h"

typedef int (*attach_recursive_mnt_t)(struct mount *, struct mount *,
				      struct mountpoint *, bool);
static attach_recursive_mnt_t attach_recursive_mnt_fp = NULL;

atomic_t ksu_mount_propagation_paused = ATOMIC_INIT(0);

#ifdef HAVE_KPROBES

/**
 * @brief Private data structure passed from entry to return handler.
 */
struct ksu_attach_mnt_state {
	struct mount *dest_mnt;
	int original_flags;
	bool spoofed; // Flag to track if we actually modified anything.
};

static int ksu_attach_recursive_mnt_entry(struct kretprobe_instance *ri,
					  struct pt_regs *regs)
{
	struct mount *dest_mnt;
	struct ksu_attach_mnt_state *state;
	struct pt_regs *real_regs = PT_REAL_REGS(regs);

	state = (struct ksu_attach_mnt_state *)ri->data;
	state->spoofed = false; // Default to not spoofed

	// Only proceed if the pausing feature is active
	if (atomic_read(&ksu_mount_propagation_paused) == 0)
		return 0;

	// Use KernelSU-style macros to get the second argument (the parent/destination mount)
	dest_mnt = (struct mount *)PT_REGS_PARM2(real_regs);

	// We only care about modifying mounts that are currently shared.
	if (!(dest_mnt->mnt.mnt_flags & MNT_SHARED))
		return 0; // It's already private, do nothing.

	pr_info("KernelSU: Paused mount propagation: Spoofing shared mount %p to private.\n",
		dest_mnt);

	// --- The Spoof ---
	// 1. Save the original state to our private data area.
	state->dest_mnt = dest_mnt;
	state->original_flags = dest_mnt->mnt.mnt_flags;
	state->spoofed = true;

	// 2. Temporarily remove the MNT_SHARED flag.
	//    !! CRITICAL WARNING: This is the racy, unsafe part !!
	dest_mnt->mnt.mnt_flags &= ~MNT_SHARED;

	return 0; // Proceed to execute the now-tricked original function.
}

static int ksu_attach_recursive_mnt_ret(struct kretprobe_instance *ri,
					struct pt_regs *regs)
{
	struct ksu_attach_mnt_state *state =
		(struct ksu_attach_mnt_state *)ri->data;

	// The entry_handler ensures `state->spoofed` is only true if we
	// actually modified the flags.
	if (!state->spoofed)
		return 0;

	// --- The Restoration ---
	// Restore the original flags to the destination mount. This is guaranteed
	// to run after the original function returns.
	pr_info("KernelSU: Restoring original shared flags to mount %p.\n",
		state->dest_mnt);
	state->dest_mnt->mnt.mnt_flags = state->original_flags;

	return 0;
}

static struct kretprobe attach_recursive_mnt_krp = {
	.handler = ksu_attach_recursive_mnt_ret,
	.entry_handler = ksu_attach_recursive_mnt_entry,
	.data_size = sizeof(struct ksu_attach_mnt_state),
	.maxactive = 64, // A reasonable default for concurrent mounts
};

#endif // HAVE_KPROBES

void ksu_pause_mount_propagation(bool pause)
{
	if (pause) {
		pr_info("KernelSU: Pausing mount propagation.\n");
		atomic_set(&ksu_mount_propagation_paused, 1);
	} else {
		pr_info("KernelSU: Resuming mount propagation.\n");
		atomic_set(&ksu_mount_propagation_paused, 0);
	}
}

int ksu_mount_hook_init(void)
{
#ifdef HAVE_KPROBES
	int ret;

	attach_recursive_mnt_fp = (attach_recursive_mnt_t)kallsyms_lookup_name(
		"attach_recursive_mnt");
	if (!attach_recursive_mnt_fp) {
		pr_err("KernelSU: Could not find symbol 'attach_recursive_mnt'. Hooking failed.\n");
		return -ENOENT;
	}

	attach_recursive_mnt_krp.kp.addr =
		(kprobe_opcode_t *)attach_recursive_mnt_fp;

	ret = register_kretprobe(&attach_recursive_mnt_krp);
	if (ret < 0) {
		pr_err("KernelSU: kretprobe registration failed, returned %d\n",
		       ret);
		return ret;
	}

	pr_info("KernelSU: Mount propagation hook registered successfully.\n");
	return 0;
#else
	pr_info("KernelSU: Mount hook not enabled (HAVE_KPROBES not set).\n");
	return 0;
#endif
}

void ksu_mount_hook_exit(void)
{
#ifdef HAVE_KPROBES
	unregister_kretprobe(&attach_recursive_mnt_krp);
	pr_info("KernelSU: Mount propagation hook unregistered.\n");

	if (attach_recursive_mnt_krp.nmissed > 0) {
		pr_warn("KernelSU: Missed %u instances of attach_recursive_mnt probe.\n",
			attach_recursive_mnt_krp.nmissed);
	}
#endif
}
