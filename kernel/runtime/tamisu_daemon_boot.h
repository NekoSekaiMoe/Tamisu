#ifndef __TAMISU_H_DAEMON_BOOT
#define __TAMISU_H_DAEMON_BOOT

#include <linux/types.h>

void on_post_fs_data(void);
void on_module_mounted(void);
void on_boot_completed(void);

bool tamisu_is_safe_mode(void);

extern bool tamisu_boot_completed;

#endif // #ifndef __TAMISU_H_DAEMON_BOOT
