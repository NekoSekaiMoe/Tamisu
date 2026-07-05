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
#include "runtime/tamisu_daemon.h"
#include "selinux/selinux.h"

/*
 * tamisu_daemon execve hook: controlled by a static key so that the common case
 * (hook disabled after boot) has zero overhead.
 */
DEFINE_STATIC_KEY_TRUE(tamisu_daemon_execve_key);

void tamisu_stop_tamisu_daemon_execve_hook(void)
{
	static_branch_disable(&tamisu_daemon_execve_key);
	pr_info("hook_manager: tamisu_daemon execve hook disabled\n");
}

// Unmark init's child that are not zygote, adbd or tamisu_daemon
static void tamisu_handle_init_mark_tracker(const char __user *filename_user)
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
		tamisu_clear_task_tracepoint_flag_if_needed(current);
	}
}

long __nocfi tamisu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
	const char __user **filename_user =
	    (const char __user **)&PT_REGS_PARM1(regs);

	/* tamisu_daemon boot-time tracking (init second_stage, zygote detection).
	 * Gated by a static key that tamisu_stop_tamisu_daemon_execve_hook() disables
	 * once init second_stage has run, so the common case post-boot is
	 * zero-overhead. Persistent zygote re-detection is handled entirely
	 * by the LSM bprm_committed_creds hook in zygote_probe.c — the
	 * syscall path no longer carries a long-lived execve interceptor. */
	if (static_branch_unlikely(&tamisu_daemon_execve_key))
		tamisu_execve_hook_tamisu_daemon(regs);

	if (current->pid != 1 && is_init(current_cred())) {
		tamisu_handle_init_mark_tracker(*filename_user);
		return tamisu_syscall_table[orig_nr](regs);
	}

	return tamisu_syscall_table[orig_nr](regs);
}
