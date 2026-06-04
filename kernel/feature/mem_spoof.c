#include <asm/page.h>
#include <linux/kprobes.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/sysinfo.h>
#include <linux/mm.h>
#include <linux/ptrace.h>
#include "infra/symbol_resolver.h"
#include "feature/mem_spoof.h"

unsigned long spoof_total_ram_pages = 0;
static unsigned long *totalcma_pages_ptr = NULL;
static unsigned long original_cma_pages = 0;

static struct kretprobe *rp_meminfo = NULL;
static struct kretprobe *rp_mem_available = NULL;
static struct kretprobe *rp_commit_limit = NULL;
static bool kretprobes_registered = false;
static int (*reg_kprobe_fn)(struct kretprobe *rp) = NULL;
static void (*unreg_kprobe_fn)(struct kretprobe *rp) = NULL;

/* Context structure to track the struct sysinfo pointer from entry to return */
struct sysinfo_probe_data {
    struct sysinfo *val;
};

/* Register abstractions for cross-architecture safety (ARM64 vs x86_64) */
static inline unsigned long ksu_get_first_arg(struct pt_regs *regs)
{
#if defined(CONFIG_ARM64) || defined(__aarch64__)
    return regs->regs[0];
#elif defined(CONFIG_X86_64) || defined(__x86_64__)
    return regs->di;
#else
    return 0;
#endif
}

static inline void ksu_set_return_reg(struct pt_regs *regs, unsigned long value)
{
#if defined(CONFIG_ARM64) || defined(__aarch64__)
    regs->regs[0] = value;
#elif defined(CONFIG_X86_64) || defined(__x86_64__)
    regs->ax = value;
#endif
}

static inline unsigned long ksu_get_return_reg(struct pt_regs *regs)
{
#if defined(CONFIG_ARM64) || defined(__aarch64__)
    return regs->regs[0];
#elif defined(CONFIG_X86_64) || defined(__x86_64__)
    return regs->ax;
#else
    return 0;
#endif
}

/* --- si_meminfo Hook Implementation --- */
static int si_meminfo_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct sysinfo_probe_data *data = (struct sysinfo_probe_data *)ri->data;
    data->val = (struct sysinfo *)ksu_get_first_arg(regs);
    return 0;
}

static int si_meminfo_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct sysinfo_probe_data *data = (struct sysinfo_probe_data *)ri->data;
    struct sysinfo *val = data->val;

    if (val && spoof_total_ram_pages > 0) {
        unsigned long real_total = val->totalram;
        if (spoof_total_ram_pages > real_total) {
            unsigned long diff = spoof_total_ram_pages - real_total;
            val->totalram = spoof_total_ram_pages;
            /* Proportionally scale freeram up so free memory ratios look natural */
            val->freeram += diff;
        }
    }
    return 0;
}

/* --- si_mem_available Hook Implementation --- */
static int si_mem_available_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if (spoof_total_ram_pages > 0) {
        unsigned long real_avail = ksu_get_return_reg(regs);
        unsigned long real_total = totalram_pages();
        if (spoof_total_ram_pages > real_total && real_total > 0) {
            unsigned long diff = spoof_total_ram_pages - real_total;
            unsigned long spoof_avail = real_avail + diff;
            ksu_set_return_reg(regs, spoof_avail);
        }
    }
    return 0;
}

/* --- vm_commit_limit Hook Implementation --- */
static int vm_commit_limit_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if (spoof_total_ram_pages > 0) {
        unsigned long real_limit = ksu_get_return_reg(regs);
        unsigned long real_total = totalram_pages();
        if (spoof_total_ram_pages > real_total && real_total > 0) {
            /* Scale commit limit proportionally to RAM scale factor */
            unsigned long spoof_limit = (real_limit * spoof_total_ram_pages) / real_total;
            ksu_set_return_reg(regs, spoof_limit);
        }
    }
    return 0;
}

/* --- Dynamic kretprobe allocation helper (adapted from syscall_hook_manager) --- */
static struct kretprobe *ksu_init_kretprobe_mem(const char *name, kretprobe_handler_t handler,
                                                kretprobe_handler_t entry_handler, size_t data_size)
{
    struct kretprobe *rp;
    unsigned long addr;
    int ret;

    rp = kzalloc(sizeof(struct kretprobe), GFP_KERNEL);
    if (!rp) {
        pr_err("ksu_mem_spoof: failed to allocate kretprobe memory for %s\n", name);
        return NULL;
    }

    addr = find_kernel_symbol_exact(name);
    if (!addr) {
        pr_warn("ksu_mem_spoof: unable to resolve symbol address of '%s'\n", name);
        kfree(rp);
        return NULL;
    }

    rp->kp.addr = (kprobe_opcode_t *)addr;
    rp->handler = handler;
    rp->entry_handler = entry_handler;
    rp->data_size = data_size;
    rp->maxactive = 32;

    ret = reg_kprobe_fn(rp);
    if (ret) {
        pr_err("ksu_mem_spoof: registration of %s hook failed: %d\n", name, ret);
        kfree(rp);
        return NULL;
    }

    pr_info("ksu_mem_spoof: dynamically hooked %s at address %lx\n", name, addr);
    return rp;
}

