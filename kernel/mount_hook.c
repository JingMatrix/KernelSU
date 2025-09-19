/**
 * @file mount_hook.c
 * @brief KernelSU hook to dynamically pause shared mount propagation.
 *
 * This file implements a feature to temporarily prevent new mounts from
 * inheriting the "shared" property from their destination using a kretprobe.
 *
 * The mechanism uses a kretprobe on the internal VFS function 
 * `attach_recursive_mnt`. An entry handler temporarily clears the MNT_SHARED
 * flag on the destination mount, and then a return handler then restores the
 * original flags, ensuring consistency.
 */

#include "mount_hook.h"

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "../../fs/mount.h"
#include "arch.h"
#include "klog.h"

// State variable to track if Zygote has been initialized.
static atomic_t ksu_zygote_started = ATOMIC_INIT(0);
// State and storage for the KernelSU modules' device name.
static atomic_t ksu_modules_devname_initialized = ATOMIC_INIT(0);
static char ksu_modules_devname[256];

#ifdef CONFIG_KSU_DEBUG
/**
 * @brief Logs useful information about a mount point for debugging.
 * @param prefix A string to prepend to the log lines (e.g., "Source").
 * @param mnt The mount structure to inspect.
 */
static void log_mount_info(const char *prefix, struct mount *mnt)
{
	if (!mnt) {
		pr_info("%s mount is <NULL>\n", prefix);
		return;
	}

	char path_buf[256];
	char *dpath;

	struct path p = { .mnt = &mnt->mnt, .dentry = mnt->mnt.mnt_root };
	dpath = d_path(&p, path_buf, sizeof(path_buf));

	pr_info("--- Mount Info: %s ---\n", prefix);
	pr_info("  -> Mnt Ptr:   %p\n", mnt);
	pr_info("  -> Flags:     %#x\n", mnt->mnt.mnt_flags);
	pr_info("  -> Dev Name:  %s\n", mnt->mnt_devname);

	if (mnt->mnt.mnt_sb && mnt->mnt.mnt_sb->s_type) {
		pr_info("  -> FS Type:   %s\n", mnt->mnt.mnt_sb->s_type->name);
	}

	if (IS_ERR(dpath)) {
		pr_info("  -> Path:      <Error getting path: %ld>\n",
			PTR_ERR(dpath));
	} else {
		pr_info("  -> Path:      %s\n", dpath);
	}
	pr_info("--------------------------\n");
}
#endif // CONFIG_KSU_DEBUG

#ifdef CONFIG_KPROBES

/**
 * @brief Private data structure passed from entry to return handler.
 */
struct ksu_attach_mnt_state {
	struct mount *dest_mnt;
	int original_flags;
	bool spoofed;
};

static int ksu_attach_recursive_mnt_entry(struct kretprobe_instance *ri,
					  struct pt_regs *regs)
{
	struct mount *source_mnt = (struct mount *)PT_REGS_PARM1(regs);
	struct mount *dest_mnt = (struct mount *)PT_REGS_PARM2(regs);
	struct ksu_attach_mnt_state *state;

	state = (struct ksu_attach_mnt_state *)ri->data;
	state->spoofed = false;

	// Always validate pointers from hooks before dereferencing.
	if (!source_mnt || !dest_mnt) {
		return 0;
	}

	// --- Step 1: Dynamically capture the modules device name (runs once) ---
	if (atomic_read(&ksu_modules_devname_initialized) == 0 &&
	    source_mnt->mnt_devname &&
	    strncmp(source_mnt->mnt_devname, "/dev/block/loop", 15) == 0) {
		strncpy(ksu_modules_devname, source_mnt->mnt_devname,
			sizeof(ksu_modules_devname) - 1);
		// Ensure null termination.
		ksu_modules_devname[sizeof(ksu_modules_devname) - 1] = '\0';

		atomic_set(&ksu_modules_devname_initialized, 1);
		pr_info("KernelSU modules devname captured: %s\n",
			ksu_modules_devname);
	}

	// --- Step 2: Check conditions for skipping the spoof ---
	// Skip if Zygote has started AND the source device is NOT our modules device.
	if (atomic_read(&ksu_zygote_started) != 0 &&
	    (atomic_read(&ksu_modules_devname_initialized) == 0 ||
	     (source_mnt->mnt_devname &&
	      strcmp(source_mnt->mnt_devname, ksu_modules_devname) != 0))) {
		return 0;
	}

#ifdef CONFIG_KSU_DEBUG
	log_mount_info("Source", source_mnt);
	log_mount_info("Dest  ", dest_mnt);
#endif

	// We only need to act if the destination is a shared mount.
	if (!(dest_mnt->mnt.mnt_flags & MNT_SHARED)) {
		return 0;
	}

	pr_info("Spoofing shared mount %p to private.\n", dest_mnt);

	// --- The Spoof ---
	state->dest_mnt = dest_mnt;
	state->original_flags = dest_mnt->mnt.mnt_flags;
	state->spoofed = true;
	dest_mnt->mnt.mnt_flags &= ~MNT_SHARED;

	return 0;
}

static int ksu_attach_recursive_mnt_ret(struct kretprobe_instance *ri,
					struct pt_regs *regs)
{
	struct ksu_attach_mnt_state *state =
		(struct ksu_attach_mnt_state *)ri->data;

	if (!state->spoofed) {
		return 0;
	}

	// --- The Restoration ---
	pr_info("Restoring original shared flags to mount %p.\n",
		state->dest_mnt);
	state->dest_mnt->mnt.mnt_flags = state->original_flags;

	return 0;
}

static struct kretprobe attach_recursive_mnt_krp = {
	.handler = ksu_attach_recursive_mnt_ret,
	.entry_handler = ksu_attach_recursive_mnt_entry,
	.data_size = sizeof(struct ksu_attach_mnt_state),
	.maxactive =
		64, // Max concurrent probed instances. 64 is a safe default.
	.kp.symbol_name = "attach_recursive_mnt",
};

#endif // CONFIG_KPROBES

void ksu_set_zygote_started(void)
{
	pr_info("Zygote started, mount propagation logic is now active.\n");
	atomic_set(&ksu_zygote_started, 1);
}

int ksu_mount_hook_init(void)
{
#ifdef CONFIG_KPROBES
	int ret = register_kretprobe(&attach_recursive_mnt_krp);
	if (ret < 0) {
		pr_err("kretprobe registration failed, returned %d\n", ret);
		return ret;
	}

	pr_info("Mount propagation hook registered successfully.\n");
	return 0;
#else
	pr_info("Mount hook not enabled (CONFIG_KPROBES not set).\n");
	return 0;
#endif
}

void ksu_mount_hook_exit(void)
{
#ifdef CONFIG_KPROBES
	unregister_kretprobe(&attach_recursive_mnt_krp);
	pr_info("Mount propagation hook unregistered.\n");

	if (attach_recursive_mnt_krp.nmissed > 0) {
		pr_warn("Missed %u instances of attach_recursive_mnt probe.\n",
			attach_recursive_mnt_krp.nmissed);
	}
#endif
}
