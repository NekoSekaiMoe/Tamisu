/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __TAMISU_TP_MARKER_H
#define __TAMISU_TP_MARKER_H

#include <linux/sched.h>
#include <linux/thread_info.h>
#include <linux/version.h>

static inline void tamisu_set_task_tracepoint_flag(struct task_struct *t)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	set_task_syscall_work(t, SYSCALL_TRACEPOINT);
#else
	set_tsk_thread_flag(t, TIF_SYSCALL_TRACEPOINT);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
}

static inline void tamisu_clear_task_tracepoint_flag(struct task_struct *t)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	clear_task_syscall_work(t, SYSCALL_TRACEPOINT);
#else
	clear_tsk_thread_flag(t, TIF_SYSCALL_TRACEPOINT);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
}

/* Tracepoint marker management */
void tamisu_tp_marker_init(void);
void tamisu_tp_marker_exit(void);

/* Process marking */
void tamisu_mark_all_process(void);
void tamisu_unmark_all_process(void);
void tamisu_mark_running_process(void);

/* Per-task mark operations */
int tamisu_get_task_mark(pid_t pid);
int tamisu_set_task_mark(pid_t pid, bool mark);

/* Clear flag only if no other tracepoint user */
void tamisu_clear_task_tracepoint_flag_if_needed(struct task_struct *t);

#endif /* __TAMISU_TP_MARKER_H */
