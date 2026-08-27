#ifndef __KSU_UAPI_SUPERCALL_H
#define __KSU_UAPI_SUPERCALL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#include "uapi/app_profile.h"

// 2: allowlist v4 root profile flags
static const __u32 KERNEL_SU_UAPI_VERSION = 2;

/* Magic numbers for reboot hook to install fd */
static const __u32 KSU_INSTALL_MAGIC1 = 0xDEADBEEF;
static const __u32 KSU_INSTALL_MAGIC2 = 0xCAFEBABE;

struct ksu_become_daemon_cmd {
    __u8 token[65]; /* Input: daemon token (null-terminated) */
};

static const __u32 EVENT_POST_FS_DATA = 1;
static const __u32 EVENT_BOOT_COMPLETED = 2;
static const __u32 EVENT_MODULE_MOUNTED = 3;

static const __u32 KSU_GET_INFO_FLAG_LKM = (1U << 0);
static const __u32 KSU_GET_INFO_FLAG_MANAGER = (1U << 1);
static const __u32 KSU_GET_INFO_FLAG_LATE_LOAD = (1U << 2);
static const __u32 KSU_GET_INFO_FLAG_PR_BUILD = (1U << 3);

struct ksu_get_info_cmd {
    __u32 version; /* Output: KERNEL_SU_VERSION */
    __u32 flags; /* Output: KSU_GET_INFO_FLAG_* bits */
    __u32 features; /* Output: max feature ID supported */
    __u32 uapi_version; /* Output: KERNEL_SU_UAPI_VERSION */
};

struct ksu_get_info_legacy_cmd {
    __u32 version; /* Output: KERNEL_SU_VERSION */
    __u32 flags; /* Output: KSU_GET_INFO_FLAG_* bits */
    __u32 features; /* Output: max feature ID supported */
};

struct ksu_report_event_cmd {
    __u32 event; /* Input: EVENT_POST_FS_DATA, EVENT_BOOT_COMPLETED, etc. */
};

struct ksu_set_sepolicy_cmd {
    __u64 data_len; /* Input: bytes of serialized command payload */
    __aligned_u64 data; /* Input: pointer to serialized payload */
};

struct ksu_sepolicy_cmd_hdr {
    __u32 cmd; /* Input: command type, CMD_* */
    __u32 subcmd; /* Input: command subtype */
};
/*
 * After each ksu_sepolicy_cmd_hdr, command arguments are encoded sequentially as:
 * [u32 len][len bytes][\0], where len excludes the trailing '\0'.
 * len == 0 represents ALL.
 * Argument count is derived from cmd:
 * KSU_SEPOLICY_CMD_NORMAL_PERM=4, KSU_SEPOLICY_CMD_XPERM=5,
 * KSU_SEPOLICY_CMD_TYPE_STATE=1, KSU_SEPOLICY_CMD_TYPE=2,
 * KSU_SEPOLICY_CMD_TYPE_ATTR=2, KSU_SEPOLICY_CMD_ATTR=1,
 * KSU_SEPOLICY_CMD_TYPE_TRANSITION=5, KSU_SEPOLICY_CMD_TYPE_CHANGE=4,
 * KSU_SEPOLICY_CMD_GENFSCON=3.
 */

struct ksu_check_safemode_cmd {
    __u8 in_safe_mode; /* Output: true if in safe mode, false otherwise */
};

/* deprecated */
struct ksu_get_allow_list_cmd {
    __u32 uids[128]; /* Output: array of allowed/denied UIDs */
    __u32 count; /* Output: number of UIDs in array */
    __u8 allow; /* Input: true for allow list, false for deny list */
};

struct ksu_new_get_allow_list_cmd {
    __u16 count; /* Input / Output: number of UIDs in array */
    __u16 total_count; /* Output: total number of UIDs in requested list */
    __u32 uids[0]; /* Output: array of allowed/denied UIDs */
};

struct ksu_uid_granted_root_cmd {
    __u32 uid; /* Input: target UID to check */
    __u8 granted; /* Output: true if granted, false otherwise */
};

