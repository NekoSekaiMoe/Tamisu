#ifndef __KSU_H_SUPERCALL
#define __KSU_H_SUPERCALL

#include "uapi/supercall.h"

int ksu_install_fd(void);
void ksu_supercalls_init(void);
void ksu_supercalls_exit(void);

#endif // #ifndef __KSU_H_SUPERCALL
