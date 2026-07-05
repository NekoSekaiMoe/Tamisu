/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tamisu Zygisk - kernel <-> zygiskd netlink channel.
 *
 * Author: Anatdx
 */
#ifndef __TAMISU_H_ZYGOTE_NL
#define __TAMISU_H_ZYGOTE_NL

#include <linux/types.h>

void tamisu_zygote_nl_init(void);
void tamisu_zygote_nl_exit(void);
void tamisu_zygote_nl_emit_specialize(u32 pid, u32 appid);
void tamisu_zygote_nl_emit_reload(void);

#endif // #ifndef __TAMISU_H_ZYGOTE_NL
