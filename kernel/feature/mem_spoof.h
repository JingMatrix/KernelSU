#ifndef __KSU_H_MEM_SPOOF
#define __KSU_H_MEM_SPOOF

#include <linux/types.h>

extern unsigned long spoof_total_ram_pages;

int ksu_set_spoof_mem(u64 total_ram_bytes, u64 cma_total_bytes);

#endif
