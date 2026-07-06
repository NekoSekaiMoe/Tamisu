#include "module.h"
#include "../core/tamisuctl.h"
#include "../core/restorecon.h"
#include "../defs.h"
#include "../log.h"
#include "../sepolicy/sepolicy.h"
#include "../utils.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
extern "C" int resetprop_main(int argc, char** argv);
#endif // #if defined(RESETPROP_ALONE_AVAILABLE) ...

namespace tamisu_daemon {

namespace {

constexpr const char* METADATA_FILE_CON = "u:object_r:metadata_file:s0";

bool file_exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

std::filesystem::path preinit_tamisu_dir() {
    if (std::filesystem::is_directory("/metadata/watchdog")) {
        return PREINIT_DIR_WATCHDOG;
    }
    return PREINIT_DIR_DEFAULT;
}

bool has_rc_extension(const std::filesystem::path& path) {
    return path.extension() == ".rc";
}

void collect_rc_files(const std::filesystem::path& dir, const std::string* module_id,
                      std::ofstream& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }

    std::vector<std::filesystem::directory_entry> entries;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            return;
        }
        entries.push_back(entry);
    }

    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.path().filename().string() < rhs.path().filename().string();
    });

    for (const auto& entry : entries) {
        const std::filesystem::path path = entry.path();
        if (!std::filesystem::is_regular_file(path, ec) || !has_rc_extension(path)) {
            continue;
        }

        if (module_id == nullptr && access(path.c_str(), X_OK) != 0) {
            continue;
        }

        if (module_id != nullptr) {
            out << "# === from " << *module_id << ":" << path.string() << " ===\n";
        } else {
            out << "# === from " << path.string() << " ===\n";
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            LOGW("Failed to read init rc: %s", path.c_str());
            continue;
        }
        out << input.rdbuf();
        out << "\n";
    }
}

void warn_regenerate_preinit_rc_failed(int ret) {
    if (ret != 0) {
        LOGW("regenerate preinit rc failed: %d", ret);
    }
}

}  // namespace

CommonScriptEnv build_common_script_env() {
    CommonScriptEnv env;
    env.kernel_ver_code = std::to_string(get_version());
    const auto [zygisk_value, zygisk_supported] = get_feature(TAMISU_FEATURE_YUKIZYGISK);
    env.zygisk_enabled = zygisk_supported && zygisk_value != 0;

    std::string binary_dir = std::string(BINARY_DIR);
    if (!binary_dir.empty() && binary_dir.back() == '/') {
        binary_dir.pop_back();
    }

    const char* old_path = getenv("PATH");
    if (old_path && old_path[0] != '\0') {
        env.path = std::string(old_path) + ":" + binary_dir;
    } else {
        env.path = binary_dir;
    }

    return env;
}

void apply_common_script_env(const CommonScriptEnv& env, const char* module_id,
                             bool set_magisk_compat) {
    setenv("ASH_STANDALONE", "1", 1);
    setenv("TAMISU", "true", 1);
    setenv("YUKISU", "1", 1);
    setenv("TAMISU_KERNEL_VER_CODE", env.kernel_ver_code.c_str(), 1);
    setenv("TAMISU_VER_CODE", VERSION_CODE, 1);
    setenv("TAMISU_VER", VERSION_NAME, 1);
    setenv("PATH", env.path.c_str(), 1);

    if (env.zygisk_enabled) {
        setenv("ZYGISK_ENABLED", "1", 1);
    } else {
        unsetenv("ZYGISK_ENABLED");
    }

    if (set_magisk_compat) {
        setenv("MAGISK_VER", "25.2", 1);
        setenv("MAGISK_VER_CODE", "25200", 1);
    }

    if (module_id != nullptr && module_id[0] != '\0') {
        setenv("TAMISU_MODULE", module_id, 1);
    } else {
        unsetenv("TAMISU_MODULE");
    }
}

// Forward declaration for run_script (defined below); used by exec_stage_script,
// exec_common_scripts.
int run_script(const std::string& script, bool block, const std::string& module_id = "");