static void ksu_destroy_kretprobe_mem(struct kretprobe **rp_ptr)
{
    struct kretprobe *rp = *rp_ptr;
    if (!rp)
        return;
    unreg_kprobe_fn(rp);
    synchronize_rcu();
    kfree(rp);
    *rp_ptr = NULL;
}

static int resolve_kretprobe_symbols(void)
{
    if (reg_kprobe_fn && unreg_kprobe_fn)
        return 0;

    reg_kprobe_fn = (void *)find_kernel_symbol_exact("register_kretprobe");
    unreg_kprobe_fn = (void *)find_kernel_symbol_exact("unregister_kretprobe");

    if (!reg_kprobe_fn || !unreg_kprobe_fn) {
        pr_err("ksu_mem_spoof: kretprobe registration APIs not found in running kernel!\n");
        return -ENOSYS;
    }
    return 0;
}

static int register_all_mem_hooks(void)
{
    if (kretprobes_registered)
        return 0;

    rp_meminfo = ksu_init_kretprobe_mem("si_meminfo", si_meminfo_ret_handler, si_meminfo_entry_handler,
                                        sizeof(struct sysinfo_probe_data));

    rp_mem_available = ksu_init_kretprobe_mem("si_mem_available", si_mem_available_ret_handler, NULL, 0);

    rp_commit_limit = ksu_init_kretprobe_mem("vm_commit_limit", vm_commit_limit_ret_handler, NULL, 0);

    if (!rp_meminfo || !rp_mem_available || !rp_commit_limit) {
        pr_err("ksu_mem_spoof: failed to register one or more memory hooks\n");

        /* Clean up any partially registered probes */
        ksu_destroy_kretprobe_mem(&rp_meminfo);
        ksu_destroy_kretprobe_mem(&rp_mem_available);
        ksu_destroy_kretprobe_mem(&rp_commit_limit);
        return -EFAULT;
    }

    kretprobes_registered = true;
    return 0;
}

static void unregister_all_mem_hooks(void)
{
    if (!kretprobes_registered)
        return;

    ksu_destroy_kretprobe_mem(&rp_meminfo);
    ksu_destroy_kretprobe_mem(&rp_mem_available);
    ksu_destroy_kretprobe_mem(&rp_commit_limit);

    kretprobes_registered = false;
}

static void ksu_spoof_cma_pages(u64 cma_total_bytes)
{
    if (!totalcma_pages_ptr) {
        totalcma_pages_ptr = (unsigned long *)find_kernel_symbol_exact("totalcma_pages");
        if (totalcma_pages_ptr) {
            original_cma_pages = *totalcma_pages_ptr;
        }
    }

    if (totalcma_pages_ptr) {
        if (cma_total_bytes > 0) {
            *totalcma_pages_ptr = cma_total_bytes >> PAGE_SHIFT;
            pr_info("ksu_mem_spoof: totalcma_pages spoofed to %lu (%llu bytes)\n", *totalcma_pages_ptr,
                    cma_total_bytes);
        } else {
            *totalcma_pages_ptr = original_cma_pages;
            pr_info("ksu_mem_spoof: totalcma_pages restored to original value %lu\n", original_cma_pages);
        }
    }
}

int ksu_set_spoof_mem(u64 total_ram_bytes, u64 cma_total_bytes)
{
    int ret;

    /* 1. Handle teardown / disable request */
    if (total_ram_bytes == 0) {
        if (kretprobes_registered) {
            ret = resolve_kretprobe_symbols();
            if (ret)
                return ret;

            unregister_all_mem_hooks();
            spoof_total_ram_pages = 0;
            ksu_spoof_cma_pages(0); /* Restore CMA pages */
            pr_info("ksu_mem_spoof: deactivated and hooks unregistered.\n");
        }
        return 0;
    }

    /* 2. Resolve Kretprobe APIs dynamically */
    ret = resolve_kretprobe_symbols();
    if (ret)
        return ret;

    /* 3. Duplicate Check (Check both RAM and CMA to avoid redundant work) */
    unsigned long target_pages = total_ram_bytes >> PAGE_SHIFT;
    unsigned long target_cma_pages = cma_total_bytes >> PAGE_SHIFT;

    if (kretprobes_registered && spoof_total_ram_pages == target_pages && totalcma_pages_ptr &&
        *totalcma_pages_ptr == target_cma_pages) {
        return 0;
    }

    /* 4. Register Hooks on-demand */
    ret = register_all_mem_hooks();
    if (ret)
        return ret;

    /* 5. Set target page value and spoof CMA variable */
    spoof_total_ram_pages = target_pages;
    ksu_spoof_cma_pages(cma_total_bytes);

    pr_info("ksu_mem_spoof: memory spoofed to %lu pages (%llu bytes)\n", spoof_total_ram_pages, total_ram_bytes);

    return 0;
}