struct ksu_uid_should_umount_cmd {
    __u32 uid; /* Input: target UID to check */
    __u8 should_umount; /* Output: true if should umount, false otherwise */
};

struct ksu_get_manager_appid_cmd {
    __u32 appid; /* Output: manager app id */
};

struct ksu_get_app_profile_cmd {
    struct app_profile profile; /* Input/Output: app profile structure */
};

struct ksu_set_app_profile_cmd {
    struct app_profile profile; /* Input: app profile structure */
};

struct ksu_get_feature_cmd {
    __u32 feature_id; /* Input: feature ID (enum ksu_feature_id) */
    __u64 value; /* Output: feature value/state */
    __u8 supported; /* Output: true if feature is supported, false otherwise */
};

struct ksu_set_feature_cmd {
    __u32 feature_id; /* Input: feature ID (enum ksu_feature_id) */
    __u64 value; /* Input: feature value/state to set */
};

struct ksu_get_wrapper_fd_cmd {
    __u32 fd; /* Input: userspace fd */
    __u32 flags; /* Input: flags of userspace fd */
};

struct ksu_manage_mark_cmd {
    __u32 operation; /* Input: KSU_MARK_* */
    __s32 pid; /* Input: target pid (0 for all processes) */
    __u32 result; /* Output: for get operation - mark status or reg_count */
};

static const __u32 KSU_MARK_GET = 1;
static const __u32 KSU_MARK_MARK = 2;
static const __u32 KSU_MARK_UNMARK = 3;
static const __u32 KSU_MARK_REFRESH = 4;

struct ksu_nuke_ext4_sysfs_cmd {
    __aligned_u64 arg; /* Input: mnt pointer */
};

struct ksu_add_try_umount_cmd {
    __aligned_u64 arg; /* char ptr, this is the mountpoint */
    __u32 flags; /* this is the flag we use for it */
    __u8 mode; /* denotes what to do with it 0:wipe_list 1:add_to_list 2:delete_entry */
};

struct ksu_get_sulog_fd_cmd {
    __u32 flags; /* Input: reserved for future use, must be 0 */
};

struct ksu_set_spoof_version_cmd {
    __u8 release[65]; /* Input: e.g., "5.10.115-android12-9-g00000000" */
    __u8 version[65]; /* Input: e.g., "#1 SMP PREEMPT Thu Jan 1 00:00:00 UTC 2026" */
};

struct ksu_set_spoof_cpu_cmd {
    __u32 cpu_index;  /* Target processor core index */
    __u32 midr;       /* Main ID Register payload */
    __u32 bogomips;   /* BogoMIPS performance timing metric */
    __u64 hwcap;      /* Main ELF Hardware Capabilities mask */
    __u64 hwcap2;     /* Auxiliary ELF Hardware Capabilities mask */
};

struct ksu_set_spoof_mem_cmd {
    __u64 total_ram_bytes; /* Target total memory size in bytes (e.g. 8GB) */
    __u64 cma_total_bytes; /* Target total CMA size in bytes (e.g. 512MB), can be 0 */
};

static const __u8 KSU_UMOUNT_WIPE = 0; /* ignore everything and wipe list */
static const __u8 KSU_UMOUNT_ADD = 1; /* add entry (path + flags) */
static const __u8 KSU_UMOUNT_DEL = 2; /* delete entry, strcmp */

