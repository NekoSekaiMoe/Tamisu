#ifndef __TAMISU_H_DAEMON
#define __TAMISU_H_DAEMON

#include <linux/compat.h>
#include <linux/types.h>

struct pt_regs;

#define TAMISU_DAEMON_PATH "/data/tamisu_daemon"

void tamisu_tamisu_daemon_init(void);
void tamisu_tamisu_daemon_exit(void);

bool tamisu_is_safe_mode(void);
void tamisu_stop_input_hook_runtime(void);

extern u32 tamisu_file_sid;

#define MAX_ARG_STRINGS 0x7FFFFFFF
struct user_arg_ptr {
#ifdef CONFIG_COMPAT
	bool is_compat;
#endif // #ifdef CONFIG_COMPAT
	union {
		const char __user *const __user *native;
#ifdef CONFIG_COMPAT
		const compat_uptr_t __user *compat;
#endif // #ifdef CONFIG_COMPAT
	} ptr;
};

void tamisu_handle_execveat_tamisu_daemon(const char *filename,
					  struct user_arg_ptr *argv,
					  struct user_arg_ptr *envp);
void tamisu_execve_hook_tamisu_daemon(const struct pt_regs *regs);

#endif // #ifndef __TAMISU_H_DAEMON
