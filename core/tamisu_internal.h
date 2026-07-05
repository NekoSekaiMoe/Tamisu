#ifndef __TAMISU_H_SUPERCALL_INTERNAL
#define __TAMISU_H_SUPERCALL_INTERNAL

#include <linux/types.h>
#include <linux/uaccess.h>

#include "tamisu_supercall.h"

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
bool trusted_caller(void);
bool always_allow(void);
bool injected_app(void);
long tamisu_supercall_handle_ioctl(unsigned int cmd, void __user *argp);
void tamisu_supercall_dump_commands(void);

typedef int (*tamisu_ioctl_handler_t)(void __user *arg);
typedef bool (*tamisu_perm_check_t)(void);

struct tamisu_ioctl_cmd_map {
	unsigned int cmd;
	const char *name;
	tamisu_ioctl_handler_t handler;
	tamisu_perm_check_t perm_check;
};

#endif // #ifndef __TAMISU_H_SUPERCALL_INTERNAL
