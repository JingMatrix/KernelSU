#include <linux/types.h>

#include "hook/kprobe_patch_compat.h"

bool ksu_kprobe_text_patch_unsafe(void)
{
#ifdef CONFIG_KSU_KPROBE_TEXT_PATCH_UNSAFE
    return true;
#else
    return false;
#endif
}
