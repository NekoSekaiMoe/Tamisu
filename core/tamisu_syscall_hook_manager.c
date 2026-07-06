// SPDX-License-Identifier: GPL-2.0-only
/*
 * TSR (Tracepoint Syscall Redirect) hook manager.
 *
 * The sys_enter tracepoint handler only rewrites the syscall number to
 * redirect execution to our dispatcher installed at an unused sys_ni_syscall
 * slot.  All actual hook logic runs in normal process context via the
 * dispatcher, not in the atomic tracepoint context.
 */

#include "linux/printk.h"
#include <asm/syscall.h>
#include <linux/ptrace.h>
#include <linux/tracepoint.h>

#include "arch.h"
#include "tamisu_syscall_event_bridge.h"
#include "tamisu_syscall_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "tamisu_ksyms.h"
#include "tamisu_setuid_hook.h"
#include "tamisu_syscall_hook_manager.h"
#include "tamisu_tp_marker.h"

// ---------------------------------------------------------------
// Tracepoint redirect handler
// ---------------------------------------------------------------

#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
static void tamisu_sys_enter_handler(void *data, struct pt_regs *regs, long id)
{
	struct pt_regs *current_regs;

#ifdef CONFIG_COMPAT
	if (unlikely(is_compat_task()))
		return;
#endif // #ifdef CONFIG_COMPAT
	if (tamisu_dispatcher_nr < 0)
		return;
	if (!tamisu_has_syscall_hook(id))
		return;

	/* Redirect the real task pt_regs, not a tracepoint-local view. */
	current_regs = task_pt_regs(current);

	PT_REGS_ORIG_SYSCALL(current_regs) = id;
	current_regs->syscallno = tamisu_dispatcher_nr;
}
#endif /* CONFIG_HAVE_SYSCALL_TRACEPOINTS */

// ---------------------------------------------------------------
// Init / Exit
// ---------------------------------------------------------------

void tamisu_syscall_hook_manager_init(void)
{
	int ret;
	pr_info("hook_manager: initializing TSR hook manager\n");

	/* Initialize tracepoint marker (kretprobes + process marking) */
	tamisu_tp_marker_init();

	/* Register individual syscall hooks via dispatcher */
	tamisu_register_syscall_hook(__NR_execve, tamisu_hook_execve);
#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
	ret = tamisu_tracepoint_probe_register(
	    "sys_enter", (void *)tamisu_sys_enter_handler, NULL);
	if (ret) {
		pr_err("hook_manager: failed to register sys_enter tracepoint: "
		       "%d\n",
		       ret);
	} else {
		pr_info("hook_manager: sys_enter tracepoint registered\n");
	}
#endif // #ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
	tamisu_setuid_hook_init();
}

void tamisu_syscall_hook_manager_exit(void)
{
	pr_info("hook_manager: cleaning up TSR hook manager\n");

#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS
	tamisu_tracepoint_probe_unregister(
	    "sys_enter", (void *)tamisu_sys_enter_handler, NULL);
	tracepoint_synchronize_unregister();
#endif // #ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS

	tamisu_tp_marker_exit();

	/* Unregister dispatcher routes before restoring the syscall table. */
	tamisu_unregister_syscall_hook(__NR_execve);
	/*
	 * Restore the syscall table while feature handlers are still alive, so
	 * any in-flight syscall hook finishes against valid state.
	 */
	tamisu_syscall_hook_exit();
	tamisu_setuid_hook_exit();
}
