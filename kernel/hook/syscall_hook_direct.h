#ifndef __KSU_H_SYSCALL_HOOK_DIRECT
#define __KSU_H_SYSCALL_HOOK_DIRECT

/*
 * Fallback syscall interception for a boot where the tracepoint dispatcher
 * is unsafe to install (ksu_kprobe_text_patch_unsafe(),
 * hook/kprobe_patch_compat.h -- the dispatcher needs a kretprobe on
 * syscall_regfunc, one of the two confirmed-unsafe targets that check
 * exists for) -- ksu_dispatcher_nr stays unset in that case, and
 * ksu_syscall_hook_manager_init() calls this instead.
 *
 * This places a kprobe directly on each hooked syscall's own entry point --
 * reading its address out of sys_call_table, never writing the table -- and
 * redirects execution straight into the same ksu_hook_* implementations the
 * dispatcher path calls (hook/syscall_event_bridge.h), unchanged. This is
 * not a new mechanism: it is the one KernelSU used exclusively before the
 * tracepoint-dispatcher model existed (see kernel/ksud.c as of the parent of
 * upstream commit 225ffbbf, "use syscall table hook to avoid running in
 * atomic context" -- execve_kp/sys_read_kp/input_event_kp there are the same
 * plain-kprobe-on-a-symbol idiom this file uses), revived for the one case
 * that still needs it rather than reimplemented from scratch.
 */
int ksu_syscall_hook_direct_init(void);
void ksu_syscall_hook_direct_exit(void);

#endif
