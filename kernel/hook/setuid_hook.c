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
typedef const struct cred tamisu_cred_arg_t;
#else
typedef struct cred tamisu_cred_arg_t;
#endif

typedef int (*task_fix_setuid_fn)(tamisu_cred_arg_t *new, const struct cred *old,
				  int flags);

static int __nocfi my_task_fix_setuid(tamisu_cred_arg_t *new,
				      const struct cred *old, int flags);

static struct tamisu_lsm_hook setuid_lsm_hook = TAMISU_LSM_HOOK_INIT(
    task_fix_setuid, "apparmor_task_setrlimit", my_task_fix_setuid, 0);

static int __nocfi my_task_fix_setuid(tamisu_cred_arg_t *new,
				      const struct cred *old, int flags)
{
	uid_t old_uid = old->uid.val;
	uid_t new_uid = new->uid.val;
	int ret;

	/* Preserve the original LSM security decision; never override it. */
	ret = ((task_fix_setuid_fn)setuid_lsm_hook.original)(new, old, flags);
	if (ret)
		return ret;

	/* Feed the orchestrator when UID changes to app range */
	if (old_uid != new_uid) {
		tamisu_zygote_orch_on_uid_change(old_uid, new_uid);
	}

	return 0;
}

void tamisu_setuid_hook_init(void)
{
	int ret = tamisu_register_lsm_hook(&setuid_lsm_hook);

	if (ret)
		pr_err("setuid_hook: failed to register LSM hook: %d\n", ret);
	else
		pr_info("setuid_hook: task_fix_setuid LSM hook registered\n");
}

void tamisu_setuid_hook_exit(void)
{
	tamisu_unregister_lsm_hook(&setuid_lsm_hook);
	pr_info("setuid_hook: LSM hook unregistered\n");
}
