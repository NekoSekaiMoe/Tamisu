#include <asm/unistd.h>
#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "arch.h"
#include "policy/feature.h"
#include "infra/file_wrapper.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "runtime/ksud_boot.h"
#include "runtime/ksud.h"
#include "selinux/selinux.h"
#include "supercall/supercall.h"
#include "supercall/internal.h"
#include "feature/zygote_ctl.h"
#include "feature/zygote_nl.h"
#include "feature/zygote_probe.h"
#include "uapi/yukizygisk.h"
#include "hook/syscall_hook_manager.h"
#include "hook/tp_marker.h"

static int do_get_info(void __user *arg)
{
	struct ksu_get_info_cmd cmd = {.version = KERNEL_SU_VERSION,
				       .flags = 0};

	cmd.flags |= KSU_GET_INFO_FLAG_LKM;
	if (ksu_late_loaded) {
		cmd.flags |= KSU_GET_INFO_FLAG_LATE_LOAD;
	}
	cmd.features = KSU_FEATURE_MAX;

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_report_event(void __user *arg)
{
	struct ksu_report_event_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	switch (cmd.event) {
	case EVENT_POST_FS_DATA: {
		static bool post_fs_data_lock = false;
		if (!post_fs_data_lock) {
			post_fs_data_lock = true;
			if (ksu_late_loaded) {
				pr_info("post-fs-data skipped (late load)\n");
			} else {
				pr_info("post-fs-data triggered\n");
				on_post_fs_data();
			}
		}
		break;
	}
	case EVENT_BOOT_COMPLETED: {
		static bool boot_complete_lock = false;
		if (!boot_complete_lock) {
			boot_complete_lock = true;
			if (ksu_late_loaded) {
				pr_info("boot_complete skipped (late load)\n");
			} else {
				pr_info("boot_complete triggered\n");
				on_boot_completed();
			}
		}
		break;
	}
	case EVENT_MODULE_MOUNTED: {
		pr_info("module mounted!\n");
		on_module_mounted();
		break;
	}
	default:
		break;
	}

	return 0;
}

static int do_set_sepolicy(void __user *arg)
{
	struct ksu_set_sepolicy_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	return handle_sepolicy((void __user *)cmd.data, cmd.data_len);
}

static int do_check_safemode(void __user *arg)
{
	struct ksu_check_safemode_cmd cmd;

	cmd.in_safe_mode = ksu_is_safe_mode();

	if (cmd.in_safe_mode) {
		pr_warn("safemode enabled!\n");
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("check_safemode: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_set_init_pgrp(void __user *arg)
{
	int err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	struct pid *pids[PIDTYPE_MAX] = {0};
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	struct task_struct *p = current->group_leader;
	struct pid *init_group = task_pgrp(&init_task);

	write_lock_irq(&tasklist_lock);
	err = -EPERM;
	if (task_session(p) != task_session(&init_task))
		goto out;

	err = 0;
	if (task_pgrp(p) != init_group) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
		change_pid(pids, p, PIDTYPE_PGID, init_group);
#else
		change_pid(p, PIDTYPE_PGID, init_group);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	}

out:
	write_unlock_irq(&tasklist_lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	free_pids(pids);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	return err;
}

static int do_get_feature(void __user *arg)
{
	struct ksu_get_feature_cmd cmd;
	bool supported;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_feature: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = ksu_get_feature(cmd.feature_id, &cmd.value, &supported);
	cmd.supported = supported ? 1 : 0;

	if (ret && supported) {
		pr_err("get_feature: failed for feature %u: %d\n",
		       cmd.feature_id, ret);
		return ret;
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_feature: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_set_feature(void __user *arg)
{
	struct ksu_set_feature_cmd cmd;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("set_feature: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = ksu_set_feature(cmd.feature_id, cmd.value);
	if (ret) {
		pr_err("set_feature: failed for feature %u: %d\n",
		       cmd.feature_id, ret);
		return ret;
	}

	return 0;
}

static int do_get_wrapper_fd(void __user *arg)
{
	if (!ksu_file_sid) {
		return -EINVAL;
	}

	struct ksu_get_wrapper_fd_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_wrapper_fd: copy_from_user failed\n");
		return -EFAULT;
	}

	return ksu_install_file_wrapper(cmd.fd);
}

static int do_manage_mark(void __user *arg)
{
	struct ksu_manage_mark_cmd cmd;
	int ret = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("manage_mark: copy_from_user failed\n");
		return -EFAULT;
	}

	switch (cmd.operation) {
	case KSU_MARK_GET: {
		// Get task mark status
		ret = ksu_get_task_mark(cmd.pid);
		if (ret < 0) {
			pr_err("manage_mark: get failed for pid %d: %d\n",
			       cmd.pid, ret);
			return ret;
		}
		cmd.result = (u32)ret;
		break;
	}
	case KSU_MARK_MARK: {
		if (cmd.pid == 0) {
			ksu_mark_all_process();
		} else {
			ret = ksu_set_task_mark(cmd.pid, true);
			if (ret < 0) {
				pr_err("manage_mark: set_mark failed for pid "
				       "%d: %d\n",
				       cmd.pid, ret);
				return ret;
			}
		}
		break;
	}
	case KSU_MARK_UNMARK: {
		if (cmd.pid == 0) {
			ksu_unmark_all_process();
		} else {
			ret = ksu_set_task_mark(cmd.pid, false);
			if (ret < 0) {
				pr_err("manage_mark: set_unmark failed for pid "
				       "%d: %d\n",
				       cmd.pid, ret);
				return ret;
			}
		}
		break;
	}
	case KSU_MARK_REFRESH: {
		ksu_mark_running_process();
		pr_info("manage_mark: refreshed running processes\n");
		break;
	}
	default: {
		pr_err("manage_mark: invalid operation %u\n", cmd.operation);
		return -EINVAL;
	}
	}
	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("manage_mark: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

// 100. GET_FULL_VERSION - Get full version string
static int do_get_full_version(void __user *arg)
{
	struct ksu_get_full_version_cmd cmd = {0};

	strscpy(cmd.version_full, KSU_VERSION_FULL, sizeof(cmd.version_full));

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_full_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

// 101. HOOK_TYPE - Get hook type
static int do_get_hook_type(void __user *arg)
{
	struct ksu_hook_type_cmd cmd = {0};
	const char *type = "TSR Hook";

	strscpy(cmd.hook_type, type, sizeof(cmd.hook_type));

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_hook_type: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_uapi_version(void __user *arg)
{
	__u32 v = KERNEL_SU_UAPI_VERSION;

	if (copy_to_user(arg, &v, sizeof(v))) {
		pr_err("get_uapi_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_yz_handoff(void __user *arg)
{
	return ksu_zygote_ctl_handoff(arg);
}

static int do_yz_set_dlopen(void __user *arg)
{
	struct yz_dlopen_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	ksu_zygote_probe_set_dlopen_off(cmd.dlopen_offset, cmd.dlsym_offset);
	return 0;
}

static int do_yz_reload(void __user *arg)
{
	(void)arg;
	ksu_zygote_nl_emit_reload();
	return 0;
}

// IOCTL handlers mapping table
static const struct ksu_ioctl_cmd_map ksu_ioctl_handlers[] = {
    {.cmd = KSU_IOCTL_GET_INFO,
     .name = "GET_INFO",
     .handler = do_get_info,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_REPORT_EVENT,
     .name = "REPORT_EVENT",
     .handler = do_report_event,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_SET_SEPOLICY,
     .name = "SET_SEPOLICY",
     .handler = do_set_sepolicy,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_CHECK_SAFEMODE,
     .name = "CHECK_SAFEMODE",
     .handler = do_check_safemode,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_GET_FEATURE,
     .name = "GET_FEATURE",
     .handler = do_get_feature,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_SET_FEATURE,
     .name = "SET_FEATURE",
     .handler = do_set_feature,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_WRAPPER_FD,
     .name = "GET_WRAPPER_FD",
     .handler = do_get_wrapper_fd,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_MANAGE_MARK,
     .name = "MANAGE_MARK",
     .handler = do_manage_mark,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_SET_INIT_PGRP,
     .name = "SET_INIT_PGRP",
     .handler = do_set_init_pgrp,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_GET_UAPI_VERSION,
     .name = "GET_UAPI_VERSION",
     .handler = do_get_uapi_version,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_GET_FULL_VERSION,
     .name = "GET_FULL_VERSION",
     .handler = do_get_full_version,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_HOOK_TYPE,
     .name = "GET_HOOK_TYPE",
     .handler = do_get_hook_type,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_YZ_HANDOFF,
     .name = "YZ_HANDOFF",
     .handler = do_yz_handoff,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_SET_DLOPEN,
     .name = "YZ_SET_DLOPEN",
     .handler = do_yz_set_dlopen,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_RELOAD,
     .name = "YZ_RELOAD",
     .handler = do_yz_reload,
     .perm_check = manager_or_root},
    {.cmd = 0, .name = NULL, .handler = NULL, .perm_check = NULL} // Sentinel
};

void ksu_supercall_dump_commands(void)
{
	int i;

	pr_info("KernelSU IOCTL Commands:\n");
	for (i = 0; ksu_ioctl_handlers[i].handler; i++) {
		pr_info("  %-18s = 0x%08x\n", ksu_ioctl_handlers[i].name,
			ksu_ioctl_handlers[i].cmd);
	}
}

long ksu_supercall_handle_ioctl(unsigned int cmd, void __user *argp)
{
	int i;

#ifdef CONFIG_KSU_DEBUG
	pr_info("ksu ioctl: cmd=0x%x from uid=%d\n", cmd, current_uid().val);
#endif // #ifdef CONFIG_KSU_DEBUG

	for (i = 0; ksu_ioctl_handlers[i].handler; i++) {
		if (cmd == ksu_ioctl_handlers[i].cmd) {
			if (ksu_ioctl_handlers[i].perm_check &&
			    !ksu_ioctl_handlers[i].perm_check()) {
				pr_warn("ksu ioctl: permission denied for "
					"cmd=0x%x uid=%d\n",
					cmd, current_uid().val);
				return -EPERM;
			}
			return ksu_ioctl_handlers[i].handler(argp);
		}
	}

	pr_warn("ksu ioctl: unknown cmd 0x%x uid=%d\n", cmd, current_uid().val);
	return -EINVAL;
}
