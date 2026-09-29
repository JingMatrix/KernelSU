#ifndef __KSU_H_UTIL
#define __KSU_H_UTIL

#include "linux/fdtable.h" // IWYU pragma: keep
#include <linux/version.h>
#include <linux/syscalls.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/cred.h>
#include <linux/ptrace.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>

#include "arch.h"

#if defined(__aarch64__)
#define KSU_SYS_PREFIX(name) __arm64_sys_##name
#elif defined(__x86_64__)
#define KSU_SYS_PREFIX(name) __x64_sys_##name
#elif defined(__riscv)
#define KSU_SYS_PREFIX(name) __riscv_sys_##name
#else // wire up your arch here.
static_assert(1 == 0, "Unsupported architecture!");
#define KSU_SYS_PREFIX(name) sys_##name
#endif

/**
 * ksyscall: call syscalls from kernelspace
 * - tries to copy unistd's syscall()
 *
 * usage: ksyscall(close, fd);
 */
#define __ksyscall(name, a, b, c, d, e, f)                                                                             \
    ({                                                                                                                 \
        extern long KSU_SYS_PREFIX(name)(const struct pt_regs *);                                                      \
        struct pt_regs __ksu_regs = { 0 };                                                                             \
        PT_REGS_SYSCALL_PARM1(&__ksu_regs) = (unsigned long)(a);                                                       \
        PT_REGS_PARM2(&__ksu_regs) = (unsigned long)(b);                                                               \
        PT_REGS_PARM3(&__ksu_regs) = (unsigned long)(c);                                                               \
        PT_REGS_SYSCALL_PARM4(&__ksu_regs) = (unsigned long)(d);                                                       \
        PT_REGS_PARM5(&__ksu_regs) = (unsigned long)(e);                                                               \
        PT_REGS_PARM6(&__ksu_regs) = (unsigned long)(f);                                                               \
        (long)KSU_SYS_PREFIX(name)(&__ksu_regs);                                                                       \
    })

// https://elixir.bootlin.com/musl/v1.2.6/source/src/internal/syscall.h#L45
#define ksyscall_0(name) __ksyscall(name, 0, 0, 0, 0, 0, 0)
#define ksyscall_1(name, a) __ksyscall(name, a, 0, 0, 0, 0, 0)
#define ksyscall_2(name, a, b) __ksyscall(name, a, b, 0, 0, 0, 0)
#define ksyscall_3(name, a, b, c) __ksyscall(name, a, b, c, 0, 0, 0)
#define ksyscall_4(name, a, b, c, d) __ksyscall(name, a, b, c, d, 0, 0)
#define ksyscall_5(name, a, b, c, d, e) __ksyscall(name, a, b, c, d, e, 0)
#define ksyscall_6(name, a, b, c, d, e, f) __ksyscall(name, a, b, c, d, e, f)

#define __ksyscall_arg_n(_1, _2, _3, _4, _5, _6, _7, N, ...) N
#define __ksyscall_count_args(...) __ksyscall_arg_n(__VA_ARGS__, 6, 5, 4, 3, 2, 1, 0)
#define __ksyscall_concat(a, b) a##b
#define __ksyscall_exp(func, arg) __ksyscall_concat(func, arg)
#define ksyscall(...) __ksyscall_exp(ksyscall_, __ksyscall_count_args(__VA_ARGS__))(__VA_ARGS__)

#define ksu_close_fd(fd) ({ ksyscall(close, fd); })
#define ksu_sys_setns(fd, flags) ({ ksyscall(setns, fd, flags); })

static inline struct file *ksu_filp_open_nonotify(const char *path, int flags)
{
    struct path p;
    struct file *f;
    int ret;
    ret = kern_path(path, (flags & O_NOFOLLOW) ? 0 : LOOKUP_FOLLOW, &p);
    if (ret) {
        return ERR_PTR(ret);
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
    f = dentry_open_nonotify(&p, flags, current_cred());
#else
    f = dentry_open(&p, flags | __FMODE_NONOTIFY, current_cred());
#endif

    path_put(&p);
    return f;
}

/* task_work_add()'s third argument was `bool notify` before commit
 * 4799fc5b3bc3 ("task_work: cleanup notification modes", Linux 5.10)
 * replaced it with the task_work_notify_mode enum (ptctl.c,
 * policy/allowlist.c, supercall/supercall.c all pass TWA_RESUME). `true` is
 * the exact old equivalent -- TWA_RESUME's own value is 1, chosen to keep
 * every existing `true` caller correct across that change; neither the
 * enum nor `true` are preprocessor macros, so this cannot be detected with
 * `#ifndef TWA_RESUME` (an unguarded one would silently no-op on a 5.10+
 * build, since the identifier is never a macro there either -- this must
 * stay a version check). */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
#define TWA_RESUME true
#endif

/* mm/maccess.c's probe_kernel_{read,write}/probe_user_{read,write}/
 * strncpy_from_unsafe_user were renamed to copy_{from,to}_{kernel,user}_
 * nofault/strncpy_from_user_nofault in Linux 5.8 (Christoph Hellwig's
 * maccess rename series) -- same signature, same 0-on-success/-EFAULT-on-
 * fault convention, so the old name is an exact alias, not an
 * approximation. Real functions on both sides of 5.8, never macros, so
 * (as with TWA_RESUME above) this has to be a version check, not
 * `#ifndef`. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
#define copy_from_kernel_nofault probe_kernel_read
#define copy_to_kernel_nofault probe_kernel_write
#define copy_from_user_nofault probe_user_read
#define copy_to_user_nofault probe_user_write
#define strncpy_from_user_nofault strncpy_from_unsafe_user
#endif

#endif
