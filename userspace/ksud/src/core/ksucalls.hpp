#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Kernel uapi headers — single source of truth for ioctl numbers,
// struct layouts, and feature IDs. Guarded for userspace by
// #ifndef __KERNEL__ blocks in each uapi header.
extern "C" {
#include "uapi/supercall.h"
}

namespace ksud {

// C++ convenience aliases (keep callers unchanged)
using GetInfoCmd = ksu_get_info_cmd;
using ReportEventCmd = ksu_report_event_cmd;
using SetSepolicyCmd = ksu_set_sepolicy_cmd;
using CheckSafemodeCmd = ksu_check_safemode_cmd;
using GetFeatureCmd = ksu_get_feature_cmd;
using SetFeatureCmd = ksu_set_feature_cmd;
using GetWrapperFdCmd = ksu_get_wrapper_fd_cmd;
using ManageMarkCmd = ksu_manage_mark_cmd;

// API functions
int ksuctl(int request, void* arg);

int32_t get_version();
uint32_t get_flags();
uint32_t get_uapi_version();

void report_post_fs_data();
void report_boot_complete();
void report_module_mounted();
bool check_kernel_safemode();

int set_sepolicy(const void* payload, uint64_t payload_len);

// Feature management
// Returns: pair<value, supported>
std::pair<uint64_t, bool> get_feature(uint32_t feature_id);
int set_feature(uint32_t feature_id, uint64_t value);

int get_wrapped_fd(int fd);

// Mark management
uint32_t mark_get(int32_t pid);
int mark_set(int32_t pid);
int mark_unset(int32_t pid);
int mark_refresh();

int set_init_pgrp();

// Zygisk compatibility stubs: the su/profile/allowlist features were removed
// from the kernel, so these always report "no root granted / no umount".
// Kept because zygiskd's query_flags() path calls them when filling module
// StateFlags; returning false simply means modules see no per-uid grant state.
bool uid_granted_root(uint32_t uid);
bool uid_should_umount(uint32_t uid);

}  // namespace ksud
