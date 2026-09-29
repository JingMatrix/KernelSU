#ifndef __KSU_H_KPROBE_PATCH_COMPAT
#define __KSU_H_KPROBE_PATCH_COMPAT

/*
 * Whether a kprobe's BRK-based patch is safe to arm and trigger on a
 * specific, small set of kernel text addresses this boot.
 *
 * Some vendor kernels put certain kernel text addresses under a protection
 * layer that lives outside Linux entirely -- a hypervisor stage-2 mapping,
 * a TrustZone-side integrity check -- which this kernel's own page tables
 * have no visibility into: a kprobe's write can read back as having
 * landed, with completely normal-looking permission bits, while the next
 * execution through the patched instruction still faults. Confirmed on
 * hardware for two specific targets on one platform: a kretprobe on
 * syscall_regfunc (armed by ksu_syscall_hook_init()'s tracepoint-dispatcher
 * path) and a plain kprobe on __arm64_sys_reboot (the install-handshake
 * hook, supercall/supercall.c) both fault this way. Two other things
 * confirmed NOT to trigger it on the same hardware: five other kprobes
 * placed the identical way on ordinary syscall entries (execve, execveat,
 * newfstatat, faccessat, setresuid -- hook/syscall_hook_direct.c), and a
 * plain data write to sys_call_table[__NR_reboot]'s own pointer (no BRK
 * involved at all, supercall.c's own compat path). So this is not "any
 * kprobe" and it is not "any write to a protected table" -- it is
 * something narrower, tied to specific addresses, that this project does
 * not have a full explanation for.
 *
 * There is no safe way to test the real thing at runtime -- the test would
 * be the same fault -- and the platform cannot be recognised from inside
 * the kernel either. The subsystems whose presence is visible here, the
 * RKP/KDP hypervisor layer and DEFEX, are equally present on vendor
 * kernels where it is the reboot kprobe that installs and the plain write
 * to sys_call_table that is refused: the exact opposite pairing, so their
 * presence separates nothing.
 *
 * So this is a statement the build makes about the kernel it is being
 * built for: CONFIG_KSU_KPROBE_TEXT_PATCH_UNSAFE=y where arming one of
 * these has been found to fault, and nothing otherwise. A kernel that says
 * nothing is treated as one where these patches work, which is the
 * assumption a module built for many kernels at once has to ship with; a
 * kernel that turns out to need the opposite is given its own build with
 * the flag set.
 *
 * It answers "is this a kernel where syscall_regfunc and reboot's own
 * kprobe fault," not "will an arbitrary new kprobe target fault" -- treat a
 * newly-considered kprobe target on a kernel where this returns true as
 * unverified until tested, not as automatically unsafe.
 */
bool ksu_kprobe_text_patch_unsafe(void);

#endif
