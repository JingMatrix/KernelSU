/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 bmax121. All Rights Reserved.
 */

#ifndef __KSU_PATCH_MEMORY_H
#define __KSU_PATCH_MEMORY_H

#include <linux/types.h>
#include "linux/version.h"

#ifdef __aarch64__
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#include "asm/text-patching.h" // IWYU pragma: keep
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
#include "asm/patching.h" // IWYU pragma: keep
#else
#include "asm/insn.h" // IWYU pragma: keep
#endif
#elif defined(__x86_64__) || defined(__riscv)
#include <asm/ptrace.h>
#else
#error "Unsupported arch"
#endif

#define KSU_PATCH_TEXT_FLUSH_DCACHE 1
#define KSU_PATCH_TEXT_FLUSH_ICACHE 2

unsigned long phys_from_virt(unsigned long addr, int *err);

/*
 * Write `len` bytes of `src` over `dst`, which is kernel memory the kernel's
 * own mappings keep read-only. Returns 0, or an error its callers are
 * expected to handle: each has a route that needs no such write.
 *
 * On arm64 the write runs inside stop_machine(), so every CPU is halted for
 * its duration. Where a vendor hypervisor refuses the write, that is an
 * ordinary error return and the caller takes its other route. Where one
 * TRAPS it, the write never returns: the CPUs stay halted, the hardware
 * watchdog resets the device, and the last printk is the one before the
 * write -- which makes the site look like whatever ran next.
 *
 * Nothing visible from inside the kernel separates a refusal from a trap,
 * and those are not the only two answers a vendor layer gives. Samsung's
 * RKP/KDP hypervisor holds the kernel's static mappings read-only at stage 2
 * below anything EL1 can lift, so on most of its builds this store is lost
 * whichever way it is answered -- while on others the same store lands. So
 * the arm64 implementation does not attempt it on a build whose RKP/KDP
 * symbols are visible (infra/samsung_compat.h), reporting instead the error
 * its callers already handle.
 *
 * CONFIG_KSU_STATIC_PATCH_LANDS=y is how a build says its kernel takes the
 * write, and it matters where a caller has no other route: the platform
 * hook/kprobe_patch_compat.h describes installs its reboot handshake through
 * this write precisely because the BRK route faults there, so declining it
 * would leave that handshake with no channel.
 */
int ksu_patch_text(void *dst, void *src, size_t len, int flags);
void *scan_call_to(void *start, size_t size, void *target);

#endif
