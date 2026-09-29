#ifndef __KSU_H_UTIL
#define __KSU_H_UTIL

#include "linux/fdtable.h" // IWYU pragma: keep
#include <linux/version.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#define ksu_close_fd close_fd
#else
#define ksu_close_fd ksys_close
#endif

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
