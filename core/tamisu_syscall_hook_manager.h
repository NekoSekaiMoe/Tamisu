#ifndef __TAMISU_H_HOOK_MANAGER
#define __TAMISU_H_HOOK_MANAGER

#include "tamisu_tp_marker.h"

// Hook manager initialization and cleanup
void tamisu_syscall_hook_manager_init(void);
void tamisu_syscall_hook_manager_exit(void);

#endif // #ifndef __TAMISU_H_HOOK_MANAGER
