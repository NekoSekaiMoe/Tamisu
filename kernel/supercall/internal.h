#ifndef __KSU_H_SUPERCALL_INTERNAL
#define __KSU_H_SUPERCALL_INTERNAL

#include <linux/types.h>
#include <linux/uaccess.h>

#include "supercall/supercall.h"

/*
 * UID helper used by dispatch.c unmap path. Pure inline so it stays
 * header-only; no policy/allowlist backing needed.
 */
static inline bool is_appuid(uid_t uid)
{
	uid_t appid = uid % 100000; /* PER_USER_RANGE */
	return appid >= 10000 && appid <= 19999;
}

bool only_root(void);
bool manager_or_root(void);
bool always_allow(void);
bool injected_app(void);
long ksu_supercall_handle_ioctl(unsigned int cmd, void __user *argp);
void ksu_supercall_dump_commands(void);

typedef int (*ksu_ioctl_handler_t)(void __user *arg);
typedef bool (*ksu_perm_check_t)(void);

struct ksu_ioctl_cmd_map {
	unsigned int cmd;
	const char *name;
	ksu_ioctl_handler_t handler;
	ksu_perm_check_t perm_check;
};

#endif // #ifndef __KSU_H_SUPERCALL_INTERNAL
