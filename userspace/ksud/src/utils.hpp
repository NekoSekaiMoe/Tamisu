#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace ksud {

// File system utilities
bool ensure_dir_exists(const std::string& path);
bool ensure_clean_dir(const std::string& path);
bool ensure_file_exists(const std::string& path);
bool ensure_binary(const std::string& path, const uint8_t* data, size_t size,
                   bool ignore_if_exist = false);

// Property utilities
std::optional<std::string> getprop(const std::string& prop);
bool is_safe_mode();

// Process utilities
bool switch_mnt_ns(pid_t pid);
void switch_cgroups();
void umask(mode_t mask);

// Magisk detection
bool has_magisk();

// Zygisk conflict detection.
//
// Tamisu has its own zygisk injection. To avoid double-injecting zygote
// (which crashes the process), it yields to higher-priority zygisk
// implementations and force-disables lower-priority ones.
//
// Priority order (highest first):
//   1. Magisk Zygisk (built-in, userspace native-bridge injection)
//   2. Tamisu (kernel-level zygote hook)
//   3. ZygiskNext / NeoZygisk / ReZygisk (module-based zygisk daemons)
//
// Returns true if Magisk is installed AND its Zygisk toggle is enabled in
// magisk.db. When true, Tamisu zygiskd must not start -- the two loaders
// in one zygote process corrupt each other.
bool is_magisk_zygisk_enabled();

// Module ids that ship an independent zygisk daemon. When Tamisu is active,
// these must be skipped at module-scan time (stage scripts, sepolicy,
// system.prop, zygisk payload) so their injection does not race ours.
bool is_zygisk_impl_module(const std::string& module_id);

// Install/Uninstall
int install(const std::optional<std::string>& magiskboot_path);
int uninstall(const std::optional<std::string>& magiskboot_path);

// Zip utilities
uint64_t get_zip_uncompressed_size(const std::string& zip_path);

// String utilities
std::string trim(const std::string& str);
std::vector<std::string> split(const std::string& str, char delim);
bool starts_with(const std::string& str, const std::string& prefix);
bool ends_with(const std::string& str, const std::string& suffix);

// File I/O
std::optional<std::string> read_file(const std::string& path);
bool write_file(const std::filesystem::path& path, const std::string& content);
bool append_file(const std::filesystem::path& path, const std::string& content);

// Command execution
struct ExecResult {
    int exit_code;
    std::string stdout_str;
    std::string stderr_str;
};
ExecResult exec_command(const std::vector<std::string>& args);
ExecResult exec_command(const std::vector<std::string>& args, const std::string& workdir);
/** Run magiskboot binary (path may be multi-call ksud); argv[0] is set to "magiskboot". */
ExecResult exec_command_magiskboot(const std::string& magiskboot_path,
                                   const std::vector<std::string>& sub_args,
                                   const std::string& workdir = "");
int exec_command_async(const std::vector<std::string>& args);

// Exception-free number parsers. Return true on success.
bool parse_uint32(const std::string& s, uint32_t* out);
bool parse_uint64(const std::string& s, uint64_t* out);

}  // namespace ksud
