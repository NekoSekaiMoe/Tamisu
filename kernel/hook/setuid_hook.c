#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/uidgid.h>

#include "feature/zygote_orch.h"
#include "klog.h" // IWYU pragma: keep
#include "hook/setuid_hook.h"

/*
 * Called AFTER the original setresuid syscall has succeeded (via TSR
 * dispatcher). old_uid/new_uid are the uid before/after the call.
 *
 * Zygisk-only build: the sole responsibility here is to feed the zygote
 * orchestrator, which learns an app child's identity when it drops to its
 * app uid during specialization. su/profile/umount/manager handling is gone.
 */
int ksu_handle_setresuid(uid_t old_uid, uid_t new_uid)
{
	pr_info("handle_setresuid from %d to %d\n", old_uid, new_uid);

	ksu_zygote_orch_on_setresuid(old_uid, new_uid);

	return 0;
}

void ksu_setuid_hook_init(void)
{
}

void ksu_setuid_hook_exit(void)
{
	pr_info("ksu_setuid_hook_exit\n");
}
