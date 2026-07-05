/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel control plane: zygiskd -> kernel handoff + fd brokering.
 *
 * Author: Anatdx
 */
#ifndef __TAMISU_H_ZYGOTE_CTL
#define __TAMISU_H_ZYGOTE_CTL

#include <linux/types.h>

int tamisu_zygote_ctl_handoff(void __user *arg);
void tamisu_zygote_ctl_release(pid_t pid);
void tamisu_zygote_ctl_init(void);
void tamisu_zygote_ctl_exit(void);

#endif /* __TAMISU_H_ZYGOTE_CTL */
