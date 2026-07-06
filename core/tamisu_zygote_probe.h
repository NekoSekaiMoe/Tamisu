/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tamisu Zygisk - kernel-side zygote detection and AT_ENTRY injection.
 *
 * Author: Anatdx
 */
#ifndef __TAMISU_H_ZYGOTE_PROBE
#define __TAMISU_H_ZYGOTE_PROBE

#include <linux/types.h>

struct yz_native_targets_cmd;
struct yz_safemode_status_cmd;

void tamisu_zygote_probe_init(void);
void tamisu_zygote_probe_exit(void);
void tamisu_zygote_probe_set_dlopen_off(u64 dlopen_off, u64 dlsym_off);
void tamisu_zygote_probe_set_zygisk_linker(bool enabled);
int tamisu_zygote_probe_set_native_targets(
    const struct yz_native_targets_cmd *cmd);
int tamisu_zygote_probe_restore_native_policy(pid_t tgid);
int tamisu_zygote_probe_get_safemode(struct yz_safemode_status_cmd *cmd);

#endif // #ifndef __TAMISU_H_ZYGOTE_PROBE
