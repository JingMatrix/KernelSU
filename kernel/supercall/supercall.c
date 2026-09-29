#include <linux/anon_inodes.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "uapi/supercall.h"
#include "supercall/internal.h"
#include "arch.h"
#include "util.h"
#include "klog.h" // IWYU pragma: keep
#include "hook/syscall_hook.h"
#include "hook/kprobe_patch_compat.h"

#define KSU_DRIVER_PERMISSION_SU_SESSION (1UL << 0)

struct ksu_driver_context {
    unsigned long permissions;
};

struct ksu_install_fd_tw {
    struct callback_head cb;
    int __user *outp;
};

static int anon_ksu_release(struct inode *inode, struct file *filp)
{
    kfree(filp->private_data);
    pr_info("ksu fd released\n");
    return 0;
}

static long anon_ksu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    return ksu_supercall_handle_ioctl(filp, cmd, (void __user *)arg);
}

static const struct file_operations anon_ksu_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = anon_ksu_ioctl,
    .compat_ioctl = anon_ksu_ioctl,
    .release = anon_ksu_release,
};

static int ksu_install_fd_with_permissions(unsigned int fd_flags, unsigned long permissions)
{
    struct ksu_driver_context *context;
    struct file *filp;
    const char *name;
    int fd;

    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return -ENOMEM;

    context->permissions = permissions;
    name = permissions & KSU_DRIVER_PERMISSION_SU_SESSION ? "[ksu_driver_su]" : "[ksu_driver]";

    fd = get_unused_fd_flags(fd_flags);
    if (fd < 0) {
        pr_err("ksu_install_fd: failed to get unused fd\n");
        kfree(context);
        return fd;
    }

    filp = anon_inode_getfile(name, &anon_ksu_fops, context, O_RDWR);
    if (IS_ERR(filp)) {
        pr_err("ksu_install_fd: failed to create anon inode file\n");
        put_unused_fd(fd);
        kfree(context);
        return PTR_ERR(filp);
    }

    fd_install(fd, filp);
    pr_info("ksu fd installed: %d for pid %d\n", fd, current->pid);
    return fd;
}

int ksu_install_fd(void)
{
    return ksu_install_fd_with_permissions(O_CLOEXEC, 0);
}

int ksu_install_su_fd(void)
{
    // This descriptor must be installed after the exec into ksud.
    return ksu_install_fd_with_permissions(O_CLOEXEC, KSU_DRIVER_PERMISSION_SU_SESSION);
}

bool ksu_is_su_session_fd(const struct file *filp)
{
    const struct ksu_driver_context *context = filp->private_data;

    return context && (context->permissions & KSU_DRIVER_PERMISSION_SU_SESSION);
}

static void ksu_install_fd_tw_func(struct callback_head *cb)
{
    struct ksu_install_fd_tw *tw = container_of(cb, struct ksu_install_fd_tw, cb);
    int fd = ksu_install_fd();

    pr_info("[%d] install ksu fd: %d\n", current->pid, fd);
    if (copy_to_user(tw->outp, &fd, sizeof(fd))) {
        pr_err("install ksu fd reply err\n");
        ksu_close_fd(fd);
    }

    kfree(tw);
}

static int reboot_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct pt_regs *real_regs = PT_REAL_REGS(regs);
    int magic1 = (int)PT_REGS_PARM1(real_regs);
    int magic2 = (int)PT_REGS_PARM2(real_regs);

    if (magic1 == KSU_INSTALL_MAGIC1 && magic2 == KSU_INSTALL_MAGIC2) {
        struct ksu_install_fd_tw *tw;
        unsigned long arg4 = (unsigned long)PT_REGS_SYSCALL_PARM4(real_regs);

        tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
        if (!tw)
            return 0;

        tw->outp = (int __user *)arg4;
        tw->cb.func = ksu_install_fd_tw_func;

        if (task_work_add(current, &tw->cb, TWA_RESUME)) {
            kfree(tw);
            pr_warn("install fd add task_work failed\n");
        }
    }

    return 0;
}

static struct kprobe reboot_kp = {
    .symbol_name = REBOOT_SYMBOL,
    .pre_handler = reboot_handler_pre,
};

static bool reboot_kp_registered;

/*
 * Compat install handshake for a kernel where reboot_kp above is unsafe to
 * trigger (ksu_kprobe_text_patch_unsafe(), hook/kprobe_patch_compat.h) --
 * confirmed on hardware to crash exactly the way syscall_regfunc's
 * kretprobe does: the BRK write lands and verifies clean, but the CPU
 * still faults fetching it, before reboot_handler_pre ever runs (PC at the
 * probed symbol's own address+0).
 *
 * Keeps reboot(2) as the install-handshake syscall exactly as userspace
 * already calls it -- the difference is *how* the hook goes in.
 * reboot_kp's crash is about *executing a newly injected instruction* in
 * already-existing, protected .text. Overwriting
 * sys_call_table[__NR_reboot]'s pointer is a different kind of write -- a
 * data write in a table, redirecting to code that already exists and
 * already executes fine (this module's own), never injecting a single new
 * instruction into anything. Confirmed on hardware: ksud's own,
 * completely unmodified reboot(2) call gets a live driver version through
 * this path. (An earlier prctl(2)-based LSM-hook fallback was also built
 * and verified working, but is not needed -- this keeps the existing
 * reboot(2) wire protocol intact, which the prctl(2) path could not.)
 */
static syscall_fn_t real_reboot_fn;
static bool reboot_table_hooked;

static long ksu_reboot_table_replacement(const struct pt_regs *regs)
{
    int magic1 = (int)PT_REGS_PARM1(regs);
    int magic2 = (int)PT_REGS_PARM2(regs);

    if (magic1 == (int)KSU_INSTALL_MAGIC1 && magic2 == (int)KSU_INSTALL_MAGIC2) {
        unsigned long arg4 = (unsigned long)PT_REGS_SYSCALL_PARM4(regs);
        int fd = ksu_install_fd();

        pr_info("[%d] install ksu fd (table-hook compat): %d\n", current->pid, fd);
        if (copy_to_user((int __user *)arg4, &fd, sizeof(fd))) {
            pr_err("install ksu fd reply err (table-hook compat)\n");
            ksu_close_fd(fd);
        }
        return 0;
    }

    return real_reboot_fn(regs);
}

void __init ksu_supercalls_init(void)
{
    int rc;

    ksu_supercall_dump_commands();

    if (ksu_kprobe_text_patch_unsafe() && ksu_syscall_table) {
        ksu_syscall_table_hook(__NR_reboot, (syscall_fn_t)ksu_reboot_table_replacement, &real_reboot_fn);
        pr_info("reboot table-hook compat installed (reboot kprobe unsafe on this kernel)\n");
        reboot_table_hooked = true;
        return;
    }

    rc = register_kprobe(&reboot_kp);
    if (rc) {
        pr_err("reboot kprobe failed: %d\n", rc);
        return;
    }
    pr_info("reboot kprobe registered successfully\n");
    reboot_kp_registered = true;
}

void __exit ksu_supercalls_exit(void)
{
    if (reboot_table_hooked)
        ksu_syscall_table_unhook(__NR_reboot);
    if (reboot_kp_registered)
        unregister_kprobe(&reboot_kp);
    ksu_supercall_cleanup_state();
}
