#include <linux/printk.h>
#include <linux/rwsem.h>
#include <linux/string.h>
#include <linux/utsname.h>

#include "uapi/supercall.h"
#include "feature/version_spoof.h"

int ksu_set_spoof_version(struct ksu_set_spoof_version_cmd *cmd)
{
    /* Strictly enforce null-termination to prevent kernel panics */
    cmd->release[sizeof(cmd->release) - 1] = '\0';
    cmd->version[sizeof(cmd->version) - 1] = '\0';

    /* Acquire the UTS namespace write lock */
    down_write(&uts_sem);

    /* Overwrite the RAM where the kernel stored the compile-time strings */
    if (strlen(cmd->release) > 0) {
        strscpy(init_uts_ns.name.release, cmd->release, sizeof(init_uts_ns.name.release));
    }
    if (strlen(cmd->version) > 0) {
        strscpy(init_uts_ns.name.version, cmd->version, sizeof(init_uts_ns.name.version));
    }

    /* Release the lock */
    up_write(&uts_sem);

    pr_info("KernelSU Stealth: OS release spoofed to '%s'\n", init_uts_ns.name.release);
    pr_info("KernelSU Stealth: OS version spoofed to '%s'\n", init_uts_ns.name.version);

    return 0;
}