/* IOCTL command definitions */
static const __u32 KSU_IOCTL_GRANT_ROOT = _IOC(_IOC_NONE, 'K', 1, 0);
static const __u32 KSU_IOCTL_GET_INFO = _IOR('K', 2, struct ksu_get_info_cmd);
/* deprecated */
static const __u32 KSU_IOCTL_GET_INFO_LEGACY = _IOC(_IOC_READ, 'K', 2, 0);
static const __u32 KSU_IOCTL_REPORT_EVENT = _IOC(_IOC_WRITE, 'K', 3, 0);
static const __u32 KSU_IOCTL_SET_SEPOLICY = _IOC(_IOC_READ | _IOC_WRITE, 'K', 4, 0);
static const __u32 KSU_IOCTL_CHECK_SAFEMODE = _IOC(_IOC_READ, 'K', 5, 0);
/* deprecated */
static const __u32 KSU_IOCTL_GET_ALLOW_LIST = _IOC(_IOC_READ | _IOC_WRITE, 'K', 6, 0);
/* deprecated */
static const __u32 KSU_IOCTL_GET_DENY_LIST = _IOC(_IOC_READ | _IOC_WRITE, 'K', 7, 0);
static const __u32 KSU_IOCTL_NEW_GET_ALLOW_LIST = _IOWR('K', 6, struct ksu_new_get_allow_list_cmd);
static const __u32 KSU_IOCTL_NEW_GET_DENY_LIST = _IOWR('K', 7, struct ksu_new_get_allow_list_cmd);
static const __u32 KSU_IOCTL_UID_GRANTED_ROOT = _IOC(_IOC_READ | _IOC_WRITE, 'K', 8, 0);
static const __u32 KSU_IOCTL_UID_SHOULD_UMOUNT = _IOC(_IOC_READ | _IOC_WRITE, 'K', 9, 0);
static const __u32 KSU_IOCTL_GET_MANAGER_APPID = _IOC(_IOC_READ, 'K', 10, 0);
static const __u32 KSU_IOCTL_GET_APP_PROFILE = _IOC(_IOC_READ | _IOC_WRITE, 'K', 11, 0);
static const __u32 KSU_IOCTL_SET_APP_PROFILE = _IOC(_IOC_WRITE, 'K', 12, 0);
static const __u32 KSU_IOCTL_GET_FEATURE = _IOC(_IOC_READ | _IOC_WRITE, 'K', 13, 0);
static const __u32 KSU_IOCTL_SET_FEATURE = _IOC(_IOC_WRITE, 'K', 14, 0);
static const __u32 KSU_IOCTL_GET_WRAPPER_FD = _IOC(_IOC_WRITE, 'K', 15, 0);
static const __u32 KSU_IOCTL_MANAGE_MARK = _IOC(_IOC_READ | _IOC_WRITE, 'K', 16, 0);
static const __u32 KSU_IOCTL_NUKE_EXT4_SYSFS = _IOC(_IOC_WRITE, 'K', 17, 0);
static const __u32 KSU_IOCTL_ADD_TRY_UMOUNT = _IOC(_IOC_WRITE, 'K', 18, 0);
static const __u32 KSU_IOCTL_SET_INIT_PGRP = _IO('K', 19);
static const __u32 KSU_IOCTL_GET_SULOG_FD = _IOW('K', 20, struct ksu_get_sulog_fd_cmd);
static const __u32 KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT = _IO('K', 21);

static const __u32 KSU_IOCTL_SET_SPOOF_VERSION = _IOC(_IOC_WRITE, 'K', 42, 0);
static const __u32 KSU_IOCTL_SET_SPOOF_CPU = _IOC(_IOC_WRITE, 'K', 43, 0);
static const __u32 KSU_IOCTL_SET_SPOOF_MEM = _IOC(_IOC_WRITE, 'K', 44, 0);

/* i386 aligns __u64 to 4 bytes while every 64-bit arch aligns it to 8, and the
 * driver points .compat_ioctl at the same handler, so every 64-bit field in
 * these structs is force-aligned to keep one layout for both. */
#ifndef __aligned_s64
#define __aligned_s64 __s64 __attribute__((aligned(8)))
#endif

/* ---- ptctl: general process control / debug primitives (root only) ----
 * One op-dispatched ioctl so many operations ship in a single kernel build.
 * Kernel-unique capabilities userspace root cannot do: block another process's
 * signals, and read/write/inspect an arbitrary task without ptrace (invisible to
 * self-ptrace anti-debug + no TracerPid). */
