#ifndef __TAMISU_SYMBOL_RESOLVER_H
#define __TAMISU_SYMBOL_RESOLVER_H

void *tamisu_lookup_symbol(const char *symbol_name);
void *tamisu_resolve_symbol_for_functable_hook(const char *symbol_name);
unsigned long find_kernel_symbol_exact(const char *symbol_name);
void tamisu_init_symbol_resolver(void);

#endif // #ifndef __TAMISU_SYMBOL_RESOLVER_H
