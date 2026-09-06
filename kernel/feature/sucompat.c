#include "linux/file.h"
#include "linux/fcntl.h"
#include "linux/namei.h"
#include <linux/compiler_types.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/uaccess.h>
#include <asm/current.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/sched/task_stack.h>
#include <linux/ptrace.h>

#include "arch.h"
#include "policy/allowlist.h"
#include "policy/feature.h"
#include "klog.h" // IWYU pragma: keep
#include "runtime/ksud.h"
#include "feature/sucompat.h"
#include "policy/app_profile.h"
#include "hook/syscall_hook.h"
#include "supercall/supercall.h"
#include "sulog/event.h"
#include "ksu.h"
#include "util.h"

#define SU_PATH "/system/bin/su"
#define SH_PATH "/system/bin/sh"

bool ksu_su_compat_enabled __read_mostly = true;

static int su_compat_feature_get(u64 *value)
{
    *value = ksu_su_compat_enabled ? 1 : 0;
    return 0;
}

static int su_compat_feature_set(u64 value)
{
    bool enable = value != 0;
    ksu_su_compat_enabled = enable;
    pr_info("su_compat: set to %d\n", enable);
    return 0;
}

static const struct ksu_feature_handler su_compat_handler = {
    .feature_id = KSU_FEATURE_SU_COMPAT,
    .name = "su_compat",
    .get_handler = su_compat_feature_get,
    .set_handler = su_compat_feature_set,
};

static void __user *userspace_stack_buffer(const void *d, size_t len)
{
    // To avoid having to mmap a page in userspace, just write below the stack
    // pointer.
    char __user *p = (void __user *)current_user_stack_pointer() - len;

    return copy_to_user(p, d, len) ? NULL : p;
}

static char __user *user_path_of(const char *path)
{
    return userspace_stack_buffer(path, strlen(path) + 1);
}

static char __user *empty_user_path(void)
{
    return userspace_stack_buffer("", sizeof(""));
}

static const char su_path[] = SU_PATH;

static bool is_ksud_exists()
{
    struct path path;

    if (kern_path(KSUD_PATH, 0, &path) < 0) {
        return false;
    }
    path_put(&path);
    return true;
}

/*
 * What a reference to `su` should resolve to, or NULL to leave it alone.
 *
 * Normally that is ksud, which is the su implementation; the kernel only
 * supplies the identity. Requiring it to exist is deliberate -- it is what
 * keeps a chrooted process, which cannot see it, from being redirected
 * unexpectedly.
 *
 * An image patched with allow_shell is the one case where there may be no
 * ksud to redirect to and su is still expected to work: the option grants
 * root to the shell precisely so a device can be worked on before any
 * manager has installed one. There, fall back to a plain root shell.
 *
 * That fallback is restricted to the shell itself rather than to everything
 * ksu_is_allow_uid_for_current() admits. A manager, and any uid in an
 * allowlist that survived on disk from an earlier install, can reach this
 * path too, and a bare shell answers `su -c <cmd> <user>` by running <cmd>
 * as root instead of as <user> -- so they keep the no-redirect behaviour.
 *
 * The caller must hold ksu_cred, because /data/adb is 0700 root and an
 * unprivileged caller cannot walk to KSUD_PATH itself -- which is exactly why
 * caller_uid is a parameter. Under the override current_uid() is ksu_cred's
 * uid, so reading it here would compare 0 against SHELL_UID and never match.
 */
static const char *su_target_path(uid_t caller_uid)
{
    if (is_ksud_exists()) {
        return KSUD_PATH;
    }
    if (unlikely(allow_shell && caller_uid == SHELL_UID)) {
        return SH_PATH;
    }
    return NULL;
}

long ksu_handle_faccessat_sucompat(int orig_nr, struct pt_regs *regs)
{
    const char __user **filename_user, *orig_filename, *target_filename;
    const uid_t caller_uid = current_uid().val;
    const char *target;
    long ret;
    const struct cred *old_cred;

    if (!ksu_is_allow_uid_for_current(caller_uid)) {
        goto do_orig_facessat;
    }

    filename_user = (const char __user **)&PT_REGS_PARM2(regs);

    char path[sizeof(su_path) + 1];
    memset(path, 0, sizeof(path));
    strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (unlikely(!memcmp(path, su_path, sizeof(su_path)))) {
        old_cred = override_creds(ksu_cred);
        target = su_target_path(caller_uid);
        target_filename = target ? user_path_of(target) : NULL;
        if (target_filename) {
            pr_info("faccessat su->%s!\n", target);
            orig_filename = *filename_user;
            *filename_user = target_filename;
            ret = ksu_syscall_table[orig_nr](regs);
            revert_creds(old_cred);
            *filename_user = orig_filename;
            return ret;
        } else {
            revert_creds(old_cred);
        }
    }

do_orig_facessat:
    return ksu_syscall_table[orig_nr](regs);
}

long ksu_handle_stat_sucompat(int orig_nr, struct pt_regs *regs)
{
    const char __user **filename_user, *orig_filename, *target_filename;
    const uid_t caller_uid = current_uid().val;
    const char *target;
    long ret;
    const struct cred *old_cred;

    if (!ksu_is_allow_uid_for_current(caller_uid)) {
        goto do_orig_stat;
    }

    filename_user = (const char __user **)&PT_REGS_PARM2(regs);

    char path[sizeof(su_path) + 1];
    memset(path, 0, sizeof(path));
    strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (unlikely(!memcmp(path, su_path, sizeof(su_path)))) {
        old_cred = override_creds(ksu_cred);
        target = su_target_path(caller_uid);
        target_filename = target ? user_path_of(target) : NULL;
        if (target_filename) {
            pr_info("newfstatat su->%s!\n", target);
            orig_filename = *filename_user;
            *filename_user = target_filename;
            ret = ksu_syscall_table[orig_nr](regs);
            revert_creds(old_cred);
            *filename_user = orig_filename;
            return ret;
        } else {
            revert_creds(old_cred);
        }
    }

do_orig_stat:
    return ksu_syscall_table[orig_nr](regs);
}

