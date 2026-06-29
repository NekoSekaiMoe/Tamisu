/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tamisu - kernel <-> zygiskd netlink ABI.
 *
 * Author: Anatdx
 */
#ifndef _UAPI_TAMISU_H
#define _UAPI_TAMISU_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Custom netlink protocol number.
 *
 * In-tree Linux consumers stop at NETLINK_SMC=22; protocol IDs in the
 * 23..31 range are accepted by netlink_kernel_create() and effectively
 * private (no central registry). 27 is chosen so Tamisu's zygote event
 * channel does not collide with any mainline kernel netlink consumer.
 */
#define TAMISU_NETLINK_PROTO 27
#define TAMISU_NL_GROUP_EVENTS 1 /* multicast group: lifecycle events */
#define TAMISU_NL_MSG_EVENT 0x10 /* nlmsg_type for a tamisu_event */

enum tamisu_event_type {
  TAMISU_EV_SPECIALIZE = 1,
  TAMISU_EV_RELOAD = 2, /* kernel -> zygiskd: re-read yzconfig.json now */
};

struct tamisu_event {
  __u32 type; /* enum tamisu_event_type */
  __u32 pid;
  __u32 appid;
};

/* ---- zygiskd -> kernel control plane (ioctl on the ksu driver) ---- */

#define TAMISU_MAX_MODULE_FDS 8

/* hand the kernel the module fds it should broker for a just-specialized app */
#define KSU_IOCTL_TAMISU_HANDOFF _IOC(_IOC_WRITE, 'T', 50, 0)

struct tamisu_handoff_cmd {
  __u32 pid; /* target app process (tgid) */
  __u32 appid;
  __u32 n_fds; /* valid entries in fds[] */
  __u32 flags; /* injection flags, reserved */
  __s32 fds[TAMISU_MAX_MODULE_FDS];
};

/* zygiskd -> kernel: offsets of the linker's dlopen/dlsym within linker64, so
 * the kernel resolves them per-zygote as AT_BASE + offset. The injected stub
 * dlopens the loader, then dlsym's and calls its entry (bionic won't run a
 * dlopen'd lib's constructor this early). */
#define KSU_IOCTL_TAMISU_SET_DLOPEN _IOC(_IOC_WRITE, 'T', 51, 0)

struct tamisu_dlopen_cmd {
  __u64 dlopen_offset;
  __u64 dlsym_offset;
};

/* zygiskd -> kernel: multicast a TAMISU_EV_RELOAD to every zygiskd listener so
 * they re-read yzconfig.json. The manager writes the JSON, then asks ksud to
 * fire this (via supercall) -- the config applies on the next specialize, no
 * reboot.
 */
#define KSU_IOCTL_TAMISU_RELOAD _IOC(_IOC_WRITE, 'T', 52, 0)

#define KSU_IOCTL_TAMISU_SET_YUKILINKER _IOC(_IOC_WRITE, 'T', 53, 0)

struct tamisu_yukilinker_cmd {
  __u32 enabled; /* 0 = off (stub loads core directly), 1 = on */
};

#define KSU_IOCTL_TAMISU_UNMAP_PID _IOC(_IOC_WRITE, 'T', 55, 0)

#define TAMISU_MAX_UNMAP_SEGS 8

struct tamisu_unmap_pid_cmd {
  __u32 pid; /* target app process */
  __u32 n_segs; /* valid entries in addr[]/size[] */
  __u64 addr[TAMISU_MAX_UNMAP_SEGS];
  __u64 size[TAMISU_MAX_UNMAP_SEGS];
};

#define KSU_IOCTL_TAMISU_UNMAP_SELF _IOC(_IOC_WRITE, 'T', 56, 0)

struct tamisu_unmap_self_cmd {
  __u32 n_segs; /* valid entries in addr[]/size[] */
  __u32 reserved;
  __u64 addr[TAMISU_MAX_UNMAP_SEGS];
  __u64 size[TAMISU_MAX_UNMAP_SEGS];

};

/* zygiskd (root) -> kernel: write `len` bytes at `addr` in TARGET pid's address
 * space via access_process_vm(FOLL_FORCE|FOLL_WRITE) -- the kernel equivalent
 * of a /proc/<pid>/mem write. The specialize inline-hook uses this to patch
 * libandroid_runtime's code: FOLL_FORCE makes the kernel copy-on-write the
 * (read-only, file-backed) code page and write through it, without ever calling
 * mprotect -- so the executable mapping is not split. An mprotect-based patch
 * fragments that VMA into several, which detectors flag. copy_to_user_page on
 * the write path flushes the I-cache for the exec range. Routed through
 * zygiskd because the app/zygote cannot get the ksu driver fd; zygiskd resolves
 * the caller pid via SO_PEERCRED, so the target is always the caller's OWN
 * process. Kernel verifies app-or-zygote. */
#define KSU_IOCTL_TAMISU_PATCH_TEXT _IOC(_IOC_WRITE, 'T', 57, 0)

#define TAMISU_PATCH_TEXT_MAX 64

struct tamisu_patch_text_cmd {
  __u32 pid;  /* target process (== the SO_PEERCRED caller) */
  __u32 len;  /* number of bytes to write, 1..TAMISU_PATCH_TEXT_MAX */
  __u64 addr; /* target virtual address */
  __u8 bytes[TAMISU_PATCH_TEXT_MAX]; /* the patch bytes */
};

/* ---- runtime config ---- */

/* Mirrors /data/adb/ksu/tamisu/tamisu_config.json. zygiskd parses the JSON and
 * brokers this compact binary form to core over the daemon socket
 * (ZdRequest::GetConfig). Kept tiny + fixed-layout so it crosses the socket and
 * the JNI boundary unchanged. */
struct tamisu_config {
  __u8 yukilinker; /* 0=android_dlopen_ext, 1=yukilinker anonymous load */
  __u8
      denylist_mode; /* 0=off, 1=force-umount+no-inject, 2=inject+umount-only */
  __u8 dmesg_log;    /* 0=off, 1=route Tamisu logs to dmesg */
  __u8 reserved;
};

#endif /* _UAPI_TAMISU_H */