enum ksu_ptctl_op {
    /* A SHORT transfer is success, not failure: access_process_vm() stops at the first page it
     * cannot reach, so a request that runs off the end of a mapping returns ret < len with no
     * error. PEEK leaves the tail of your buffer untouched -- whatever was there before will read
     * back as if it were target memory -- and POKE has written only the first ret bytes. Always
     * compare ret against len. Total failure (no mm, wholly unmapped, refused) is -EIO. */
    KSU_PTCTL_PEEK          = 1,  /* read  task mem: pid, addr, len(<=64K), uptr(out) -> ret=bytes */
    KSU_PTCTL_POKE          = 2,  /* write task mem: pid, addr, len(<=64K), uptr(in)  -> ret=bytes */
    /* GETREGS/SETREGS transfer exactly the USER register view -- struct
     * user_pt_regs (272 B) on arm64, struct pt_regs (168 B) on x86_64 -- never
     * the kernel-private tail of struct pt_regs. Pass len = 0 for "the whole
     * user view"; any other value must match that size exactly or you get
     * -EINVAL. SETREGS sanitises the incoming frame the way PTRACE_SETREGSET
     * does (arm64 valid_user_regs; x86_64 pins cs/ss/orig_ax and masks eflags),
     * refuses the calling thread itself, and refuses a target that is not
     * off-CPU (-EBUSY) because a running task's frame is rewritten by the next
     * kernel entry anyway. A thread parked by HWBP_WAIT always qualifies.
     * GETREGS on your OWN tid is answered without that pin and returns the
     * frame you entered this ioctl with -- useful, but not "where that thread
     * is running", which is what the same call means for any other tid.
     * SETREGS commits a SANITISED frame: pstate/eflags bits the kernel owns are
     * forced regardless of what you wrote, so read back with GETREGS whenever
     * the exact value matters. */
    KSU_PTCTL_GETREGS       = 3,  /* read user regs of tid: pid, uptr(out), len=0  -> ret=bytes */
    KSU_PTCTL_SETREGS       = 4,  /* write user regs of tid: pid, uptr(in), len=0  -> ret=bytes */
    KSU_PTCTL_INFO          = 5,  /* query task: pid -> arg1=tracer_pid arg2=tgid ret=1 if exists */
    /* Guards the whole thread group of `pid` (a pid or a tid) against signals
     * that would terminate it and that are INJECTED by another task through
     * do_send_sig_info() -- kill(2), tgkill(2), rt_sigqueueinfo(2),
     * pidfd_send_signal(2), cgroup.kill, the OOM killer. It cannot stop a
     * synchronous fault (a real SIGSEGV/SIGBUS/SIGILL/SIGFPE never enters that
     * path), exec's zap_other_threads(), seccomp's do_exit(), or the OOM
     * reaper. arg2 returns the tgid actually guarded. -ENOSYS if the kprobe
     * could not be installed. */
    /* The guard table holds bare tgids and has no exit hook, so an entry left behind by a process
     * that has died is inherited by whatever later process recycles that tgid. Delete a guard when
     * you are done with it. Removal by raw tgid works after the process is gone; adding needs a
     * live task (-ESRCH otherwise), and the table holds 32 entries (-ENOSPC). */
    KSU_PTCTL_KILLGUARD     = 6,  /* protect a tgid from lethal signals: pid, arg1(1=add,0=del) */
    /* The ioctl returns 0 once the signal was handed to the signal core; the core's own result is
     * in ret, so a failed delivery looks like success unless you read it. */
    KSU_PTCTL_SIGSEND       = 7,  /* send signal arg1 (1.._NSIG-1) to pid; 0 is rejected, use INFO */
    KSU_PTCTL_DETACH_TRACER = 8,  /* force-detach pid from its ptracer (experimental) */
    /* --- hold-breakpoint: a kernel HW breakpoint that PAUSES the hitting thread
     * so peek/poke/regs can inspect+step obfuscated code, then release. No ptrace.
     *
     * NEVER hold a thread at an address a KSU_IOCTL_UHOOK uprobe also covers. Both features drive
     * the SAME per-thread arm64 single-step machinery, and single_step_handler() gives the
     * breakpoint first refusal: reinstall_suspended_bps() consumes the step exception and returns
     * "resume", so call_step_hook() -- and with it uprobes' arch_uprobe_post_xol() -- never runs.
     * The uprobe's pc is then never restored from the out-of-line slot and the thread runs on
     * inside it. The reverse interleaving loses the hold's step instead and the released thread
     * re-traps on the same instruction forever. Neither is reported anywhere; the only defence is
     * not to arm both at one address.
     *
     * SET arms the threads that exist AT THAT MOMENT and never follows new ones: a thread the app
     * spawns afterwards executes the address unwatched. It reports ret = threads armed and
     * arg2 = threads in the group, and ret < arg2 means the arm is partial -- a per-thread
     * register_user_hw_breakpoint() failure (a thread already using its four debug registers, or
     * exiting) is skipped, and a group larger than 640 threads is truncated. A partial arm is the
     * dangerous case: the site looks unvisited when only the unarmed threads reach it. It is not
     * hypothetical -- a 701-thread group armed 640, leaving 61 threads unwatched, and before this
     * change the only trace of that was a dmesg line. Re-issue CLEAR + SET to pick up threads
     * created since. */
    KSU_PTCTL_HWBP_SET      = 9,  /* pid=tgid, addr (4-byte aligned) -> ret=threads armed, arg2=threads in group */
    /* On a hit, uptr receives the same user register view as GETREGS and len
     * follows the same rule (0, or exactly that size). WAIT returns only once
     * the hitting thread has genuinely parked, so the SETREGS/POKE that
     * follows is guaranteed to find it off-CPU -- and arg1 comes back 1 when it
     * did park, 0 when the hold ended before it ever parked.
     * Caveat: the park is an interruptible sleep, so a signal delivered to the
     * held thread (an app's own timer or GC signal will do it) ends the hold
     * early and indistinguishably from a RELEASE. arg1 only reports the state
     * at the moment WAIT returned; nothing tells you the thread woke up in the
     * middle of a long inspection. SETREGS answering -EBUSY is the reliable
     * after-the-fact tell; PEEK is the trap, because it will read a running
     * target and hand back plausible bytes. Do NOT use HWBP_RELEASE as the
     * test: hwbp_held is cleared at the very END of the hold, so RELEASE still
     * answers 0 for a thread that has already been woken and is on its way out,
     * and two RELEASEs in a row on one hold both succeed. -ENOENT from it means
     * "nobody is held"; 0 does not mean "somebody still is". Re-arm rather
     * than assume.
     * Only ONE thread is held at a time; hits taken by other threads meanwhile
     * are dropped, and their count is readable at HWBP_CLEAR.
     * -ENOENT if no breakpoint is armed, so a wait on nothing cannot be
     * mistaken for a target that never reaches the address. */
    KSU_PTCTL_HWBP_WAIT     = 10, /* block up to arg1 ms; on hit: uptr<-regs, arg2=tid, arg1=parked, ret=1; 0=timeout */
    KSU_PTCTL_HWBP_RELEASE  = 11, /* resume the currently-held thread; -ENOENT if none is held */
    KSU_PTCTL_HWBP_CLEAR    = 12, /* remove the bp (and release any held thread) -> ret=hits dropped since SET */
};

