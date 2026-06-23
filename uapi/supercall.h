#ifndef __KSU_UAPI_SUPERCALL_H
#define __KSU_UAPI_SUPERCALL_H

#ifdef __cplusplus
extern "C" {
#endif // #ifdef __cplusplus

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
// __u8/__u16/__u32/__u64/__s32 are provided by <sys/ioctl.h>
// (-> linux/ioctl.h -> asm/types.h) on Linux/Android.
#ifndef __aligned_u64
#define __aligned_u64 __u64 __attribute__((aligned(8)))
#endif // #ifndef __aligned_u64
#endif // #ifdef __KERNEL__

#include "uapi/selinux.h"

// Magic numbers for reboot hook (fd install handshake)
#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define KSU_INSTALL_MAGIC2 0xCAFEBABE

// prctl command structures
struct ksu_prctl_get_fd_cmd {
  int result;
  int fd;
};

#define EVENT_POST_FS_DATA 1
#define EVENT_BOOT_COMPLETED 2
#define EVENT_MODULE_MOUNTED 3

/*
 * UAPI contract version. Queried via the dedicated KSU_IOCTL_GET_UAPI_VERSION
 * ioctl (NOT folded into ksu_get_info_cmd) so that GET_INFO stays byte- and
 * number-stable across versions.
 */
#define KERNEL_SU_UAPI_VERSION 2

#define KSU_GET_INFO_FLAG_LKM (1U << 0)
#define KSU_GET_INFO_FLAG_MANAGER (1U << 1)
#define KSU_GET_INFO_FLAG_LATE_LOAD (1U << 2)
#define KSU_GET_INFO_FLAG_PR_BUILD (1U << 3) // reserved

struct ksu_get_info_cmd {
  __u32 version;
  __u32 flags;    // Output: KSU_GET_INFO_FLAG_* bits
  __u32 features; // Output: max feature ID supported
};

struct ksu_report_event_cmd {
  __u32 event;
};

struct ksu_set_sepolicy_cmd {
  __u64 data_len;     /* Input: bytes of serialized command payload */
  __aligned_u64 data; /* Input: pointer to serialized payload */
};

struct ksu_sepolicy_cmd_hdr {
  __u32 cmd;    /* Input: command type, KSU_SEPOLICY_CMD_* */
  __u32 subcmd; /* Input: command subtype */
};

struct ksu_check_safemode_cmd {
  __u8 in_safe_mode;
};

struct ksu_get_feature_cmd {
  __u32 feature_id;
  __u64 value;
  __u8 supported;
};

struct ksu_set_feature_cmd {
  __u32 feature_id;
  __u64 value;
};

struct ksu_get_wrapper_fd_cmd {
  __u32 fd;
  __u32 flags;
};

struct ksu_manage_mark_cmd {
  __u32 operation;
  __s32 pid;
  __u32 result;
};

#define KSU_MARK_GET 1
#define KSU_MARK_MARK 2
#define KSU_MARK_UNMARK 3
#define KSU_MARK_REFRESH 4

#ifndef KSU_FULL_VERSION_STRING
#define KSU_FULL_VERSION_STRING 255
#endif // #ifndef KSU_FULL_VERSION_STRING

struct ksu_get_full_version_cmd {
  char version_full[KSU_FULL_VERSION_STRING];
};

struct ksu_hook_type_cmd {
  char hook_type[32];
};

// IOCTL definitions (zygisk-only build: su/profile/manager ioctls removed)
#define KSU_IOCTL_GET_INFO _IOC(_IOC_READ, 'K', 2, 0)
#define KSU_IOCTL_REPORT_EVENT _IOC(_IOC_WRITE, 'K', 3, 0)
#define KSU_IOCTL_SET_SEPOLICY _IOC(_IOC_READ | _IOC_WRITE, 'K', 4, 0)
#define KSU_IOCTL_CHECK_SAFEMODE _IOC(_IOC_READ, 'K', 5, 0)
#define KSU_IOCTL_GET_FEATURE _IOC(_IOC_READ | _IOC_WRITE, 'K', 13, 0)
#define KSU_IOCTL_SET_FEATURE _IOC(_IOC_WRITE, 'K', 14, 0)
#define KSU_IOCTL_GET_WRAPPER_FD _IOC(_IOC_WRITE, 'K', 15, 0)
#define KSU_IOCTL_MANAGE_MARK _IOC(_IOC_READ | _IOC_WRITE, 'K', 16, 0)
#define KSU_IOCTL_SET_INIT_PGRP _IO('K', 19)
#define KSU_IOCTL_GET_UAPI_VERSION _IOR('K', 22, __u32)
#define KSU_IOCTL_GET_FULL_VERSION _IOC(_IOC_READ, 'K', 100, 0)
#define KSU_IOCTL_HOOK_TYPE _IOC(_IOC_READ, 'K', 101, 0)

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // #ifndef __KSU_UAPI_SUPERCALL_H
