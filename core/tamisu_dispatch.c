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
#include <linux/mm.h>
#include <linux/ptrace.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "arch.h"
#include "tamisu_feature.h"
#include "tamisu_file_wrapper.h"
#include "klog.h" // IWYU pragma: keep
#include "tamisu.h"
#include "tamisu_daemon_boot.h"
#include "tamisu_daemon.h"
#include "tamisu_selinux.h"
#include "tamisu_supercall.h"
#include "tamisu_internal.h"
#include "tamisu_zygote_ctl.h"
#include "tamisu_zygote_nl.h"
#include "tamisu_zygote_probe.h"
#include "uapi/zygisk.h"
#include "tamisu_syscall_hook_manager.h"
#include "tamisu_tp_marker.h"

static int do_get_info(void __user *arg)
{
	struct tamisu_get_info_cmd cmd = {.version = KERNEL_SU_VERSION,
					  .flags = 0};

	cmd.flags |= TAMISU_GET_INFO_FLAG_LKM;
	if (tamisu_late_loaded) {
		cmd.flags |= TAMISU_GET_INFO_FLAG_LATE_LOAD;
	}
	cmd.features = TAMISU_FEATURE_MAX;

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_report_event(void __user *arg)
{
	struct tamisu_report_event_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	switch (cmd.event) {
	case EVENT_POST_FS_DATA: {
		static bool post_fs_data_lock = false;
		if (!post_fs_data_lock) {
			post_fs_data_lock = true;
			if (tamisu_late_loaded) {
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
			if (tamisu_late_loaded) {
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
	struct tamisu_set_sepolicy_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	return handle_sepolicy((void __user *)cmd.data, cmd.data_len);
}

static int do_check_safemode(void __user *arg)
{
	struct tamisu_check_safemode_cmd cmd;

	cmd.in_safe_mode = tamisu_is_safe_mode();

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
	struct tamisu_get_feature_cmd cmd;
	bool supported;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_feature: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = tamisu_get_feature(cmd.feature_id, &cmd.value, &supported);
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
	struct tamisu_set_feature_cmd cmd;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("set_feature: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = tamisu_set_feature(cmd.feature_id, cmd.value);
	if (ret) {
		pr_err("set_feature: failed for feature %u: %d\n",
		       cmd.feature_id, ret);
		return ret;
	}

	return 0;
}

static int do_get_wrapper_fd(void __user *arg)
{
	if (!tamisu_file_sid) {
		return -EINVAL;
	}

	struct tamisu_get_wrapper_fd_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_wrapper_fd: copy_from_user failed\n");
		return -EFAULT;
	}

	return tamisu_install_file_wrapper(cmd.fd);
}

static int do_manage_mark(void __user *arg)
{
	struct tamisu_manage_mark_cmd cmd;
	int ret = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("manage_mark: copy_from_user failed\n");
		return -EFAULT;
	}

	switch (cmd.operation) {
	case TAMISU_MARK_GET: {
		// Get task mark status
		ret = tamisu_get_task_mark(cmd.pid);
		if (ret < 0) {
			pr_err("manage_mark: get failed for pid %d: %d\n",
			       cmd.pid, ret);
			return ret;
		}
		cmd.result = (u32)ret;
		break;
	}
	case TAMISU_MARK_MARK: {
		if (cmd.pid == 0) {
			tamisu_mark_all_process();
		} else {
			ret = tamisu_set_task_mark(cmd.pid, true);
			if (ret < 0) {
				pr_err("manage_mark: set_mark failed for pid "
				       "%d: %d\n",
				       cmd.pid, ret);
				return ret;
			}
		}
		break;
	}
	case TAMISU_MARK_UNMARK: {
		if (cmd.pid == 0) {
			tamisu_unmark_all_process();
		} else {
			ret = tamisu_set_task_mark(cmd.pid, false);
			if (ret < 0) {
				pr_err("manage_mark: set_unmark failed for pid "
				       "%d: %d\n",
				       cmd.pid, ret);
				return ret;
			}
		}
		break;
	}
	case TAMISU_MARK_REFRESH: {
		tamisu_mark_running_process();
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
	struct tamisu_get_full_version_cmd cmd = {0};

	strscpy(cmd.version_full, TAMISU_VERSION_STR, sizeof(cmd.version_full));

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_full_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

// 101. HOOK_TYPE - Get hook type
static int do_get_hook_type(void __user *arg)
{
	struct tamisu_hook_type_cmd cmd = {0};
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
	__u32 v = TAMISU_UAPI_VERSION;

	if (copy_to_user(arg, &v, sizeof(v))) {
		pr_err("get_uapi_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_yz_handoff(void __user *arg)
{
	return tamisu_zygote_ctl_handoff(arg);
}

static int do_yz_set_dlopen(void __user *arg)
{
	struct yz_dlopen_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	tamisu_zygote_probe_set_dlopen_off(cmd.dlopen_offset, cmd.dlsym_offset);
	return 0;
}

static int do_yz_reload(void __user *arg)
{
	(void)arg;
	tamisu_zygote_nl_emit_reload();
	return 0;
}

static int do_yz_set_yukilinker(void __user *arg)
{
	struct yz_yukilinker_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	tamisu_zygote_probe_set_yukilinker(cmd.enabled != 0);
	return 0;
}

static int do_yz_set_native_targets(void __user *arg)
{
	struct yz_native_targets_cmd *cmd;
	int ret;

	cmd = memdup_user(arg, sizeof(*cmd));
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);
	ret = tamisu_zygote_probe_set_native_targets(cmd);
	kfree(cmd);
	return ret;
}

static int do_yz_restore_native_load_policy(void __user *arg)
{
	struct yz_native_load_policy_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	return tamisu_zygote_probe_restore_native_policy((pid_t)cmd.pid);
}

struct yz_unmap_tw {
	struct callback_head cb;
	unsigned long addr[YZ_MAX_UNMAP_SEGS];
	unsigned long size[YZ_MAX_UNMAP_SEGS];
	unsigned int n;
	unsigned int retry;
};

/* task_work unmap; retry while PC is inside the target range. */
static void yz_unmap_tw_func(struct callback_head *cb)
{
	struct yz_unmap_tw *tw = container_of(cb, struct yz_unmap_tw, cb);
	struct pt_regs *regs = task_pt_regs(current);
	unsigned long pc = regs ? instruction_pointer(regs) : 0;
	unsigned int i;

	for (i = 0; i < tw->n; i++) {
		if (pc >= tw->addr[i] && pc < tw->addr[i] + tw->size[i]) {
			if (++tw->retry < 16) {
				init_task_work(&tw->cb, yz_unmap_tw_func);
				if (!task_work_add(current, &tw->cb,
						   TWA_RESUME))
					return; /* re-queued; keep tw */
			}
			pr_warn("yz_unmap: pc=0x%lx still in core after %u "
				"tries, skip pid=%d\n",
				pc, tw->retry, current->pid);
			kfree(tw);
			return;
		}
	}

	for (i = 0; i < tw->n; i++) {
		pr_info("yz_unmap: munmap [0x%lx +0x%lx] pid=%d\n", tw->addr[i],
			tw->size[i], current->pid);
		vm_munmap(tw->addr[i], tw->size[i]);
	}
	kfree(tw);
}

/* Schedule vm_munmap of reported segments. */
static int do_yz_unmap_pid(void __user *arg)
{
	struct yz_unmap_pid_cmd cmd;
	struct task_struct *task;
	struct yz_unmap_tw *tw;
	unsigned int i;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.n_segs == 0 || cmd.n_segs > YZ_MAX_UNMAP_SEGS)
		return -EINVAL;

	rcu_read_lock();
	task = get_pid_task(find_vpid(cmd.pid), PIDTYPE_PID);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;

	if (!is_appuid(task_uid(task).val)) {
		pr_info("yz_unmap_pid: reject non-app pid=%u uid=%u\n", cmd.pid,
			task_uid(task).val);
		put_task_struct(task);
		return -EPERM;
	}

	tw = kzalloc(sizeof(*tw), GFP_KERNEL);
	if (!tw) {
		put_task_struct(task);
		return -ENOMEM;
	}
	init_task_work(&tw->cb, yz_unmap_tw_func);
	tw->n = cmd.n_segs;
	for (i = 0; i < cmd.n_segs; i++) {
		tw->addr[i] = (unsigned long)cmd.addr[i];
		tw->size[i] = (unsigned long)cmd.size[i];
	}
	if (task_work_add(task, &tw->cb, TWA_RESUME)) {
		kfree(tw);
		put_task_struct(task);
		return -ESRCH;
	}
	put_task_struct(task);
	pr_info("yz_unmap_pid: scheduled %u seg(s) for pid=%u\n", cmd.n_segs,
		cmd.pid);
	return 0;
}

/* Current-task unmap. */
static int do_yz_unmap_self(void __user *arg)
{
	struct yz_unmap_self_cmd cmd;
	struct yz_unmap_tw *tw;
	unsigned int i;

	if (!current->mm)
		return -EINVAL;
	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.n_segs == 0 || cmd.n_segs > YZ_MAX_UNMAP_SEGS)
		return -EINVAL;
	/* validate userspace ranges */
	for (i = 0; i < cmd.n_segs; i++) {
		unsigned long a = (unsigned long)cmd.addr[i];
		unsigned long s = (unsigned long)cmd.size[i];

		if (a == 0 || s == 0 || a >= TASK_SIZE || s > TASK_SIZE ||
		    a + s < a || a + s > TASK_SIZE) {
			pr_warn(
			    "yz_unmap_self: bad seg [0x%lx +0x%lx] pid=%d\n", a,
			    s, current->pid);
			return -EINVAL;
		}
	}

	tw = kzalloc(sizeof(*tw), GFP_KERNEL);
	if (!tw)
		return -ENOMEM;
	init_task_work(&tw->cb, yz_unmap_tw_func);
	tw->n = cmd.n_segs;
	for (i = 0; i < cmd.n_segs; i++) {
		tw->addr[i] = (unsigned long)cmd.addr[i];
		tw->size[i] = (unsigned long)cmd.size[i];
	}
	if (task_work_add(current, &tw->cb, TWA_RESUME)) {
		kfree(tw);
		return -ESRCH;
	}
	pr_info("yz_unmap_self: pid=%d armed %u seg(s)\n", current->pid,
		cmd.n_segs);
	return 0;
}

/* COW-patch target text via access_process_vm(FOLL_FORCE|FOLL_WRITE). */
static int do_yz_patch_text(void __user *arg)
{
	struct yz_patch_text_cmd cmd;
	struct task_struct *task;
	int n;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.len == 0 || cmd.len > YZ_PATCH_TEXT_MAX)
		return -EINVAL;
	if (cmd.addr == 0 || cmd.addr >= TASK_SIZE ||
	    cmd.addr + cmd.len < cmd.addr || cmd.addr + cmd.len > TASK_SIZE)
		return -EINVAL;

	rcu_read_lock();
	task = get_pid_task(find_vpid(cmd.pid), PIDTYPE_PID);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;

	/* cmd.pid is checked by zygiskd via SO_PEERCRED. */

	n = access_process_vm(task, (unsigned long)cmd.addr, cmd.bytes, cmd.len,
			      FOLL_FORCE | FOLL_WRITE);
	put_task_struct(task);
	if (n != (int)cmd.len) {
		pr_warn("yz_patch_text: wrote %d/%u @0x%llx pid=%u\n", n,
			cmd.len, cmd.addr, cmd.pid);
		return -EFAULT;
	}
	pr_info("yz_patch_text: %u byte(s) @0x%llx pid=%u\n", cmd.len, cmd.addr,
		cmd.pid);
	return 0;
}

// IOCTL handlers mapping table
static const struct tamisu_ioctl_cmd_map tamisu_ioctl_handlers[] = {
    {.cmd = TAMISU_IOCTL_GET_INFO,
     .name = "GET_INFO",
     .handler = do_get_info,
     .perm_check = always_allow},
    {.cmd = TAMISU_IOCTL_REPORT_EVENT,
     .name = "REPORT_EVENT",
     .handler = do_report_event,
     .perm_check = only_root},
    {.cmd = TAMISU_IOCTL_SET_SEPOLICY,
     .name = "SET_SEPOLICY",
     .handler = do_set_sepolicy,
     .perm_check = only_root},
    {.cmd = TAMISU_IOCTL_CHECK_SAFEMODE,
     .name = "CHECK_SAFEMODE",
     .handler = do_check_safemode,
     .perm_check = always_allow},
    {.cmd = TAMISU_IOCTL_GET_FEATURE,
     .name = "GET_FEATURE",
     .handler = do_get_feature,
     .perm_check = trusted_caller},
    {.cmd = TAMISU_IOCTL_SET_FEATURE,
     .name = "SET_FEATURE",
     .handler = do_set_feature,
     .perm_check = trusted_caller},
    {.cmd = TAMISU_IOCTL_GET_WRAPPER_FD,
     .name = "GET_WRAPPER_FD",
     .handler = do_get_wrapper_fd,
     .perm_check = trusted_caller},
    {.cmd = TAMISU_IOCTL_MANAGE_MARK,
     .name = "MANAGE_MARK",
     .handler = do_manage_mark,
     .perm_check = trusted_caller},
    {.cmd = TAMISU_IOCTL_SET_INIT_PGRP,
     .name = "SET_INIT_PGRP",
     .handler = do_set_init_pgrp,
     .perm_check = only_root},
    {.cmd = TAMISU_IOCTL_GET_UAPI_VERSION,
     .name = "GET_UAPI_VERSION",
     .handler = do_get_uapi_version,
     .perm_check = always_allow},
    {.cmd = TAMISU_IOCTL_GET_FULL_VERSION,
     .name = "GET_FULL_VERSION",
     .handler = do_get_full_version,
     .perm_check = always_allow},
    {.cmd = TAMISU_IOCTL_HOOK_TYPE,
     .name = "GET_HOOK_TYPE",
     .handler = do_get_hook_type,
     .perm_check = trusted_caller},
    {.cmd = TAMISU_IOCTL_YZ_HANDOFF,
     .name = "YZ_HANDOFF",
     .handler = do_yz_handoff,
     .perm_check = only_root},
    {.cmd = TAMISU_IOCTL_YZ_SET_DLOPEN,
     .name = "YZ_SET_DLOPEN",
     .handler = do_yz_set_dlopen,
     .perm_check = only_root},
    {.cmd = TAMISU_IOCTL_YZ_SET_YUKILINKER,
     .name = "YZ_SET_YUKILINKER",
     .handler = do_yz_set_yukilinker,
     .perm_check = only_root},
    {.cmd = TAMISU_IOCTL_YZ_SET_NATIVE_TARGETS,
     .name = "YZ_SET_NATIVE_TARGETS",
     .handler = do_yz_set_native_targets,
     .perm_check = only_root},
    {.cmd = TAMISU_IOCTL_YZ_RESTORE_NATIVE_LOAD_POLICY,
     .name = "YZ_RESTORE_NATIVE_LOAD_POLICY",
     .handler = do_yz_restore_native_load_policy,
     .perm_check = only_root},
    {.cmd = TAMISU_IOCTL_YZ_UNMAP_PID,
     .name = "YZ_UNMAP_PID",
     .handler = do_yz_unmap_pid,
     .perm_check = only_root},
    {.cmd = TAMISU_IOCTL_YZ_UNMAP_SELF,
     .name = "YZ_UNMAP_SELF",
     .handler = do_yz_unmap_self,
     .perm_check = injected_app},
    {.cmd = TAMISU_IOCTL_YZ_PATCH_TEXT,
     .name = "YZ_PATCH_TEXT",
     .handler = do_yz_patch_text,
     .perm_check = only_root},
    {.cmd = TAMISU_IOCTL_YZ_RELOAD,
     .name = "YZ_RELOAD",
     .handler = do_yz_reload,
     .perm_check = trusted_caller},
    {.cmd = 0, .name = NULL, .handler = NULL, .perm_check = NULL} // Sentinel
};

void tamisu_supercall_dump_commands(void)
{
	int i;

	pr_info("Tamisu IOCTL Commands:\n");
	for (i = 0; tamisu_ioctl_handlers[i].handler; i++) {
		pr_info("  %-18s = 0x%08x\n", tamisu_ioctl_handlers[i].name,
			tamisu_ioctl_handlers[i].cmd);
	}
}

long tamisu_supercall_handle_ioctl(unsigned int cmd, void __user *argp)
{
	int i;

#ifdef CONFIG_TAMISU_DEBUG
	pr_info("tamisu ioctl: cmd=0x%x from uid=%d\n", cmd, current_uid().val);
#endif // #ifdef CONFIG_TAMISU_DEBUG

	for (i = 0; tamisu_ioctl_handlers[i].handler; i++) {
		if (cmd == tamisu_ioctl_handlers[i].cmd) {
			if (tamisu_ioctl_handlers[i].perm_check &&
			    !tamisu_ioctl_handlers[i].perm_check()) {
				pr_warn("tamisu ioctl: permission denied for "
					"cmd=0x%x uid=%d\n",
					cmd, current_uid().val);
				return -EPERM;
			}
			return tamisu_ioctl_handlers[i].handler(argp);
		}
	}

	pr_warn("tamisu ioctl: unknown cmd 0x%x uid=%d\n", cmd,
		current_uid().val);
	return -EINVAL;
}
