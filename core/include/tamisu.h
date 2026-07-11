#ifndef __TAMISU_H_MAIN
#define __TAMISU_H_MAIN

#include <linux/cred.h>
#include <linux/types.h>
#include <linux/version.h>

/*
 * Outbound calls through kallsyms-resolved function pointers must not be
 * instrumented by CFI/KCFI: the type hash of a cast pointer never matches the
 * kernel callee. Match YukiZygisk's YZ_INDIRECT_CALL pattern.
 */
#if defined(__clang__)
#if __clang_major__ >= 17
#define TAMISU_NOCFI __attribute__((no_sanitize("cfi", "kcfi")))
#else
#define TAMISU_NOCFI __attribute__((no_sanitize("cfi")))
#endif // #if __clang_major__ >= 17
#else
#define TAMISU_NOCFI
#endif // #if defined(__clang__)
#define TAMISU_INDIRECT_CALL __attribute__((__noinline__)) TAMISU_NOCFI

// Build timestamp as a string literal (e.g. "20260624185506"), supplied by
// Kbuild. Falls back to a constant for out-of-tree builds.
#ifndef TAMISU_VERSION_STR
#define TAMISU_VERSION_STR "12000"
#endif // #ifndef TAMISU_VERSION_STR

// Numeric version code stored in the __u32 field of TAMISU_GET_INFO.
// SELinux/KMI version semantics, not a build timestamp — the full timestamp
// is delivered via the GET_FULL_VERSION ioctl as a string.
#define KERNEL_SU_VERSION 12000u

#define EVENT_POST_FS_DATA 1
#define EVENT_BOOT_COMPLETED 2
#define EVENT_MODULE_MOUNTED 3

extern struct cred *tamisu_cred;
extern bool tamisu_late_loaded;
extern bool tamisu_no_custom_rc;
extern struct selinux_policy *backup_sepolicy;

#endif // #ifndef __TAMISU_H_MAIN
