#include <linux/bitops.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/version.h>

#if defined(CONFIG_ARM64) || defined(__aarch64__)
#include <asm/cpu.h>
#include <asm/cpucaps.h>
#include <asm/cpufeature.h>
#include <asm/cputype.h>
#include <linux/clocksource.h>
#include <vdso/datapage.h>
#include <vdso/clocksource.h>
#endif

#include "uapi/supercall.h"
#include "infra/symbol_resolver.h"
#include "feature/cpu_spoof.h"

int ksu_set_spoof_cpu(struct ksu_set_spoof_cpu_cmd *cmd)
{
    /* Boundary Enforcement to prevent out-of-bounds per-CPU page dereferencing */
    if (cmd->cpu_index >= num_possible_cpus()) {
        pr_warn("ksu: set_spoof_cpu out-of-bounds cpu_index %u\n", cmd->cpu_index);
        return -EINVAL;
    }

#if defined(CONFIG_ARM64) || defined(__aarch64__)
    /* 1. Resolve 'cpu_data' and overwrite target core MIDR (executed for each CPU) */
    {
        unsigned long cpu_data_base = find_kernel_symbol_exact("cpu_data");
        if (cpu_data_base) {
            struct cpuinfo_arm64 *info = per_cpu_ptr((struct cpuinfo_arm64 *)cpu_data_base, cmd->cpu_index);
            if (info) {
                info->reg_midr = cmd->midr;
            }
        } else {
            pr_warn("ksu: set_spoof_cpu failed to resolve 'cpu_data'\n");
        }
    }

    /* 2. Global modifications (executed exactly once on CPU 0) */
    if (cmd->cpu_index == 0) {
        /* A. Override global dynamic timing metric (executed for each CPU) */
        pr_info("ksu: set_spoof_cpu current global loops_per_jiffy: %lu\n", loops_per_jiffy);
        loops_per_jiffy = ((unsigned long)cmd->bogomips * 500000) / (100 * HZ);
        pr_info("ksu: set_spoof_cpu updated global loops_per_jiffy: %lu\n", loops_per_jiffy);

        /* B. Globally overwrite hardware capabilities */
        unsigned long *elf_hwcap_bitmap = (unsigned long *)find_kernel_symbol_exact("elf_hwcap");
        if (elf_hwcap_bitmap) {
            pr_info("ksu: set_spoof_cpu current elf_hwcap: 0x%lx, elf_hwcap2: 0x%lx\n", elf_hwcap_bitmap[0],
                    elf_hwcap_bitmap[1]);
            elf_hwcap_bitmap[0] = cmd->hwcap; /* Primary hwcap */
            elf_hwcap_bitmap[1] = cmd->hwcap2; /* Secondary hwcap2 (index 1 of the bitmap) */
            pr_info("ksu: set_spoof_cpu updated elf_hwcap: 0x%lx, elf_hwcap2: 0x%lx\n", elf_hwcap_bitmap[0],
                    elf_hwcap_bitmap[1]);
        } else {
            pr_warn("ksu: set_spoof_cpu failed to resolve 'elf_hwcap'\n");
        }

        /* C. Erratum Reversal and Clock Mode Alignment (Clean Cores Only) */
        {
            unsigned int part = MIDR_PARTNUM(cmd->midr);
            if (part != ARM_CPU_PART_NEOVERSE_N1 && part != ARM_CPU_PART_CORTEX_A76 &&
                part != QCOM_CPU_PART_KRYO_4XX_GOLD) {
                /* Resolve the kernel-side vDSO time data and align clock mode.
                 * v6.15 reshaped this page: struct vdso_data became struct
                 * vdso_time_data carrying struct vdso_clock clock_data[CS_BASES],
                 * so clock_mode sits one level further down, and the kernel-side
                 * pointer was renamed vdso_data -> vdso_k_time_data. Either way the
                 * resolved symbol is a pointer variable, so the cast is a double
                 * pointer. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
#ifdef CONFIG_GENERIC_VDSO_DATA_STORE
                struct vdso_time_data **vdso_ptr =
                    (struct vdso_time_data **)find_kernel_symbol_exact("vdso_k_time_data");
                if (vdso_ptr && *vdso_ptr) {
                    struct vdso_clock *vc = (*vdso_ptr)->clock_data;
                    pr_info("ksu: set_spoof_cpu found 'vdso_k_time_data' (current CS_HRES_COARSE clock_mode: %d)\n",
                            vc[CS_HRES_COARSE].clock_mode);
                    vc[CS_HRES_COARSE].clock_mode = VDSO_CLOCKMODE_ARCHTIMER;
                    vc[CS_RAW].clock_mode = VDSO_CLOCKMODE_ARCHTIMER;
                    pr_info("ksu: set_spoof_cpu updated 'vdso_k_time_data' clock_mode to %d\n",
                            vc[CS_HRES_COARSE].clock_mode);
                } else {
                    pr_warn("ksu: set_spoof_cpu failed to resolve 'vdso_k_time_data'\n");
                }
#else
                pr_warn("ksu: set_spoof_cpu: no generic vDSO data store, clock_mode left alone\n");
#endif
#else
                struct vdso_data **vdso_data_ptr = (struct vdso_data **)find_kernel_symbol_exact("vdso_data");
                if (vdso_data_ptr && *vdso_data_ptr) {
                    struct vdso_data *vdso = *vdso_data_ptr;
                    pr_info("ksu: set_spoof_cpu found 'vdso_data' (current CS_HRES_COARSE clock_mode: %d)\n",
                            vdso[CS_HRES_COARSE].clock_mode);
                    vdso[CS_HRES_COARSE].clock_mode = VDSO_CLOCKMODE_ARCHTIMER;
                    vdso[CS_RAW].clock_mode = VDSO_CLOCKMODE_ARCHTIMER;
                    pr_info("ksu: set_spoof_cpu updated 'vdso_data' clock_mode to %d\n",
                            vdso[CS_HRES_COARSE].clock_mode);
                } else {
                    pr_warn("ksu: set_spoof_cpu failed to resolve 'vdso_data'\n");
                }
#endif

                /* Resolve 'curr_clocksource' double pointer and overwrite active clock mode */
                {
                    struct clocksource **curr_cs_ptr =
                        (struct clocksource **)find_kernel_symbol_exact("curr_clocksource");
                    if (curr_cs_ptr && *curr_cs_ptr) {
                        struct clocksource *cs = *curr_cs_ptr;
                        pr_info("ksu: set_spoof_cpu found active clocksource '%s' (current vdso_clock_mode: %d)\n",
                                cs->name ? cs->name : "unknown", cs->vdso_clock_mode);
                        cs->vdso_clock_mode = VDSO_CLOCKMODE_ARCHTIMER;
                        pr_info("ksu: set_spoof_cpu updated clocksource '%s' vdso_clock_mode to %d\n",
                                cs->name ? cs->name : "unknown", cs->vdso_clock_mode);
                    } else {
                        pr_warn("ksu: set_spoof_cpu failed to resolve 'curr_clocksource'\n");
                    }
                }

                /* Overwrite fallback default clock mode variable */
                {
                    enum vdso_clock_mode *vd_default = (enum vdso_clock_mode *)find_kernel_symbol_exact("vdso_default");
                    if (vd_default) {
                        pr_info("ksu: set_spoof_cpu found 'vdso_default' (current value: %d)\n", *vd_default);
                        *vd_default = VDSO_CLOCKMODE_ARCHTIMER;
                        pr_info("ksu: set_spoof_cpu updated 'vdso_default' to %d\n", *vd_default);
                    } else {
                        pr_warn("ksu: set_spoof_cpu failed to resolve 'vdso_default'\n");
                    }
                }

                /* Resolve and clear capability bitmap (handles cpu_hwcaps / system_cpucaps fallback) */
                {
                    unsigned long *caps = (unsigned long *)find_kernel_symbol_exact("cpu_hwcaps");
                    if (!caps) {
                        caps = (unsigned long *)find_kernel_symbol_exact("system_cpucaps");
                    }
                    if (caps) {
                        pr_info("ksu: set_spoof_cpu current ARM64_WORKAROUND_1418040 capability state: %d\n",
                                test_bit(ARM64_WORKAROUND_1418040, caps));
                        clear_bit(ARM64_WORKAROUND_1418040, caps);
                        pr_info("ksu: set_spoof_cpu updated ARM64_WORKAROUND_1418040 capability state: %d\n",
                                test_bit(ARM64_WORKAROUND_1418040, caps));
                    } else {
                        pr_warn("ksu: set_spoof_cpu failed to resolve 'cpu_hwcaps' or 'system_cpucaps'\n");
                    }
                }
            }
        }
    }
#endif

    pr_info("KernelSU Stealth: CPU %u identity spoofed (MIDR: 0x%08x)\n", cmd->cpu_index, cmd->midr);
    return 0;
}
