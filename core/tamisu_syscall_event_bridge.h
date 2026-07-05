#ifndef __TAMISU_H_SYSCALL_EVENT_BRIDGE
#define __TAMISU_H_SYSCALL_EVENT_BRIDGE

#include <asm/ptrace.h>

long __nocfi tamisu_hook_execve(int orig_nr, const struct pt_regs *regs);

void tamisu_stop_tamisu_daemon_execve_hook(void);

#endif // #ifndef __TAMISU_H_SYSCALL_EVENT_BRIDGE
