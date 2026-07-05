/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel-side orchestrator: per-app process lifecycle state
 * machine.
 *
 * Author: Anatdx
 */
#ifndef __KSU_H_ZYGOTE_ORCH
#define __KSU_H_ZYGOTE_ORCH

#include <linux/types.h>

void ksu_zygote_orch_init(void);
void ksu_zygote_orch_exit(void);

/* Fed by the LSM hook to learn a tracked child's identity via kernel detection.
 */
void ksu_zygote_orch_on_uid_change(uid_t old_uid, uid_t new_uid);

/* Fed by userspace via netlink when zygisk core reports specialization. */
void ksu_zygote_orch_on_userspace_report(pid_t pid, uid_t uid);

#endif // #ifndef __KSU_H_ZYGOTE_ORCH
