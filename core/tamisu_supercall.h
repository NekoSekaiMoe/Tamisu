#ifndef __TAMISU_H_SUPERCALL
#define __TAMISU_H_SUPERCALL

#include "uapi/supercall.h"

int tamisu_install_fd(void);
void tamisu_supercalls_init(void);
void tamisu_supercalls_exit(void);

#endif // #ifndef __TAMISU_H_SUPERCALL
