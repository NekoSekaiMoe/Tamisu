// SPDX-License-Identifier: GPL-2.0-only
#include "linux/compiler.h"
#include "linux/cred.h"
#include "linux/printk.h"
#include <linux/jump_label.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "arch.h"
#include "hook/setuid_hook.h"
#include "hook/syscall_event_bridge.h"
#include "hook/syscall_hook.h"
#include "hook/tp_marker.h"
#include "klog.h" // IWYU pragma: keep
#include "runtime/ksud.h"
#include "selinux/selinux.h"

/*
 * ksud execve hook: controlled by a static key so that the common case
 * (hook disabled after boot) has zero overhead.
 */
DEFINE_STATIC_KEY_TRUE(ksud_execve_key);

void ksu_stop_ksud_execve_hook(void)
{
	static_branch_disable(&ksud_execve_key);
	pr_info("hook_manager: ksud execve hook disabled\n");
}

// Unmark init's child that are not zygote, adbd or ksud
static void ksu_handle_init_mark_tracker(const char __user *filename_user)
{
	char path[64];
	unsigned long addr;
	const char __user *fn;
	long ret;

	if (unlikely(!filename_user))
		return;

	addr = untagged_addr((unsigned long)filename_user);
	fn = (const char __user *)addr;
	memset(path, 0, sizeof(path));
	ret = strncpy_from_user(path, fn, sizeof(path));
	if (ret < 0)
		return;
	path[sizeof(path) - 1] = '\0';

	if (likely(strstr(path, "/app_process") == NULL &&
		  strstr(path, "/adbd") == NULL)) {
		pr_info("hook_manager: unmark %d exec %s\n", current->pid,
			path);
		ksu_clear_task_tracepoint_flag_if_needed(current);
	}
}

long __nocfi ksu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
	const char __user **filename_user =
	    (const char __user **)&PT_REGS_PARM1(regs);

	/* ksud boot-time tracking (init second_stage, zygote detection) */
	if (static_branch_unlikely(&ksud_execve_key))
		ksu_execve_hook_ksud(regs);

	/* kernel-zygisk: persistent zygote detection (every (re)start) */
	ksu_zygote_probe_execve(regs);

	if (current->pid != 1 && is_init(current_cred())) {
		ksu_handle_init_mark_tracker(*filename_user);
		return ksu_syscall_table[orig_nr](regs);
	}

	return ksu_syscall_table[orig_nr](regs);
}

long __nocfi ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs)
{
	long ret;
	uid_t old_uid = current_uid().val;

	/* Call the original syscall first, then inspect the result */
	ret = ksu_syscall_table[orig_nr](regs);
	if (ret < 0)
		return ret;

	ksu_handle_setresuid(old_uid, current_uid().val);

	return ret;
}
