#ifndef __KSU_H_KSU
#define __KSU_H_KSU

#include <linux/cred.h>
#include <linux/types.h>
#include <linux/version.h>

// Fallback KSU_VERSION if not defined by Kbuild (e.g. when building as LKM)
#ifndef KSU_VERSION
#define KSU_VERSION 12000
#endif // #ifndef KSU_VERSION

#define KERNEL_SU_VERSION KSU_VERSION

#define EVENT_POST_FS_DATA 1
#define EVENT_BOOT_COMPLETED 2
#define EVENT_MODULE_MOUNTED 3

extern struct cred *ksu_cred;
extern bool ksu_late_loaded;
extern bool ksu_no_custom_rc;
extern struct selinux_policy *backup_sepolicy;

#endif // #ifndef __KSU_H_KSU
