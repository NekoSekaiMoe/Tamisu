#include <linux/kallsyms.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/version.h>

#include "tamisu.h"
#include "tamisu_symbol_resolver.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define USE_KCFI 1
#else
#define USE_KCFI 0
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

#if !USE_KCFI
static const char cfi_suffix[] = ".cfi_jt";
static const size_t cfi_suffix_len = sizeof(cfi_suffix) - 1;
#endif // #if !USE_KCFI

#ifdef TAMISU_KALLSYMS_ON_EACH_SYMBOL_NO_MODULE
typedef int (*kallsyms_on_each_symbol_fn_t)(int (*fn)(void *, const char *,
						      unsigned long),
					    void *data);
#else
typedef int (*kallsyms_on_each_symbol_fn_t)(int (*fn)(void *, const char *,
						      struct module *,
						      unsigned long),
					    void *data);
#endif // #ifdef TAMISU_KALLSYMS_ON_EACH_SYMBOL_NO_MODULE

typedef int (*kallsyms_on_each_match_symbol_fn_t)(int (*fn)(void *,
							    unsigned long),
						  const char *name, void *data);
typedef unsigned long (*kallsyms_lookup_name_fn_t)(const char *name);

static kallsyms_lookup_name_fn_t kallsyms_lookup_name_fn;
static kallsyms_on_each_symbol_fn_t kallsyms_on_each_symbol_fn;
static kallsyms_on_each_match_symbol_fn_t kallsyms_on_each_match_symbol_fn;

struct tamisu_lookup_symbol_ctx {
	const char *symbol_name;
	size_t symbol_len;
	void *match;
};

static int find_kernel_symbol_exact_cb(void *data, unsigned long addr)
{
	*(unsigned long *)data = addr;
	return 0;
}

static TAMISU_INDIRECT_CALL unsigned long
tamisu_bootstrap_kallsyms_lookup_name(void)
{
#ifdef CONFIG_KPROBES
	struct kprobe kp = {.symbol_name = "kallsyms_lookup_name"};
	unsigned long addr;

	if (register_kprobe(&kp))
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
#else
	return 0;
#endif // #ifdef CONFIG_KPROBES
}

TAMISU_INDIRECT_CALL unsigned long
find_kernel_symbol_exact(const char *symbol_name)
{
	unsigned long addr = 0;

	if (!symbol_name || !symbol_name[0])
		return 0;

	if (kallsyms_on_each_match_symbol_fn) {
		kallsyms_on_each_match_symbol_fn(find_kernel_symbol_exact_cb,
						 symbol_name, &addr);
		if (addr)
			return addr;
	}

	if (kallsyms_lookup_name_fn)
		return kallsyms_lookup_name_fn(symbol_name);

	return 0;
}

static inline bool tamisu_symbol_has_suffix(const char *name, size_t name_len,
					    const char *suffix,
					    size_t suffix_len)
{
	return name_len >= suffix_len &&
	       strcmp(name + name_len - suffix_len, suffix) == 0;
}

#ifdef TAMISU_KALLSYMS_ON_EACH_SYMBOL_NO_MODULE
static int lookup_symbol_variant_cb(void *data, const char *name,
				    unsigned long addr)
#else
static int lookup_symbol_variant_cb(void *data, const char *name,
				    struct module *mod, unsigned long addr)
#endif // #ifdef TAMISU_KALLSYMS_ON_EACH_SYMBOL_NO_MODULE
{
	struct tamisu_lookup_symbol_ctx *ctx = data;
	size_t name_len;

	if (!name || !addr)
		return 0;

	name_len = strlen(name);
	if (strcmp(name, ctx->symbol_name) != 0) {
		if (name_len <= ctx->symbol_len ||
		    strncmp(name, ctx->symbol_name, ctx->symbol_len) != 0 ||
		    (name[ctx->symbol_len] != '.' &&
		     name[ctx->symbol_len] != '$'))
			return 0;
	}

#if !USE_KCFI
	if (tamisu_symbol_has_suffix(name, name_len, cfi_suffix,
				     cfi_suffix_len)) {
		ctx->match = (void *)addr;
		pr_info("use .cfi_jt variant: %s\n", name);
		return 1;
	}
#endif // #if !USE_KCFI

	if (!ctx->match) {
		ctx->match = (void *)addr;
		pr_info("found variant: %s\n", name);
#if USE_KCFI
		return 1;
#endif // #if USE_KCFI
	}

	return 0;
}

static TAMISU_INDIRECT_CALL void *
resolve_symbol_variant(const char *symbol_name, size_t symbol_len)
{
	struct tamisu_lookup_symbol_ctx ctx = {
	    .symbol_name = symbol_name,
	    .symbol_len = symbol_len,
	};

	if (kallsyms_on_each_symbol_fn)
		kallsyms_on_each_symbol_fn(lookup_symbol_variant_cb, &ctx);

	return ctx.match;
}

void *tamisu_resolve_symbol_for_functable_hook(const char *symbol_name)
{
	void *addr;
	size_t symbol_len;

	if (!symbol_name || !symbol_name[0])
		return NULL;

	symbol_len = strlen(symbol_name);

#if !USE_KCFI
	{
		char cfi_name[KSYM_NAME_LEN];
		int len;

		len = snprintf(cfi_name, sizeof(cfi_name), "%s.cfi_jt",
			       symbol_name);
		if (len > 0 && (size_t)len < sizeof(cfi_name)) {
			addr = (void *)find_kernel_symbol_exact(cfi_name);
			if (addr)
				return addr;
		}
	}

	addr = resolve_symbol_variant(symbol_name, symbol_len);
	if (addr)
		return addr;

	return (void *)find_kernel_symbol_exact(symbol_name);
#else
	addr = (void *)find_kernel_symbol_exact(symbol_name);
	if (addr)
		return addr;

	return resolve_symbol_variant(symbol_name, symbol_len);
#endif // #if !USE_KCFI
}

void *tamisu_lookup_symbol(const char *symbol_name)
{
	return tamisu_resolve_symbol_for_functable_hook(symbol_name);
}

void __init tamisu_init_symbol_resolver(void)
{
	kallsyms_lookup_name_fn =
	    (kallsyms_lookup_name_fn_t)tamisu_bootstrap_kallsyms_lookup_name();
	if (!kallsyms_lookup_name_fn)
		pr_warn("kallsyms_lookup_name not found\n");

	kallsyms_on_each_symbol_fn =
	    (kallsyms_on_each_symbol_fn_t)find_kernel_symbol_exact(
		"kallsyms_on_each_symbol");
	if (!kallsyms_on_each_symbol_fn)
		pr_warn("kallsyms_on_each_symbol not found\n");

	kallsyms_on_each_match_symbol_fn =
	    (kallsyms_on_each_match_symbol_fn_t)find_kernel_symbol_exact(
		"kallsyms_on_each_match_symbol");
	if (!kallsyms_on_each_match_symbol_fn)
		pr_warn("kallsyms_on_each_match_symbol not found\n");
}
