#ifndef __KSU_H_KSU
#define __KSU_H_KSU

#include <linux/cred.h>
#include <linux/types.h>
#include <linux/version.h>

// Build timestamp as a string literal (e.g. "20260624185506"), supplied by
// Kbuild. Falls back to a constant for out-of-tree builds.
#ifndef KSU_VERSION_STR
#define KSU_VERSION_STR "12000"
#endif // #ifndef KSU_VERSION_STR

// Numeric version code stored in the __u32 field of KSU_GET_INFO.
// SELinux/KMI version semantics, not a build timestamp — the full timestamp
// is delivered via the GET_FULL_VERSION ioctl as a string.
#define KERNEL_SU_VERSION 12000u

#define EVENT_POST_FS_DATA 1
#define EVENT_BOOT_COMPLETED 2
#define EVENT_MODULE_MOUNTED 3

extern struct cred *ksu_cred;
extern bool ksu_late_loaded;
extern bool ksu_no_custom_rc;
extern struct selinux_policy *backup_sepolicy;

#endif // #ifndef __KSU_H_KSU