int regenerate_preinit_rc() {
    const std::filesystem::path preinit_dir = preinit_tamisu_dir();
    std::error_code ec;
    std::filesystem::create_directories(preinit_dir, ec);
    if (ec) {
        LOGW("Failed to create %s: %s", preinit_dir.c_str(), ec.message().c_str());
        return 1;
    }

    const std::filesystem::path tmp_path = preinit_dir / MODULES_RC_TMP_FILE;
    const std::filesystem::path out_path = preinit_dir / MODULES_RC_FILE;

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            LOGW("Failed to create %s", tmp_path.c_str());
            return 1;
        }

        collect_rc_files(std::filesystem::path(ADB_DIR) / "initrc.d", nullptr, out);

        std::map<std::string, std::filesystem::path> modules;
        std::map<std::string, bool> skipped_modules;

        for (const char* root : {MODULE_DIR}) {
            if (!std::filesystem::is_directory(root, ec)) {
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
                if (ec) {
                    break;
                }
                if (!entry.is_directory(ec)) {
                    continue;
                }
                const std::string id = entry.path().filename().string();
                if (id.empty()) {
                    continue;
                }
                if (file_exists((entry.path() / DISABLE_FILE_NAME).string()) ||
                    file_exists((entry.path() / REMOVE_FILE_NAME).string())) {
                    modules.erase(id);
                    skipped_modules[id] = true;
                    continue;
                }
                if (skipped_modules.count(id) == 0 && modules.count(id) == 0) {
                    modules.emplace(id, entry.path());
                }
            }
        }

        for (const auto& [id, path] : modules) {
            collect_rc_files(path / MODULE_INIT_RC_DIR, &id, out);
        }

        out.flush();
        if (!out) {
            LOGW("Failed to write %s", tmp_path.c_str());
            return 1;
        }
    }

    std::filesystem::rename(tmp_path, out_path, ec);
    if (ec) {
        LOGW("Failed to rename %s -> %s: %s", tmp_path.c_str(), out_path.c_str(),
             ec.message().c_str());
        std::filesystem::remove(tmp_path, ec);
        return 1;
    }

    (void)lsetfilecon(out_path, METADATA_FILE_CON);

    const std::filesystem::path stale_dir =
        (preinit_dir == PREINIT_DIR_WATCHDOG) ? PREINIT_DIR_DEFAULT : PREINIT_DIR_WATCHDOG;
    std::filesystem::remove(stale_dir / MODULES_RC_FILE, ec);

    return 0;
}

int run_script(const std::string& script, bool block, const std::string& module_id) {
    if (!file_exists(script))
        return 0;

    LOGI("Running script: %s", script.c_str());

    // Use busybox for script execution (like Rust version)
    std::string busybox = BUSYBOX_PATH;
    if (!file_exists(busybox)) {
        LOGW("Busybox not found at %s, falling back to /system/bin/sh", BUSYBOX_PATH);
        busybox = "/system/bin/sh";
    }

    // Get the script's directory for current_dir
    std::string script_dir = script.substr(0, script.find_last_of('/'));
    if (script_dir.empty())
        script_dir = "/";

    // Prepare all environment variable values BEFORE fork
    // to avoid calling C++ library functions in child process
    const CommonScriptEnv common_env = build_common_script_env();

    // Make copies of string data that child process will use
    const char* busybox_path = busybox.c_str();
    const char* script_path = script.c_str();
    const char* script_dir_path = script_dir.c_str();
    const char* module_id_cstr = module_id.c_str();

    const pid_t pid = fork();
    if (pid == 0) {
        // Child process
        setsid();

        // Switch cgroups to escape from parent cgroup (like Rust version)
        switch_cgroups();

        // Change to script directory (like Rust version)
        chdir(script_dir_path);

        // Set environment variables (matching Rust version's get_common_script_envs)
        apply_common_script_env(common_env, module_id_cstr, true);

        // Execute with busybox sh
        execl(busybox_path, "sh", script_path, nullptr);
        _exit(127);
    }

    if (pid < 0) {
        LOGE("Failed to fork for script: %s", script.c_str());
        return -1;
    }

    if (block) {
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    return 0;
}

int exec_stage_script(const std::string& stage, bool block) {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string module_id = entry->d_name;

        // Skip independent zygisk daemons: Tamisu is the zygisk provider when
        // active, and a second loader in the same zygote crashes the process.
        // Priority: Tamisu > {ZygiskNext, NeoZygisk, ReZygisk}.
        if (is_zygisk_impl_module(module_id)) {
            LOGW("Skipping stage script for %s: Tamisu takes over zygisk",
                 module_id.c_str());
            continue;
        }

        const std::string module_path = std::string(MODULE_DIR) + module_id;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        // Skip modules marked for removal
        if (file_exists(module_path + "/" + REMOVE_FILE_NAME))
            continue;

        // Run stage script with module_id for TAMISU_MODULE env var
        std::string script;
        script.reserve(module_path.size() + 1U + stage.size() + 3U);
        script += module_path;
        script += "/";
        script += stage;
        script += ".sh";
        run_script(script, block, module_id);
    }

    closedir(dir);
    return 0;
}

int exec_common_scripts(const std::string& stage_dir, bool block) {
    const std::string dir_path = std::string(ADB_DIR) + stage_dir + "/";
    DIR* dir = opendir(dir_path.c_str());
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        const std::string script = dir_path + entry->d_name;
        if (access(script.c_str(), X_OK) != 0)
            continue;

        run_script(script, block);
    }

    closedir(dir);
    return 0;
}

