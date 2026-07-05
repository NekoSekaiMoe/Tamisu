#ifndef __TAMISU_H_SETUID_HOOK
#define __TAMISU_H_SETUID_HOOK

#include <linux/init.h>
#include <linux/types.h>

void tamisu_setuid_hook_init(void);
void tamisu_setuid_hook_exit(void);

int tamisu_handle_setresuid(uid_t old_uid, uid_t new_uid);

#endif // #ifndef __TAMISU_H_SETUID_HOOK