static long ksu_handle_execve_sucompat_common(const char __user **filename_user,
                                              const char __user *const __user *argv_user, unsigned long envp,
                                              bool execveat, int orig_nr, struct pt_regs *regs)
{
    const char __user *fn;
    const uid_t caller_uid = current_uid().val;
    struct ksu_sulog_pending_event *pending_sucompat = NULL;
    char path[sizeof(su_path) + 1];
    long ret, orig_regs[5];
    unsigned long addr;
    int su_fd = -1;
    int tmp_fd;
    const char *target;
    struct file *target_file;
    const struct cred *old_cred;

    if (execveat && ((int)PT_REGS_PARM1(regs) != AT_FDCWD || (int)PT_REGS_PARM5(regs) != 0))
        goto do_orig_execve;

    if (unlikely(!filename_user))
        goto do_orig_execve;

    if (!ksu_is_allow_uid_for_current(caller_uid))
        goto do_orig_execve;

    addr = untagged_addr((unsigned long)*filename_user);
    fn = (const char __user *)addr;
    memset(path, 0, sizeof(path));

    ret = strncpy_from_user(path, fn, sizeof(path));

    if (ret < 0) {
        pr_warn("Access filename when execve failed: %ld", ret);
        goto do_orig_execve;
    }

    if (likely(memcmp(path, su_path, sizeof(su_path))))
        goto do_orig_execve;

    pr_info("sys_execve su found\n");

    tmp_fd = get_unused_fd_flags(O_CLOEXEC);
    if (tmp_fd < 0) {
        pr_err("alloc tmp fd err: %d\n", tmp_fd);
        goto do_orig_execve;
    }

    old_cred = override_creds(ksu_cred);
    target = su_target_path(caller_uid);
    target_file = target ? filp_open(target, O_PATH, 0) : ERR_PTR(-ENOENT);
    revert_creds(old_cred);
    if (IS_ERR(target_file)) {
        pr_err("open su target err: %ld\n", PTR_ERR(target_file));
        put_unused_fd(tmp_fd);
        goto do_orig_execve;
    }
    pr_info("execve su->%s!\n", target);

    fd_install(tmp_fd, target_file);

    pending_sucompat = ksu_sulog_capture_sucompat(*filename_user, argv_user, GFP_KERNEL);
    // execve(file, argv, environ)
    // execveat(fd, file, argv, environ, flags)
    orig_regs[0] = regs->__PT_PARM1_REG;
    orig_regs[1] = regs->__PT_PARM2_REG;
    orig_regs[2] = regs->__PT_PARM3_REG;
    orig_regs[3] = regs->__PT_SYSCALL_PARM4_REG;
    orig_regs[4] = regs->__PT_PARM5_REG;
    regs->__PT_PARM5_REG = AT_EMPTY_PATH;
    regs->__PT_SYSCALL_PARM4_REG = envp;
    regs->__PT_PARM3_REG = (unsigned long)argv_user;
    regs->__PT_PARM2_REG = empty_user_path();
    regs->__PT_PARM1_REG = tmp_fd;

    ret = escape_with_root_profile();
    if (ret) {
        pr_err("escape_with_root_profile failed: %ld\n", ret);
    }
    ksu_sulog_emit_pending(pending_sucompat, ret, GFP_KERNEL);

    ret = ksu_syscall_table[__NR_execveat](regs);
    if (ret < 0) {
        ksu_close_fd(tmp_fd);
        regs->__PT_PARM1_REG = orig_regs[0];
        regs->__PT_PARM2_REG = orig_regs[1];
        regs->__PT_PARM3_REG = orig_regs[2];
        regs->__PT_SYSCALL_PARM4_REG = orig_regs[3];
        regs->__PT_PARM5_REG = orig_regs[4];
    } else {
        // Only grant the scoped driver capability after the selected root
        // profile has been applied successfully.
        su_fd = ksu_install_su_fd();
        if (su_fd < 0) {
            pr_warn("install su session fd failed: %d\n", su_fd);
        }
    }
    return ret;

do_orig_execve:
    return ksu_syscall_table[orig_nr](regs);
}

long ksu_handle_execve_sucompat(const char __user **filename_user, int orig_nr, struct pt_regs *regs)
{
    return ksu_handle_execve_sucompat_common(filename_user, (const char __user *const __user *)PT_REGS_PARM2(regs),
                                             PT_REGS_PARM3(regs), false, orig_nr, regs);
}

long ksu_handle_execveat_sucompat(const char __user **filename_user, int orig_nr, struct pt_regs *regs)
{
    return ksu_handle_execve_sucompat_common(filename_user, (const char __user *const __user *)PT_REGS_PARM3(regs),
                                             PT_REGS_SYSCALL_PARM4(regs), true, orig_nr, regs);
}

// sucompat: permitted process can execute 'su' to gain root access.
void __init ksu_sucompat_init()
{
    if (ksu_register_feature_handler(&su_compat_handler)) {
        pr_err("Failed to register su_compat feature handler\n");
    }
}

void __exit ksu_sucompat_exit()
{
    ksu_unregister_feature_handler(KSU_FEATURE_SU_COMPAT);
}
