//
// Created by weishu on 2022/12/9.
//

#ifndef KERNELSU_KSU_H
#define KERNELSU_KSU_H

#include "prelude.h"
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/types.h>

// --- Kernel UAPI headers (single source of truth) ---
// Zygisk-only build: app_profile/ksu/sulog headers were removed.
// supercall.h now carries only the ioctls we keep (info, feature,
// sepolicy, safemode, mark, yz_*).
#include "uapi/feature.h"
#include "uapi/supercall.h"

#define KSU_FULL_VERSION_STRING 255

// --- Manager JNI helper declarations (zygisk-only) ---

uint32_t get_version();
uint32_t get_uapi_version();

bool is_safe_mode();
bool is_late_load_mode();

void get_full_version(char *buff);
void get_hook_type(char *hook_type);

// Check if KSU driver is present (without authentication)
bool ksu_driver_present(void);

#endif // #ifndef KERNELSU_KSU_H
