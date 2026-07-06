#pragma once

#include <map>
#include <string>
#include <vector>

namespace tamisu_daemon {

struct CommonScriptEnv {
    std::string kernel_ver_code;
    std::string path;
    bool zygisk_enabled{};
};

// Boot infrastructure
int regenerate_preinit_rc();

// Script execution
int exec_stage_script(const std::string& stage, bool block);
int exec_common_scripts(const std::string& stage_dir, bool block);
int load_sepolicy_rule();
int load_system_prop();

// Get all managed features from active modules
// Modules declare managed features via config system (manage.<feature>=true)
// Returns: map<ModuleId, vector<ManagedFeature>>
std::map<std::string, std::vector<std::string>> get_managed_features();

// Shared script environment
CommonScriptEnv build_common_script_env();
void apply_common_script_env(const CommonScriptEnv& env, const char* module_id = nullptr,
                             bool set_magisk_compat = false);

}  // namespace tamisu_daemon
