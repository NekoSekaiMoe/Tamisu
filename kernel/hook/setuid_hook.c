#include <linux/cred.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/uidgid.h>
#include <linux/version.h>

#include "feature/zygote_orch.h"
#include "hook/lsm_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "hook/setuid_hook.h"

/*
 * LSM hook: task_fix_setuid
 * Called during credential commit when UID changes.
 * Captures ALL UID change methods: setuid/setreuid/setresuid/setfsuid.
 *
 * This replaces the old __NR_setresuid syscall hook with a more comprehensive
 * LSM-based approach that covers all UID transition paths.
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
typedef const struct cred ksu_cred_arg_t;
#else
typedef struct cred ksu_cred_arg_t;
#endif

static int __nocfi my_task_fix_setuid(ksu_cred_arg_t *new,
				      const struct cred *old, int flags)
{
	uid_t old_uid = old->uid.val;
	uid_t new_uid = new->uid.val;

	/* Feed the orchestrator when UID changes to app range */
	if (old_uid != new_uid) {
		ksu_zygote_orch_on_uid_change(old_uid, new_uid);
	}

	return 0;
}

static struct ksu_lsm_hook setuid_lsm_hook = KSU_LSM_HOOK_INIT(
	task_fix_setuid, "apparmor_task_setrlimit", my_task_fix_setuid, 0);

void ksu_setuid_hook_init(void)
{
	int ret = ksu_register_lsm_hook(&setuid_lsm_hook);

	if (ret)
		pr_err("setuid_hook: failed to register LSM hook: %d\n", ret);
	else
		pr_info("setuid_hook: task_fix_setuid LSM hook registered\n");
}

void ksu_setuid_hook_exit(void)
{
	ksu_unregister_lsm_hook(&setuid_lsm_hook);
	pr_info("setuid_hook: LSM hook unregistered\n");
}
