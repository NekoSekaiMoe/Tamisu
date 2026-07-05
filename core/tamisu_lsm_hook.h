#ifndef __TAMISU_H_LSM_HOOK
#define __TAMISU_H_LSM_HOOK

#include <linux/lsm_hooks.h>
#include <linux/stddef.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define TAMISU_LSM_HOOK_HEADS_TYPE struct lsm_static_calls_table
#else
#define TAMISU_LSM_HOOK_HEADS_TYPE struct security_hook_heads
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...

struct tamisu_lsm_hook {
	const char *head_name;
	const char *target_name;
	size_t head_offset;
	size_t hook_offset;
	void *replacement;
	void *original;
	struct security_hook_list *entry;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	struct lsm_static_call *scall;
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	/*
	 * Offset from target_name to the real hook target. This is used when a
	 * nearby hook has a resolvable target but the desired hook does not.
	 */
	int offset;
};

// clang-format off
#define TAMISU_LSM_HOOK_INIT(member, target_symbol, replacement_fn, off)          \
	{                                                                      \
	    .head_name = #member,                                              \
	    .target_name = target_symbol,                                      \
	    .head_offset = offsetof(TAMISU_LSM_HOOK_HEADS_TYPE, member),          \
	    .hook_offset = offsetof(struct security_hook_list, hook.member),   \
	    .replacement = (void *)(replacement_fn),                           \
	    .offset = off,                                                     \
	}
// clang-format on

int tamisu_lsm_hook(struct tamisu_lsm_hook *hook);
void tamisu_lsm_unhook(struct tamisu_lsm_hook *hook);

int tamisu_register_lsm_hook(struct tamisu_lsm_hook *hook);
void tamisu_unregister_lsm_hook(struct tamisu_lsm_hook *hook);

void tamisu_lsm_hook_init(void);
void tamisu_lsm_hook_exit(void);

#endif // #ifndef __TAMISU_H_LSM_HOOK
