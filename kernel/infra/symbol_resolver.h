#ifndef __KSU_SYMBOL_RESOLVER_H
#define __KSU_SYMBOL_RESOLVER_H

/*
 * Kallsyms lookups, for a symbol this module cannot link against.
 *
 * A function that calls through a pointer either of these returns has to be
 * __nocfi. What they hand back is the address of the real function, which
 * carries no kCFI type id for an indirect call's check to match, so on a
 * kernel built with CONFIG_CFI_CLANG the first such call is a hard failure
 * rather than a graceful one -- confirmed on hardware, where the boot reason
 * named the target of the indirect call. The attribute suppresses the check
 * for the annotated function's own indirect calls and nothing else, which is
 * exactly the scope of the problem.
 *
 * It belongs on every function that calls through one of these pointers, not
 * only on whichever one resolved it.
 */
void *ksu_resolve_symbol_for_functable_hook(const char *symbol_name);
unsigned long find_kernel_symbol_exact(const char *symbol_name);
void ksu_init_symbol_resolver();

#endif
