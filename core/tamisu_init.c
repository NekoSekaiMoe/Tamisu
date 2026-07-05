/*
 * Tamisu Main Entry Point (LKM only)
 *
 * YukiSU supports only loadable kernel module (CONFIG_TAMISU=m).
 * Zygisk-only build: all su/profile/apk/sulog features removed.
 */

#include <linux/export.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/version.h>

// workaround for A12-5.10 kernels with mismatched stack protector toolchain
#if defined(CONFIG_STACKPROTECTOR) &&                                          \
    (defined(CONFIG_ARM64) && !defined(CONFIG_STACKPROTECTOR_PER_TASK))
#include <linux/random.h>
#include <linux/stackprotector.h>
unsigned long __stack_chk_guard __ro_after_init
    __attribute__((visibility("hidden")));

__attribute__((no_stack_protector)) void tamisu_setup_stack_chk_guard(void)
{
	unsigned long canary;

	get_random_bytes(&canary, sizeof(canary));
	canary ^= LINUX_VERSION_CODE;
	canary &= CANARY_MASK;
	__stack_chk_guard = canary;
}

__attribute__((naked)) int __init tamisu_init_early(void)
{
	asm("mov x19, x30;\n"
	    "bl tamisu_setup_stack_chk_guard;\n"
	    "mov x30, x19;\n"
	    "b tamisu_init;\n");
}
#define NEED_OWN_STACKPROTECTOR 1
#else
#define NEED_OWN_STACKPROTECTOR 0
#endif // #if defined(CONFIG_STACKPROTECTOR) &&
       //     (defined(CONFIG_ARM64) &&
       //      !defined(CONFIG_STACKPROTECTOR_PER_TASK))

#include "tamisu_feature.h"
#include "tamisu_zygote_probe.h"
#include "tamisu_zygote_orch.h"
#include "tamisu_zygote_nl.h"
#include "tamisu_zygote_ctl.h"
#include "tamisu_file_wrapper.h"
#include "tamisu_lsm_hook.h"
#include "tamisu_symbol_resolver.h"
#include "klog.h" // IWYU pragma: keep
#include "tamisu.h"
#include "tamisu_daemon_boot.h"
#include "tamisu_daemon.h"
#include "tamisu_selinux.h"
#include "tamisu_supercall.h"

struct cred *tamisu_cred;
bool tamisu_late_loaded;

#ifdef CONFIG_TAMISU_DEBUG
bool allow_shell = true;
#else
bool allow_shell = false;
#endif // #ifdef CONFIG_TAMISU_DEBUG
module_param(allow_shell, bool, 0);

bool tamisu_no_custom_rc = false;
module_param_named(norc, tamisu_no_custom_rc, bool, 0);

#include "tamisu_syscall_hook.h"
#include "tamisu_syscall_hook_manager.h"

static bool tamisu_hooks_started;

static void tamisu_hook_init(void)
{
	int ret = tamisu_syscall_hook_init();

	tamisu_hooks_started = false;
	if (ret) {
		pr_err("tamisu: syscall_hook_init failed: %d\n", ret);
		return;
	}
	tamisu_syscall_hook_manager_init();
	tamisu_hooks_started = true;
}

static void tamisu_hook_exit(void)
{
	if (!tamisu_hooks_started)
		return;

	tamisu_syscall_hook_manager_exit();
	tamisu_hooks_started = false;
}

int __init tamisu_init(void)
{
	pr_info("Tamisu LKM initializing, build: %s\n", TAMISU_VERSION_STR);
	tamisu_late_loaded = (current->pid != 1);

#ifdef CONFIG_TAMISU_DEBUG
	pr_alert(
	    "*************************************************************");
	pr_alert("**\t NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE\t**");
	pr_alert("**\t\t\t\t\t\t\t"
		 "\t\t\t **");
	pr_alert("**\t\t You are running Tamisu in DEBUG mode\t\t  **");
	pr_alert("**\t\t\t\t\t\t\t"
		 "\t\t\t **");
	pr_alert("**\t NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE\t**");
	pr_alert(
	    "*************************************************************");
#endif // #ifdef CONFIG_TAMISU_DEBUG

	if (allow_shell) {
		pr_alert("shell is allowed at init!");
	}

	tamisu_init_symbol_resolver();

	tamisu_cred = prepare_creds();
	if (!tamisu_cred) {
		pr_err("prepare cred failed!\n");
	}

	tamisu_feature_init();
	tamisu_lsm_hook_init();
	tamisu_zygote_probe_init();
	tamisu_zygote_nl_init();
	tamisu_zygote_orch_init();
	tamisu_zygote_ctl_init();

	tamisu_supercalls_init();

	if (tamisu_late_loaded) {
		pr_info("late load mode, skipping kprobe hooks\n");

		apply_tamisu_rules();
		cache_sid();
		setup_tamisu_cred();

		if (!getenforce()) {
			pr_info("Permissive SELinux, enforcing\n");
			setenforce(true);
		}

		tamisu_hook_init();
		tamisu_file_wrapper_init();

		tamisu_boot_completed = true;
	} else {
		tamisu_hook_init();
		tamisu_tamisu_daemon_init();
		tamisu_file_wrapper_init();
	}

#ifndef CONFIG_TAMISU_DEBUG
	kobject_del(&THIS_MODULE->mkobj.kobj);
#endif // #ifndef CONFIG_TAMISU_DEBUG

	pr_info("Tamisu LKM initialized\n");
	return 0;
}

void tamisu_exit(void)
{
	// Phase 1: Stop hooks first to prevent new callbacks
	tamisu_hook_exit();
	tamisu_supercalls_exit();

	if (!tamisu_late_loaded)
		tamisu_tamisu_daemon_exit();

	// Wait for any in-flight RCU readers before releasing data structures.
	synchronize_rcu();

	// Phase 2: Now safe to release data structures
	tamisu_zygote_probe_exit();
	tamisu_zygote_orch_exit();
	tamisu_zygote_nl_exit();
	tamisu_zygote_ctl_exit();
	tamisu_lsm_hook_exit();
	tamisu_feature_exit();

	if (tamisu_cred) {
		put_cred(tamisu_cred);
	}
}

#if NEED_OWN_STACKPROTECTOR
module_init(tamisu_init_early);
#else
module_init(tamisu_init);
#endif // #if NEED_OWN_STACKPROTECTOR
module_exit(tamisu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("dabao1955");
MODULE_DESCRIPTION("Android Tamisu core");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
#else
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