struct ksu_ptctl_cmd {
    __u32 op;              /* Input: enum ksu_ptctl_op */
    __s32 pid;             /* Input: target pid or tid */
    __aligned_u64 addr;    /* Input: target address (peek/poke) */
    __aligned_u64 len;     /* Input: byte length (peek/poke/regs) */
    __aligned_u64 uptr;    /* Input/Output: userspace buffer */
    __aligned_u64 arg1;    /* Input: op-specific (HWBP_WAIT also returns the parked flag here) */
    __aligned_u64 arg2;    /* Output: op-specific */
    /* ret and arg2 are cleared by the kernel before every op, so a field the op does not write
     * reads back 0 instead of echoing the value you passed in. */
    __aligned_s64 ret;     /* Output: op-specific result */
};

/* NOTE: the GETREGS/SETREGS/HWBP_WAIT wire format changed from
 * sizeof(struct pt_regs) to the user-visible register view. struct
 * ksu_ptctl_cmd itself is unchanged, so the ioctl number is unchanged and an
 * out-of-date caller is NOT rejected by the dispatcher -- it will simply get
 * -EINVAL from the length check. Rebuild every consumer from this header. */
static const __u32 KSU_IOCTL_PTCTL = _IOWR('K', 50, struct ksu_ptctl_cmd);

#endif