int load_sepolicy_rule() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip independent zygisk daemons (see exec_stage_script).
        if (is_zygisk_impl_module(entry->d_name))
            continue;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        const std::string rule_file = module_path + "/sepolicy.rule";
        if (!file_exists(rule_file))
            continue;

        // Read and apply rules
        std::ifstream ifs(rule_file);
        std::string line;
        std::string all_rules;
        while (std::getline(ifs, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            all_rules += line + "\n";
        }

        if (!all_rules.empty()) {
            LOGI("Applying sepolicy rules from %s", entry->d_name);
            const int ret = sepolicy_live_patch(all_rules);
            if (ret != 0) {
                LOGW("Failed to apply some sepolicy rules from %s", entry->d_name);
            }
        }
    }

    closedir(dir);
    return 0;
}

int load_system_prop() {
    DIR* dir = opendir(MODULE_DIR);
    if (!dir)
        return 0;

    // Check if resetprop exists
    if (!file_exists(RESETPROP_PATH)) {
        LOGW("resetprop not found at %s, skipping system.prop loading", RESETPROP_PATH);
        closedir(dir);
        return 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string module_path = std::string(MODULE_DIR) + entry->d_name;

        // Skip independent zygisk daemons (see exec_stage_script).
        if (is_zygisk_impl_module(entry->d_name))
            continue;

        // Skip disabled modules
        if (file_exists(module_path + "/" + DISABLE_FILE_NAME))
            continue;

        const std::string prop_file = module_path + "/system.prop";
        if (!file_exists(prop_file))
            continue;

        LOGI("Loading system.prop from %s", entry->d_name);

        // Read and set properties
        std::ifstream ifs(prop_file);
        std::string line;
        while (std::getline(ifs, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;

            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            const std::string key = trim(line.substr(0, eq));
            const std::string value = trim(line.substr(eq + 1));

            // Execute resetprop in a child process
            const pid_t pid = fork();
            if (pid == 0) {
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
                const char* k = key.c_str();
                const char* v = value.c_str();
                std::array<char*, 5> argv_c = {
                    const_cast<char*>("resetprop"),
                    const_cast<char*>("-n"),
                    const_cast<char*>(k),
                    const_cast<char*>(v),
                    nullptr,
                };
                const int rc = resetprop_main(4, argv_c.data());
                _exit(rc);
#else
                execl(RESETPROP_PATH, "resetprop", "-n", key.c_str(), value.c_str(), nullptr);
                _exit(127);
#endif // #if defined(RESETPROP_ALONE_AVAILABLE) ...
            }
            if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
            }
        }
    }

    closedir(dir);
    return 0;
}

// Parse bool config value (true, yes, 1, on -> true)
bool parse_bool_config(const std::string& value) {
    std::string lower = value;
    for (char& c : lower)
        c = tolower(c);
    return lower == "true" || lower == "yes" || lower == "1" || lower == "on";
}

// Merge module configs (persist + temp, temp takes priority)
std::map<std::string, std::string> merge_module_configs(const std::string& module_id) {
    std::map<std::string, std::string> config;

    const std::string config_dir = std::string(MODULE_CONFIG_DIR) + module_id + "/";
    const std::string persist_path = config_dir + PERSIST_CONFIG_NAME;
    const std::string temp_path = config_dir + TEMP_CONFIG_NAME;

    // Load persist config first
    auto persist_content = read_file(persist_path);
    if (persist_content) {
        std::istringstream iss(*persist_content);
        std::string line;
        while (std::getline(iss, line)) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                const std::string key = line.substr(0, eq);
                const std::string value = line.substr(eq + 1);
                config[key] = value;
            }
        }
    }

    // Load temp config (overrides persist)
    auto temp_content = read_file(temp_path);
    if (temp_content) {
        std::istringstream iss(*temp_content);
        std::string line;
        while (std::getline(iss, line)) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                const std::string key = line.substr(0, eq);
                const std::string value = line.substr(eq + 1);
                config[key] = value;
            }
        }
    }

    return config;
}

std::map<std::string, std::vector<std::string>> get_managed_features() {
    std::map<std::string, std::vector<std::string>> managed_features_map;

    DIR* dir = opendir(MODULE_DIR);
    if (!dir) {
        return managed_features_map;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;
        if (entry->d_type != DT_DIR)
            continue;

        const std::string module_id = entry->d_name;
        const std::string module_path = std::string(MODULE_DIR) + module_id;

        // Check if module is active (not disabled/removed)
        if (file_exists(module_path + "/disable"))
            continue;
        if (file_exists(module_path + "/remove"))
            continue;

        // Read module config
        auto config = merge_module_configs(module_id);

        // Extract manage.* config entries
        std::vector<std::string> feature_list;
        for (const auto& [key, value] : config) {
            // Check if key starts with "manage."
            if (key.size() > 7 && key.substr(0, 7) == "manage.") {
                const std::string feature_name = key.substr(7);
                if (parse_bool_config(value)) {
                    feature_list.push_back(feature_name);
                }
            }
        }

        if (!feature_list.empty()) {
            managed_features_map[module_id] = feature_list;
        }
    }

    closedir(dir);
    return managed_features_map;
}

}  // namespace tamisu_daemon
