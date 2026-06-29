#ifndef __KSU_H_SUPERCALL_INTERNAL
#define __KSU_H_SUPERCALL_INTERNAL

#include <linux/types.h>
#include <linux/uaccess.h>

#include "supercall/supercall.h"

/*
 * UID helpers used by dispatch.c umount/unmap paths. Pure inline so they
 * stay header-only; no policy/allowlist backing needed.
 */
static inline bool is_appuid(uid_t uid)
{
	uid_t appid = uid % 100000; /* PER_USER_RANGE */
	return appid >= 10000 && appid <= 19999;
}

/*
 * Revert module mounts on an app task. Tamisu's current build does NOT
 * ship the per-app kernel_umount path (the denylist umount is being moved
 * to a ReZygisk-style userspace delegation in zygiskd). The handler is
 * kept as a stub so dispatch.c compiles; callers get -ENOSYS.
 *
 * The eventual userspace implementation lives in zygiskd: it fork+setns
 * into the target's mount namespace, parses /proc/self/mountinfo, and
 * detaches only the mounts owned by the active root solution (KSU /
 * Magisk / APatch), avoiding cross-solution interference. See ReZygisk's
 * zygiskd/src/utils.c::umount_root for the reference design.
 */
struct task_struct;
int ksu_umount_task_modules(struct task_struct *task);

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
