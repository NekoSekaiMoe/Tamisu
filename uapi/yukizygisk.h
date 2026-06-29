/* SPDX-License-Identifier: GPL-2.0 */
/*
 * YukiZygisk - kernel <-> zygiskd netlink ABI.
 *
 * Author: Anatdx
 */
#ifndef _UAPI_YUKIZYGISK_H
#define _UAPI_YUKIZYGISK_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Custom netlink protocol number.
 *
 * In-tree Linux consumers stop at NETLINK_SMC=22; protocol IDs in the
 * 23..31 range are accepted by netlink_kernel_create() and effectively
 * private (no central registry). 27 is chosen so Tamisu's zygote event
 * channel does not collide with any mainline kernel netlink consumer.
 */
#define YZ_NETLINK_PROTO 27
#define YZ_NL_GROUP_EVENTS 1 /* multicast group: lifecycle events */
#define YZ_NL_MSG_EVENT 0x10 /* nlmsg_type for a yz_event */

enum yz_event_type {
  YZ_EV_SPECIALIZE = 1,
  YZ_EV_RELOAD = 2, /* kernel -> zygiskd: re-read yzconfig.json now */
};

struct yz_event {
  __u32 type; /* enum yz_event_type */
  __u32 pid;
  __u32 appid;
};

/* ---- zygiskd -> kernel control plane (ioctl on the ksu driver) ---- */

#define YZ_MAX_MODULE_FDS 8

/* hand the kernel the module fds it should broker for a just-specialized app */
#define KSU_IOCTL_YZ_HANDOFF _IOC(_IOC_WRITE, 'T', 50, 0)

struct yz_handoff_cmd {
  __u32 pid; /* target app process (tgid) */
  __u32 appid;
  __u32 n_fds; /* valid entries in fds[] */
  __u32 flags; /* injection flags, reserved */
  __s32 fds[YZ_MAX_MODULE_FDS];
};

/* zygiskd -> kernel: offsets of the linker's dlopen/dlsym within linker64, so
 * the kernel resolves them per-zygote as AT_BASE + offset. The injected stub
 * dlopens the loader, then dlsym's and calls its entry (bionic won't run a
 * dlopen'd lib's constructor this early). */
#define KSU_IOCTL_YZ_SET_DLOPEN _IOC(_IOC_WRITE, 'T', 51, 0)

struct yz_dlopen_cmd {
  __u64 dlopen_offset;
  __u64 dlsym_offset;
};

/* zygiskd -> kernel: multicast a YZ_EV_RELOAD to every zygiskd listener so they
 * re-read yzconfig.json. The manager writes the JSON, then asks ksud to fire
 * this (via supercall) -- the config applies on the next specialize, no reboot.
 */
#define KSU_IOCTL_YZ_RELOAD _IOC(_IOC_WRITE, 'T', 52, 0)

#define KSU_IOCTL_YZ_SET_YUKILINKER _IOC(_IOC_WRITE, 'T', 53, 0)

struct yz_yukilinker_cmd {
  __u32 enabled; /* 0 = off (stub loads core directly), 1 = on */
};

#define KSU_IOCTL_YZ_UNMAP_PID _IOC(_IOC_WRITE, 'T', 55, 0)

#define YZ_MAX_UNMAP_SEGS 8

struct yz_unmap_pid_cmd {
  __u32 pid; /* target app process */
  __u32 n_segs; /* valid entries in addr[]/size[] */
  __u64 addr[YZ_MAX_UNMAP_SEGS];
  __u64 size[YZ_MAX_UNMAP_SEGS];
};

#define KSU_IOCTL_YZ_UNMAP_SELF _IOC(_IOC_WRITE, 'T', 56, 0)

struct yz_unmap_self_cmd {
  __u32 n_segs; /* valid entries in addr[]/size[] */
  __u32 reserved;
  __u64 addr[YZ_MAX_UNMAP_SEGS];
  __u64 size[YZ_MAX_UNMAP_SEGS];
  
};

/* ---- runtime config ---- */

/* Mirrors /data/adb/ksu/yukizygisk/yzconfig.json. zygiskd parses the JSON and
 * brokers this compact binary form to core over the daemon socket
 * (ZdRequest::GetConfig). Kept tiny + fixed-layout so it crosses the socket and
 * the JNI boundary unchanged. */
struct yz_config {
  __u8 yukilinker; /* 0=android_dlopen_ext, 1=yukilinker anonymous load */
  __u8
      denylist_mode; /* 0=off, 1=force-umount+no-inject, 2=inject+umount-only */
  __u8 dmesg_log;    /* 0=off, 1=route YukiZygisk logs to dmesg */
  __u8 reserved;
};

#endif /* _UAPI_YUKIZYGISK_H */
