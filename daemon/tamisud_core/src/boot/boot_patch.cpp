#include "boot_patch.h"
#include "../assets.h"
#include "../defs.h"
#include "../flash/flash_partition.h"
#include "../log.h"
#include "../utils.h"
#include "tools.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <vector>

namespace fs = std::filesystem;

namespace tamisu_daemon {

// Forward declaration — defined near get_current_kmi() at the bottom of this file.
std::string read_kernel_release_from_sysfs();

namespace {

// LZ4 legacy ramdisk magic (reject before cpio to avoid huge cache/hang).
constexpr std::array<unsigned char, 4> LZ4_LEGACY_MAGIC = {0x02, 0x21, 0x4c, 0x18};

// Execute magiskboot cpio command (runs in workdir for relative path resolution).
bool do_cpio_cmd(const std::string& magiskboot, const std::string& workdir,
                 const std::string& cpio_path, const std::string& cmd) {
    auto result = exec_command_magiskboot(magiskboot, {"cpio", cpio_path, cmd}, workdir);
    if (result.exit_code != 0) {
        LOGE("magiskboot cpio %s failed", cmd.c_str());
        if (!result.stdout_str.empty()) {
            LOGE("magiskboot cpio stdout: %s", result.stdout_str.c_str());
        }
        if (!result.stderr_str.empty()) {
            LOGE("magiskboot cpio stderr: %s", result.stderr_str.c_str());
        }
        return false;
    }
    return true;
}

// Check if boot image is patched by Magisk
bool is_magisk_patched(const std::string& magiskboot, const std::string& workdir,
                       const std::string& cpio_path) {
    auto result = exec_command_magiskboot(magiskboot, {"cpio", cpio_path, "test"}, workdir);
    // According to magiskboot docs: 0 = stock, 1 = magisk, 2 = unsupported.
    // 但这里额外做一层防御性检查，避免误报：
    if (result.exit_code != 1) {
        return false;
    }

    // 双重确认：检查典型的 Magisk 迹象（init.magisk.rc 或 overlay.d 等）
    auto has_magisk_init =
        exec_command_magiskboot(magiskboot, {"cpio", cpio_path, "exists init.magisk.rc"}, workdir);
    auto has_overlay =
        exec_command_magiskboot(magiskboot, {"cpio", cpio_path, "exists overlay.d"}, workdir);

    return has_magisk_init.exit_code == 0 || has_overlay.exit_code == 0;
}

// List cpio entries under path (non-recursive by default; recursive if recursive=true).
// magiskboot cpio ls output lines look like: "<mode>\t<path>"
std::vector<std::string> cpio_list_entries(const std::string& magiskboot,
                                           const std::string& workdir,
                                           const std::string& cpio_path,
                                           const std::string& path, bool recursive) {
    std::vector<std::string> args = {"cpio", cpio_path};
    if (recursive) {
        args.push_back("ls -r " + path);
    } else {
        args.push_back("ls " + path);
    }
    auto result = exec_command_magiskboot(magiskboot, args, workdir);
    std::vector<std::string> entries;
    if (result.exit_code != 0 || result.stdout_str.empty()) {
        return entries;
    }
    for (const auto& line : split(result.stdout_str, '\n')) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        auto entry = trim(line.substr(tab + 1));
        if (!entry.empty()) {
            entries.push_back(entry);
        }
    }
    return entries;
}

bool cpio_entry_exists(const std::string& magiskboot, const std::string& workdir,
                       const std::string& cpio_path, const std::string& entry) {
    auto result =
        exec_command_magiskboot(magiskboot, {"cpio", cpio_path, "exists " + entry}, workdir);
    return result.exit_code == 0;
}

// Extract a single cpio entry to a workdir-relative outfile. Returns true on success.
bool cpio_extract_entry(const std::string& magiskboot, const std::string& workdir,
                        const std::string& cpio_path, const std::string& entry,
                        const std::string& outfile_rel) {
    auto result = exec_command_magiskboot(
        magiskboot, {"cpio", cpio_path, "extract " + entry + " " + outfile_rel}, workdir);
    return result.exit_code == 0;
}

// Ensure text ends with a single trailing newline (if non-empty).
std::string ensure_trailing_newline(std::string s) {
    if (s.empty()) {
        return s;
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    s.push_back('\n');
    return s;
}

// Append a line to modules.load if not already present (matches basename or full path).
std::string merge_modules_load(const std::string& existing, const std::string& module_line) {
    const std::string needle = trim(module_line);
    if (needle.empty()) {
        return existing;
    }
    for (const auto& line : split(existing, '\n')) {
        auto t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        // Match "tamisu.ko", "tamisu", or any path ending with /tamisu.ko
        if (t == needle || t == "tamisu" || ends_with(t, "/" + needle) || ends_with(t, "/tamisu")) {
            return existing;
        }
    }
    std::string out = ensure_trailing_newline(existing);
    out += needle;
    out += '\n';
    return out;
}

// Append "tamisu.ko:" dep entry if missing.
std::string merge_modules_dep(const std::string& existing, const std::string& dep_line) {
    const std::string needle = "tamisu.ko";
    for (const auto& line : split(existing, '\n')) {
        auto t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        auto colon = t.find(':');
        auto key = (colon == std::string::npos) ? t : t.substr(0, colon);
        key = trim(key);
        if (key == needle || ends_with(key, "/" + needle)) {
            return existing;
        }
    }
    std::string out = ensure_trailing_newline(existing);
    out += dep_line;
    if (out.back() != '\n') {
        out += '\n';
    }
    return out;
}

// Remove tamisu entries from modules.load content.
std::string strip_tamisu_from_modules_load(const std::string& existing) {
    std::string out;
    for (const auto& line : split(existing, '\n')) {
        auto t = trim(line);
        if (t.empty()) {
            continue;
        }
        if (t == "tamisu.ko" || t == "tamisu" || ends_with(t, "/tamisu.ko") ||
            ends_with(t, "/tamisu")) {
            continue;
        }
        out += line;
        out += '\n';
    }
    return out;
}

// Remove tamisu.ko dependency lines from modules.dep content.
std::string strip_tamisu_from_modules_dep(const std::string& existing) {
    std::string out;
    for (const auto& line : split(existing, '\n')) {
        auto t = trim(line);
        if (t.empty()) {
            continue;
        }
        auto colon = t.find(':');
        auto key = (colon == std::string::npos) ? t : t.substr(0, colon);
        key = trim(key);
        if (key == "tamisu.ko" || ends_with(key, "/tamisu.ko")) {
            continue;
        }
        out += line;
        out += '\n';
    }
    return out;
}

// Pick the ramdisk lib/modules/<dir> we should inject into.
// Prefer exact uname -r, then major.minor match that already has modules.load,
// then any existing modules dir with modules.load. Empty if none.
std::string select_modules_dir(const std::string& magiskboot, const std::string& workdir,
                               const std::string& cpio_path, const std::string& kernel_release) {
    if (cpio_entry_exists(magiskboot, workdir, cpio_path,
                          "lib/modules/" + kernel_release + "/modules.load") ||
        cpio_entry_exists(magiskboot, workdir, cpio_path, "lib/modules/" + kernel_release)) {
        return "lib/modules/" + kernel_release;
    }

    int major = 0;
    int minor = 0;
    sscanf(kernel_release.c_str(), "%d.%d", &major, &minor);

    auto entries = cpio_list_entries(magiskboot, workdir, cpio_path, "lib/modules", true);
    std::vector<std::string> candidates;
    for (const auto& e : entries) {
        // Expect lib/modules/<release>/...
        if (!starts_with(e, "lib/modules/")) {
            continue;
        }
        auto rest = e.substr(std::string("lib/modules/").size());
        auto slash = rest.find('/');
        if (slash == std::string::npos) {
            // directory entry itself: lib/modules/<release>
            if (!rest.empty() && rest != "." && rest != "..") {
                candidates.push_back("lib/modules/" + rest);
            }
            continue;
        }
        candidates.push_back("lib/modules/" + rest.substr(0, slash));
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    std::string mm_match;
    std::string any_with_load;
    for (const auto& dir : candidates) {
        const bool has_load =
            cpio_entry_exists(magiskboot, workdir, cpio_path, dir + "/modules.load");
        if (!has_load) {
            continue;
        }
        if (any_with_load.empty()) {
            any_with_load = dir;
        }
        auto name = dir.substr(std::string("lib/modules/").size());
        int dmaj = 0;
        int dmin = 0;
        if (sscanf(name.c_str(), "%d.%d", &dmaj, &dmin) == 2 && dmaj == major && dmin == minor) {
            mm_match = dir;
            break;
        }
    }
    if (!mm_match.empty()) {
        return mm_match;
    }
    if (!any_with_load.empty()) {
        return any_with_load;
    }
    if (!candidates.empty()) {
        return candidates.front();
    }
    return "";
}

// Check if boot image is already patched by Tamisu.
// Detects modprobe layout (tamisu.ko under /lib/modules/) and legacy flat layout.
bool is_tamisu_patched(const std::string& magiskboot, const std::string& workdir,
                       const std::string& cpio_path) {
    // Legacy: flat tamisu.ko at ramdisk root
    if (cpio_entry_exists(magiskboot, workdir, cpio_path, "tamisu.ko")) {
        return true;
    }
    // Look for tamisu.ko under lib/modules (stock may already have lib/modules without us)
    auto entries = cpio_list_entries(magiskboot, workdir, cpio_path, "lib/modules", true);
    for (const auto& e : entries) {
        if (e == "tamisu.ko" || ends_with(e, "/tamisu.ko")) {
            return true;
        }
    }
    return false;
}

// Repack ramdisk using kernel-version-specific mkbootfs (fixes 5.15+ boot on some devices)
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool repack_ramdisk_with_mkbootfs(const std::string& magiskboot, const std::string& mkbootfs_dir,
                                  const std::string& workdir, const std::string& ramdisk_path) {
    struct utsname uts{};
    if (uname(&uts) != 0) {
        LOGW("uname failed, skip mkbootfs repack");
        return false;
    }
    int major = 0;
    int minor = 0;
    if (sscanf(uts.release, "%d.%d", &major, &minor) < 2) {
        LOGW("Cannot parse kernel version from %s", uts.release);
        return false;
    }
    const bool use_5_15 = (major > 5) || (major == 5 && minor >= 15);
    const std::string mkbootfs_name = use_5_15 ? "5_15+-mkbootfs" : "5_10-mkbootfs";
    const std::string mkbootfs_path = mkbootfs_dir + "/" + mkbootfs_name;
    if (access(mkbootfs_path.c_str(), X_OK) != 0) {
        LOGW("mkbootfs not found or not executable: %s", mkbootfs_path.c_str());
        return false;
    }

    const std::string extract_dir = workdir + "/ramdisk_mkbootfs_extract";
    if (!ensure_clean_dir(extract_dir)) {
        LOGE("Failed to create mkbootfs extract dir");
        return false;
    }

    const std::string ramdisk_rel = (ramdisk_path.size() > workdir.size() &&
                                     ramdisk_path.compare(0, workdir.size(), workdir) == 0 &&
                                     ramdisk_path[workdir.size()] == '/')
                                        ? ramdisk_path.substr(workdir.size() + 1)
                                        : ramdisk_path;
    const std::string ramdisk_for_extract = std::string("../") + ramdisk_rel;
    auto extract_result =
        exec_command_magiskboot(magiskboot, {"cpio", ramdisk_for_extract, "extract"}, extract_dir);
    if (extract_result.exit_code != 0) {
        LOGE("magiskboot cpio extract failed");
        return false;
    }

    auto mkbootfs_result = exec_command({mkbootfs_path, "."}, extract_dir);
    if (mkbootfs_result.exit_code != 0) {
        LOGE("mkbootfs failed (exit %d)", mkbootfs_result.exit_code);
        return false;
    }

    const std::string ramdisk_new = ramdisk_path + ".mkbootfs_new";
    if (!write_file(ramdisk_new, mkbootfs_result.stdout_str)) {
        LOGE("Failed to write mkbootfs output");
        return false;
    }
    if (rename(ramdisk_new.c_str(), ramdisk_path.c_str()) != 0) {
        LOGE("Failed to replace ramdisk: %s", strerror(errno));
        return false;
    }
    printf("- Repacked ramdisk with %s (kernel %s)\n", mkbootfs_name.c_str(), uts.release);
    return true;
}

// Flash boot image
bool flash_boot(const std::string& bootdevice, const std::string& new_boot) {
    if (bootdevice.empty()) {
        LOGE("Boot device not found");
        return false;
    }

    // Set device to read-write
    auto result = exec_command({"blockdev", "--setrw", bootdevice});
    if (result.exit_code != 0) {
        // Some devices/partitions do not require or allow this ioctl; continue and let dd decide.
        LOGW("blockdev --setrw failed, continue flashing: %s", bootdevice.c_str());
        if (!result.stderr_str.empty()) {
            LOGW("blockdev stderr: %s", result.stderr_str.c_str());
        }
        if (!result.stdout_str.empty()) {
            LOGW("blockdev stdout: %s", result.stdout_str.c_str());
        }
    }

    if (!exec_dd(new_boot, bootdevice)) {
        LOGE("Failed to flash boot image");
        return false;
    }

    return true;
}

// Calculate SHA1 hash
std::string calculate_sha1(const std::string& file_path) {
    // Use sha1sum command
    auto result = exec_command({"sha1sum", file_path});
    if (result.exit_code != 0) {
        return "";
    }
    // Output format: "hash  filename"
    const size_t space = result.stdout_str.find(' ');
    if (space != std::string::npos) {
        return result.stdout_str.substr(0, space);
    }
    return trim(result.stdout_str);
}

// Backup stock boot image
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool do_backup(const std::string& magiskboot, const std::string& workdir,
               const std::string& cpio_path, const std::string& image) {
    const std::string sha1 = calculate_sha1(image);
    if (sha1.empty()) {
        LOGE("Failed to calculate SHA1 of boot image");
        return false;
    }

    const std::string filename = std::string(TAMISU_BACKUP_FILE_PREFIX) + sha1;
    printf("- Backup stock boot image\n");

    const std::string target = std::string(TAMISU_BACKUP_DIR) + filename;

    // Copy image to backup location
    std::ifstream src(image, std::ios::binary);
    std::ofstream dst(target, std::ios::binary);
    if (!src || !dst) {
        LOGE("Failed to backup boot image to %s", target.c_str());
        return false;
    }
    dst << src.rdbuf();
    src.close();
    dst.close();

    // Write sha1 to workdir
    const std::string sha1_file = workdir + "/" + BACKUP_FILENAME;
    write_file(sha1_file, sha1);

    // Add backup info to ramdisk
    if (!do_cpio_cmd(
            magiskboot, workdir, cpio_path,
            "add 0644 " + std::string(BACKUP_FILENAME) + " " + std::string(BACKUP_FILENAME))) {
        return false;
    }

    printf("- Stock image has been backup to\n");
    printf("- %s\n", target.c_str());
    return true;
}

// Clean old backups
void clean_backup(const std::string& current_sha1) {
    printf("- Clean up backup\n");
    const std::string backup_name = std::string(TAMISU_BACKUP_FILE_PREFIX) + current_sha1;

    std::error_code ec;
    for (auto it = fs::directory_iterator(TAMISU_BACKUP_DIR, ec);
         it != fs::directory_iterator() && !ec; it.increment(ec)) {
        std::error_code rf_ec;
        if (!it->is_regular_file(rf_ec))
            continue;

        const std::string name = it->path().filename().string();
        if (name != backup_name && starts_with(name, TAMISU_BACKUP_FILE_PREFIX)) {
            if (fs::remove(it->path())) {
                printf("- removed %s\n", name.c_str());
            }
        }
    }
    if (ec) {
        LOGW("Clean backup error: %s", ec.message().c_str());
    }
}

}  // namespace

// Parse boot patch arguments
struct BootPatchArgs {
    std::string boot_image;         // -b, --boot
    std::string kernel;             // -k, --kernel
    std::string module;             // -m, --module (LKM path)
    bool ota = false;               // -u, --ota
    bool flash = false;             // -f, --flash
    std::string out;                // -o, --out
    std::string magiskboot;         // --magiskboot
    std::string mkbootfs_dir;       // --mkbootfs-dir (5_10/5_15+ tools for ramdisk format)
    std::string kmi;                // --kmi
    std::string partition;          // --partition
    std::string out_name;           // --out-name
    bool enable_adbd = false;       // --enable-adbd
    std::string adb_debug_prop;     // --adb-debug-prop
};

namespace {

BootPatchArgs parse_boot_patch_args(const std::vector<std::string>& args) {
    BootPatchArgs result;

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];

        if (arg == "-b" || arg == "--boot") {
            if (i + 1 < args.size())
                result.boot_image = args[++i];
        } else if (arg == "-k" || arg == "--kernel") {
            if (i + 1 < args.size())
                result.kernel = args[++i];
        } else if (arg == "-m" || arg == "--module") {
            if (i + 1 < args.size())
                result.module = args[++i];
        } else if (arg == "-u" || arg == "--ota") {
            result.ota = true;
        } else if (arg == "-f" || arg == "--flash") {
            result.flash = true;
        } else if (arg == "-o" || arg == "--out") {
            if (i + 1 < args.size())
                result.out = args[++i];
        } else if (arg == "--magiskboot") {
            if (i + 1 < args.size())
                result.magiskboot = args[++i];
        } else if (arg == "--mkbootfs-dir") {
            if (i + 1 < args.size())
                result.mkbootfs_dir = args[++i];
        } else if (arg == "--kmi") {
            if (i + 1 < args.size())
                result.kmi = args[++i];
        } else if (arg == "--partition") {
            if (i + 1 < args.size())
                result.partition = args[++i];
        } else if (arg == "--out-name") {
            if (i + 1 < args.size())
                result.out_name = args[++i];
        } else if (arg == "--enable-adbd") {
            result.enable_adbd = true;
        } else if (arg == "--adb-debug-prop") {
            if (i + 1 < args.size())
                result.adb_debug_prop = args[++i];
        }
    }

    return result;
}

int boot_patch_impl(const std::vector<std::string>& args) {
    auto parsed = parse_boot_patch_args(args);

    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    printf("\n");
    printf("__   __ _   _  _  __ ___  ____   _   _ \n");
    printf("\\ \\ / /| | | || |/ /|_ _|/ ___| | | | |\n");
    printf(" \\ V / | | | || ' /  | | \\___ \\ | | | |\n");
    printf("  | |  | |_| || . \\  | |  ___) || |_| |\n");
    printf("  |_|   \\___/ |_|\\_\\|___||____/  \\___/ \n");
    printf("\n");

    // Create temp working directory
    // Try multiple locations in order of preference:
    // 1. TMPDIR environment variable (set by manager app to its cache dir)
    // 2. Current working directory (if writable)
    // 3. /data/local/tmp (fallback, requires shell access)
    std::string workdir;
    char* tmpdir = nullptr;

    // Try TMPDIR env first (manager sets this to app cache dir)
    const char* env_tmpdir = getenv("TMPDIR");
    if (env_tmpdir && access(env_tmpdir, W_OK) == 0) {
        std::string template_path = std::string(env_tmpdir) + "/Tamisu_XXXXXX";
        std::vector<char> tmpdir_template(template_path.begin(), template_path.end());
        tmpdir_template.push_back('\0');
        tmpdir = mkdtemp(tmpdir_template.data());
        if (tmpdir) {
            workdir = tmpdir;
            printf("- Using TMPDIR: %s\n", env_tmpdir);
        }
    }

    // Try current directory
    if (workdir.empty()) {
        std::array<char, PATH_MAX> cwd{};
        if (getcwd(cwd.data(), cwd.size()) && access(cwd.data(), W_OK) == 0) {
            std::string template_path = std::string(cwd.data()) + "/Tamisu_XXXXXX";
            std::vector<char> tmpdir_template(template_path.begin(), template_path.end());
            tmpdir_template.push_back('\0');
            tmpdir = mkdtemp(tmpdir_template.data());
            if (tmpdir) {
                workdir = tmpdir;
                printf("- Using current directory: %s\n", cwd.data());
            }
        }
    }

    // Fallback to /data/local/tmp
    if (workdir.empty()) {
        std::array<char, 32> tmpdir_buf{};
        (void)strncpy(tmpdir_buf.data(), "/data/local/tmp/Tamisu_XXXXXX", tmpdir_buf.size() - 1);
        tmpdir_buf[tmpdir_buf.size() - 1] = '\0';
        tmpdir = mkdtemp(tmpdir_buf.data());
        if (tmpdir) {
            workdir = tmpdir;
        }
    }

    if (workdir.empty()) {
        LOGE("Failed to create temp directory");
        LOGE("Try setting TMPDIR environment variable to a writable directory");
        return 1;
    }

    // Cleanup function
    auto cleanup = [&workdir]() {
        const std::string cmd = "rm -rf " + workdir;
        system(cmd.c_str());
    };

    // Find magiskboot
    const std::string magiskboot = find_magiskboot(parsed.magiskboot, workdir);
    if (magiskboot.empty()) {
        cleanup();
        return 1;
    }
    printf("- Using magiskboot: %s\n", magiskboot.c_str());

    // Get or detect KMI
    std::string kmi = parsed.kmi;
    if (kmi.empty()) {
        kmi = get_current_kmi();
        if (kmi.empty() && parsed.boot_image.empty()) {
            printf("- Failed to detect KMI and no boot image specified\n");
            cleanup();
            return 1;
        }
    }
    if (!kmi.empty()) {
        printf("- KMI: %s\n", kmi.c_str());
    }

    // Determine boot image path
    std::string bootimage;
    std::string bootdevice;
    const bool patch_file = !parsed.boot_image.empty();

    if (patch_file) {
        bootimage = parsed.boot_image;
        if (access(bootimage.c_str(), R_OK) != 0) {
            LOGE("Boot image not found: %s", bootimage.c_str());
            cleanup();
            return 1;
        }
    } else {
        // Auto-detect boot partition
        std::string partition_name;

        // Determine if we're in replace kernel mode (when --kernel is specified)
        const bool is_replace_kernel = !parsed.kernel.empty();

        if (!parsed.partition.empty()) {
            // User specified partition name (e.g., "init_boot" or "boot")
            partition_name =
                choose_boot_partition(kmi, parsed.ota, &parsed.partition, is_replace_kernel);
        } else {
            // Auto-detect: choose_boot_partition returns full path with slot
            partition_name = choose_boot_partition(kmi, parsed.ota, nullptr, is_replace_kernel);
        }

        printf("- Bootdevice: %s\n", partition_name.c_str());

        bootimage = workdir + "/boot.img";
        if (!exec_dd(partition_name, bootimage)) {
            LOGE("Failed to read boot image from %s", partition_name.c_str());
            cleanup();
            return 1;
        }
        bootdevice = partition_name;
    }

    // Prepare LKM module
    printf("- Preparing assets\n");
    const std::string kmod_file = workdir + "/tamisu.ko";

    if (!parsed.module.empty()) {
        // Use specified module
        std::ifstream src(parsed.module, std::ios::binary);  // NOLINT(misc-const-correctness)
        std::ofstream dst(kmod_file, std::ios::binary);
        if (!src || !dst) {
            LOGE("Failed to copy kernel module from %s", parsed.module.c_str());
            cleanup();
            return 1;
        }
        dst << src.rdbuf();
    } else {
        // Try to extract LKM from embedded assets first
        const std::string kmi_lkm_name = kmi + "_tamisu.ko";
        printf("- KMI: %s\n", kmi.c_str());

        if (copy_asset_to_file(kmi_lkm_name, kmod_file)) {
            printf("- Using embedded LKM: %s\n", kmi_lkm_name.c_str());
        } else {
            // Fallback: try to find LKM from known locations
            const std::vector<std::string> search_paths = {
                std::string(BINARY_DIR) + kmi_lkm_name,  std::string(BINARY_DIR) + "tamisu.ko",
                std::string(WORKING_DIR) + kmi_lkm_name, std::string(WORKING_DIR) + "tamisu.ko",
                "/data/local/tmp/" + kmi_lkm_name,       "/data/local/tmp/tamisu.ko",
            };

            bool found = false;
            for (const auto& path : search_paths) {
                if (access(path.c_str(), R_OK) == 0) {
                    printf("- Found LKM at %s\n", path.c_str());
                    std::ifstream src(path, std::ios::binary);  // NOLINT(misc-const-correctness)
                    std::ofstream dst(kmod_file, std::ios::binary);
                    if (src && dst) {
                        dst << src.rdbuf();
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                // List available KMIs from embedded assets
                auto supported = list_supported_kmi();

                printf("\n");
                printf("! No LKM module found for KMI: %s\n", kmi.c_str());
                printf("!\n");
                if (!supported.empty()) {
                    printf("! Supported KMIs in this build:\n");
                    for (const auto& k : supported) {
                        printf("!   - %s\n", k.c_str());
                    }
                    printf("!\n");
                }
                printf("! Please select an LKM file in Manager, or place it at:\n");
                printf("!   %s%s\n", BINARY_DIR, kmi_lkm_name.c_str());
                printf("!\n");
                printf("! You can download LKM from:\n");
                printf("!   https://github.com/Anatdx/Tamisu/releases\n");
                printf("\n");
                cleanup();
                return 1;
            }
        }
    }

    // Read the real kernel release (e.g. "6.6.66-android15-8-g29d86c5fc9dd").
    // This is the directory name AOSP first_stage init's LoadKernelModules() looks for under
    // /lib/modules/. boot-patch runs on the target device, so /proc/sys/core/osrelease gives
    // the correct release of the kernel we are patching for.
    std::string kernel_release = read_kernel_release_from_sysfs();
    if (kernel_release.empty()) {
        struct utsname uts{};
        if (uname(&uts) == 0) {
            kernel_release = uts.release;
        }
    }
    if (kernel_release.empty()) {
        LOGE("Failed to determine kernel release for modprobe directory layout");
        cleanup();
        return 1;
    }
    printf("- Kernel release: %s\n", kernel_release.c_str());

    // Unpack boot image (must run in workdir so output files go there)
    printf("- Unpacking boot image\n");
    fflush(stdout);
    printf("- magiskboot: %s\n", magiskboot.c_str());
    printf("- bootimage: %s\n", bootimage.c_str());
    printf("- workdir: %s\n", workdir.c_str());

    // Verify boot image exists and is readable
    struct stat boot_stat{};
    if (stat(bootimage.c_str(), &boot_stat) != 0) {
        LOGE("Boot image not accessible: %s (errno=%d)", bootimage.c_str(), errno);
        cleanup();
        return 1;
    }
    printf("- Boot image size: %ld bytes\n", (long)boot_stat.st_size);

    // Try normal unpack first (with decompress). On some devices LZ4_LEGACY in-process decompress
    // crashes (SIGSEGV, exit 139); then we retry with --skip-decomp and tell user to use PC.
    std::vector<std::string> unpack_args = {"unpack", bootimage};  // NOLINT(misc-const-correctness)
    auto unpack_result = exec_command_magiskboot(magiskboot, unpack_args, workdir);
    printf("- unpack exit code: %d\n", unpack_result.exit_code);
    if (!unpack_result.stdout_str.empty()) {
        printf("- stdout: %s\n", unpack_result.stdout_str.c_str());
    }
    if (!unpack_result.stderr_str.empty()) {
        printf("- stderr: %s\n", unpack_result.stderr_str.c_str());
    }

#ifdef __ANDROID__
    constexpr int SIGSEGV_EXIT = 128 + 11;  // 139
    if (unpack_result.exit_code == SIGSEGV_EXIT) {
        printf(
            "- Unpack crashed (SIGSEGV); retrying with --skip-decomp to avoid LZ4 decompress.\n");
        unpack_args.push_back("--skip-decomp");
        unpack_result = exec_command_magiskboot(magiskboot, unpack_args, workdir);
        printf("- unpack (skip-decomp) exit code: %d\n", unpack_result.exit_code);
        if (unpack_result.exit_code != 0) {
            LOGE("magiskboot unpack failed with exit code %d", unpack_result.exit_code);
            cleanup();
            return 1;
        }
        std::string ramdisk_cpio = workdir + "/ramdisk.cpio";
        std::ifstream ramdisk_in(ramdisk_cpio, std::ios::binary);
        std::array<unsigned char, 4> magic_buf{};
        if (ramdisk_in && ramdisk_in.read(reinterpret_cast<char*>(magic_buf.data()), 4) &&
            ramdisk_in.gcount() == 4 && memcmp(magic_buf.data(), LZ4_LEGACY_MAGIC.data(), 4) == 0) {
            ramdisk_in.close();
            LOGE("Ramdisk is LZ4_LEGACY; on-device decompress crashed (SIGSEGV), so it was "
                 "skipped.");
            LOGE("Cannot continue without decompressed ramdisk. Patch the boot image on a PC "
                 "instead.");
            cleanup();
            return 1;
        }
    } else
#endif  // #ifdef __ANDROID__
        if (unpack_result.exit_code != 0) {
            LOGE("magiskboot unpack failed with exit code %d", unpack_result.exit_code);
            cleanup();
            return 1;
        }

    // Find ramdisk
    std::string ramdisk;
    const std::vector<std::string> ramdisk_candidates_do_patch = {
        workdir + "/ramdisk.cpio", workdir + "/vendor_ramdisk/init_boot.cpio",
        workdir + "/vendor_ramdisk/ramdisk.cpio"};

    for (const auto& candidate : ramdisk_candidates_do_patch) {
        if (access(candidate.c_str(), R_OK) == 0) {
            ramdisk = candidate;
            break;
        }
    }

    if (ramdisk.empty()) {
        printf("- No ramdisk found, creating default\n");
        ramdisk = workdir + "/ramdisk.cpio";
        // Create empty ramdisk (use a valid entry name; "." is invalid for some magiskboot builds)
        if (!do_cpio_cmd(magiskboot, workdir, ramdisk, "mkdir 000 .backup")) {
            LOGE("Failed to create default ramdisk");
            cleanup();
            return 1;
        }
    } else {
        // Unconditionally reject LZ4 ramdisk before any cpio (avoids cpio parsing LZ4 as cpio →
        // huge cache/hang).
        std::ifstream ramdisk_check(ramdisk, std::ios::binary);
        std::array<unsigned char, 4> magic_buf{};
        if (ramdisk_check && ramdisk_check.read(reinterpret_cast<char*>(magic_buf.data()), 4) &&
            ramdisk_check.gcount() == 4 &&
            memcmp(magic_buf.data(), LZ4_LEGACY_MAGIC.data(), 4) == 0) {
            ramdisk_check.close();
            LOGE("Ramdisk is LZ4 compressed; cannot patch. Use PC to patch this boot image.\n");
            cleanup();
            return 1;
        }
    }

    // Check for Magisk
    if (is_magisk_patched(magiskboot, workdir, ramdisk)) {
        LOGE("Cannot work with Magisk patched image");
        cleanup();
        return 1;
    }

    printf("- Adding tamisu.ko via AOSP first_stage modprobe layout\n");
    const bool already_patched = is_tamisu_patched(magiskboot, workdir, ramdisk);
    (void)already_patched;

    // AOSP first_stage LoadKernelModules() prefers an exact /lib/modules/<uname -r>
    // directory and loads ONLY from that dir when present. Creating a private
    // uname-r dir with only tamisu.ko would hijack module loading and drop
    // vendor modules (storage/display), causing first-stage FATAL reboot.
    // Prefer merging into an existing modules directory; only fall back to a
    // fresh uname-r dir when the ramdisk has no lib/modules tree at all.
    std::string mod_dir =
        select_modules_dir(magiskboot, workdir, ramdisk, kernel_release);
    const bool created_private_dir = mod_dir.empty();
    if (created_private_dir) {
        mod_dir = "lib/modules/" + kernel_release;
        printf("- No existing modules dir in ramdisk; creating %s\n", mod_dir.c_str());
    } else {
        printf("- Merging into existing modules dir: %s\n", mod_dir.c_str());
    }

    // Staging tree under workdir; magiskboot cpio add INFILE must be workdir-relative.
    const std::string mod_dir_local = workdir + "/modroot/" + mod_dir;
    const std::string mod_dir_rel = "modroot/" + mod_dir;

    std::error_code ec;
    fs::create_directories(mod_dir_local, ec);
    if (ec) {
        LOGE("Failed to create %s: %s", mod_dir_local.c_str(), ec.message().c_str());
        cleanup();
        return 1;
    }

    // Stage tamisu.ko
    {
        const std::string dst_ko = mod_dir_local + "/tamisu.ko";
        std::ifstream src(kmod_file, std::ios::binary);
        std::ofstream dst(dst_ko, std::ios::binary);
        if (!src || !dst) {
            LOGE("Failed to stage tamisu.ko into modprobe dir");
            cleanup();
            return 1;
        }
        dst << src.rdbuf();
    }

    // Merge modules.load (append; never replace vendor list)
    {
        std::string load_content;
        const std::string load_entry = mod_dir + "/modules.load";
        if (cpio_entry_exists(magiskboot, workdir, ramdisk, load_entry)) {
            const std::string extracted = mod_dir_rel + "/modules.load.orig";
            if (cpio_extract_entry(magiskboot, workdir, ramdisk, load_entry, extracted)) {
                auto content = read_file(workdir + "/" + extracted);
                if (content) {
                    load_content = *content;
                }
            }
        }
        load_content = merge_modules_load(load_content, "tamisu.ko");
        if (!write_file(mod_dir_local + "/modules.load", load_content)) {
            LOGE("Failed to write merged modules.load");
            cleanup();
            return 1;
        }
    }

    // Merge modules.dep (append "tamisu.ko:" if missing)
    {
        std::string dep_content;
        const std::string dep_entry = mod_dir + "/modules.dep";
        if (cpio_entry_exists(magiskboot, workdir, ramdisk, dep_entry)) {
            const std::string extracted = mod_dir_rel + "/modules.dep.orig";
            if (cpio_extract_entry(magiskboot, workdir, ramdisk, dep_entry, extracted)) {
                auto content = read_file(workdir + "/" + extracted);
                if (content) {
                    dep_content = *content;
                }
            }
        }
        // Prefer full path form used by depgen when we created a private dir; basename works too.
        dep_content = merge_modules_dep(dep_content, "tamisu.ko:");
        if (!write_file(mod_dir_local + "/modules.dep", dep_content)) {
            LOGE("Failed to write merged modules.dep");
            cleanup();
            return 1;
        }
    }

    // Ensure parent dirs exist in cpio, then add files (add replaces if present).
    {
        std::vector<std::string> parts;
        std::string acc;
        for (const auto& part : split(mod_dir, '/')) {
            if (part.empty()) {
                continue;
            }
            if (!acc.empty()) {
                acc += '/';
            }
            acc += part;
            parts.push_back(acc);
        }
        for (const auto& sub : parts) {
            if (!do_cpio_cmd(magiskboot, workdir, ramdisk, "mkdir 0755 " + sub)) {
                LOGE("Failed to mkdir %s in ramdisk", sub.c_str());
                cleanup();
                return 1;
            }
        }
    }
    for (const auto& name : {"tamisu.ko", "modules.load", "modules.dep"}) {
        const std::string local = mod_dir_rel + "/" + name;
        const std::string cpio_path = mod_dir + "/" + name;
        if (!do_cpio_cmd(magiskboot, workdir, ramdisk,
                         std::string("add 0644 ") + cpio_path + " " + local)) {
            LOGE("Failed to add %s to ramdisk", cpio_path.c_str());
            cleanup();
            return 1;
        }
    }
    printf("- Injected tamisu.ko into /%s/ (merge=%s)\n", mod_dir.c_str(),
           created_private_dir ? "no" : "yes");

    if (parsed.enable_adbd || !parsed.adb_debug_prop.empty()) {
        printf("- Adding adb debug props\n");
        std::ofstream(workdir + "/force_debuggable").close();
        if (!do_cpio_cmd(magiskboot, workdir, ramdisk,
                         "add 0644 force_debuggable force_debuggable")) {
            cleanup();
            return 1;
        }

        std::ofstream prop_file(workdir + "/adb_debug.prop");
        if (!prop_file.is_open()) {
            LOGE("Failed to create adb_debug.prop");
            cleanup();
            return 1;
        }
        if (parsed.enable_adbd) {
            printf("- Enabling adbd debug props\n");
            prop_file << "ro.debuggable=1\n";
            prop_file << "ro.force.debuggable=1\n";
            prop_file << "ro.adb.secure=0\n";
        }
        if (!parsed.adb_debug_prop.empty()) {
            printf("- Appending custom adb props\n");
            prop_file << parsed.adb_debug_prop;
            if (parsed.adb_debug_prop.back() != '\n') {
                prop_file << '\n';
            }
        }
        prop_file.close();
        if (!do_cpio_cmd(magiskboot, workdir, ramdisk, "add 0644 adb_debug.prop adb_debug.prop")) {
            cleanup();
            return 1;
        }
    } else {
        auto force_debuggable_exists = exec_command_magiskboot(
            magiskboot, {"cpio", ramdisk, "exists force_debuggable"}, workdir);
        if (force_debuggable_exists.exit_code == 0) {
            printf("- Removing /force_debuggable\n");
            do_cpio_cmd(magiskboot, workdir, ramdisk, "rm force_debuggable");
        }

        auto adb_debug_prop_exists = exec_command_magiskboot(
            magiskboot, {"cpio", ramdisk, "exists adb_debug.prop"}, workdir);
        if (adb_debug_prop_exists.exit_code == 0) {
            printf("- Removing /adb_debug.prop\n");
            do_cpio_cmd(magiskboot, workdir, ramdisk, "rm adb_debug.prop");
        }
    }

    // Backup if flashing and not already patched
    if (!already_patched && parsed.flash) {
        if (!do_backup(magiskboot, workdir, ramdisk, bootimage)) {
            printf("- Warning: Backup stock image failed\n");
        }
    }

    // Repack ramdisk with kernel-version-specific mkbootfs (fixes 5.15+ boot on some devices)
    if (!parsed.mkbootfs_dir.empty()) {
        if (repack_ramdisk_with_mkbootfs(magiskboot, parsed.mkbootfs_dir, workdir, ramdisk)) {
            // Success
        } else {
            LOGW("mkbootfs repack failed, continuing with magiskboot cpio format");
        }
    }

    // Repack boot image (must run in workdir where unpack output files are)
    // Pass explicit output path for compatibility with older magiskboot variants
    // that require: repack <in-boot.img> <out-boot.img>.
    // Output filename must match input format: boot -> new-boot.img, init_boot -> new-init_boot.img
    // (aligned with Magisk payload extract: partition.partition_name -> {partition}.img)
    const bool is_init_boot =
        (patch_file && bootimage.find("init_boot") != std::string::npos) ||
        (!bootdevice.empty() && bootdevice.find("init_boot") != std::string::npos);
    const std::string new_boot =
        workdir + "/" + (is_init_boot ? "new-init_boot.img" : "new-boot.img");
    printf("- Repacking boot image\n");
    fflush(stdout);
    // MagiskbootAlone supports LZ4_LEGACY, GZIP, and LZ4 compression natively.
    // Do NOT use --skip-comp: the ramdisk must be recompressed to fit the
    // partition. Without recompression the uncompressed ramdisk makes the output
    // image far larger than the original, exhausting cache space and overflowing
    // the boot partition.
    auto repack_result =
        exec_command_magiskboot(magiskboot, {"repack", bootimage, new_boot}, workdir);
    if (repack_result.exit_code != 0) {
        LOGE("magiskboot repack failed (exit code %d)", repack_result.exit_code);
        if (!repack_result.stdout_str.empty()) {
            LOGE("magiskboot repack stdout: %s", repack_result.stdout_str.c_str());
        }
        if (!repack_result.stderr_str.empty()) {
            LOGE("magiskboot repack stderr: %s", repack_result.stderr_str.c_str());
        }
        cleanup();
        return 1;
    }
    if (!fs::exists(new_boot)) {
        LOGE("magiskboot repack reported success but output not found: %s", new_boot.c_str());
        LOGE("Current magiskboot may be an incomplete build (unpack/repack stubs).");
        LOGE("Please use a full magiskboot implementation from upstream Magisk.");
        cleanup();
        return 1;
    }

    // Output patched image
    if (patch_file) {
        const std::string output_dir = parsed.out.empty() ? "." : parsed.out;
        std::string name = parsed.out_name;
        if (name.empty()) {
            const time_t now = time(nullptr);
            struct tm* tm_info = localtime(&now);
            std::array<char, 32> time_str{};
            (void)strftime(time_str.data(), time_str.size(), "%Y%m%d_%H%M%S", tm_info);
            name = std::string("tamisu_patched_") + time_str.data() + ".img";
        }

        const std::string output_image = output_dir + "/" + name;

        std::ifstream src(new_boot, std::ios::binary);  // NOLINT(misc-const-correctness)
        std::ofstream dst(output_image, std::ios::binary);
        if (!src || !dst) {
            LOGE("Failed to write output file");
            cleanup();
            return 1;
        }
        dst << src.rdbuf();

        printf("- Output file is written to\n");
        printf("- %s\n", output_image.c_str());
    }

    // Flash if requested
    if (parsed.flash && !bootdevice.empty()) {
        printf("- Flashing new boot image\n");
        if (!flash_boot(bootdevice, new_boot)) {
            LOGE("Failed to flash boot image");
            cleanup();
            return 1;
        }
    }

    cleanup();

    printf("- Done!\n");
    return 0;
}

// Parse boot restore arguments
struct BootRestoreArgs {
    std::string boot_image;  // -b, --boot
    bool flash = false;      // -f, --flash
    std::string magiskboot;  // --magiskboot
    std::string out_name;    // --out-name
};

BootRestoreArgs parse_boot_restore_args(const std::vector<std::string>& args) {
    BootRestoreArgs result;

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];

        if (arg == "-b" || arg == "--boot") {
            if (i + 1 < args.size())
                result.boot_image = args[++i];
        } else if (arg == "-f" || arg == "--flash") {
            result.flash = true;
        } else if (arg == "--magiskboot") {
            if (i + 1 < args.size())
                result.magiskboot = args[++i];
        } else if (arg == "--out-name") {
            if (i + 1 < args.size())
                result.out_name = args[++i];
        }
    }

    return result;
}

}  // namespace

int boot_patch(const std::vector<std::string>& args) {
    return boot_patch_impl(args);
}

int boot_restore(const std::vector<std::string>& args) {
    auto parsed = parse_boot_restore_args(args);

    // Create temp working directory
    std::array<char, 32> tmpdir_buf{};
    (void)strncpy(tmpdir_buf.data(), "/data/local/tmp/Tamisu_XXXXXX", tmpdir_buf.size() - 1);
    tmpdir_buf[tmpdir_buf.size() - 1] = '\0';
    char* tmpdir = mkdtemp(tmpdir_buf.data());
    if (!tmpdir) {
        LOGE("Failed to create temp directory");
        return 1;
    }
    std::string workdir = tmpdir;

    auto cleanup = [&workdir]() {
        const std::string cmd = "rm -rf " + workdir;
        system(cmd.c_str());
    };

    // Find magiskboot
    const std::string magiskboot = find_magiskboot(parsed.magiskboot, workdir);
    if (magiskboot.empty()) {
        cleanup();
        return 1;
    }

    // Get KMI for partition detection
    const std::string kmi = get_current_kmi();

    // Kernel release for removing the modprobe layout injected by boot-patch.
    std::string kernel_release = read_kernel_release_from_sysfs();
    if (kernel_release.empty()) {
        struct utsname uts{};
        if (uname(&uts) == 0) {
            kernel_release = uts.release;
        }
    }

    // Determine boot image path
    std::string bootimage;
    std::string bootdevice;

    if (!parsed.boot_image.empty()) {
        bootimage = parsed.boot_image;
        if (access(bootimage.c_str(), R_OK) != 0) {
            LOGE("Boot image not found: %s", bootimage.c_str());
            cleanup();
            return 1;
        }
    } else {
        // Auto-detect boot partition (restore doesn't replace kernel)
        const std::string partition_name = choose_boot_partition(kmi, false, nullptr, false);
        printf("- Bootdevice: %s\n", partition_name.c_str());

        bootimage = workdir + "/boot.img";
        if (!exec_dd(partition_name, bootimage)) {
            LOGE("Failed to read boot image");
            cleanup();
            return 1;
        }
        bootdevice = partition_name;
    }

    // Unpack boot image (must run in workdir so output files go there)
    printf("- Unpacking boot image\n");
    std::vector<std::string> unpack_args_restore = {"unpack",
                                                    bootimage};  // NOLINT(misc-const-correctness)
    auto unpack_result = exec_command_magiskboot(magiskboot, unpack_args_restore, workdir);
#ifdef __ANDROID__
    constexpr int SIGSEGV_EXIT_R = 128 + 11;  // 139
    if (unpack_result.exit_code == SIGSEGV_EXIT_R) {
        printf("- Unpack crashed (SIGSEGV); retrying with --skip-decomp.\n");
        unpack_args_restore.push_back("--skip-decomp");
        unpack_result = exec_command_magiskboot(magiskboot, unpack_args_restore, workdir);
    }
    if (unpack_result.exit_code == 0) {
        constexpr unsigned char LZ4_LEG_MAGIC[] = {0x02, 0x21, 0x4c, 0x18};
        std::string rd_cpio = workdir + "/ramdisk.cpio";
        std::ifstream rd_in(rd_cpio, std::ios::binary);
        unsigned char mb[4];
        if (rd_in && rd_in.read(reinterpret_cast<char*>(mb), 4) && rd_in.gcount() == 4 &&
            memcmp(mb, LZ4_LEG_MAGIC, 4) == 0) {
            rd_in.close();
            LOGE("Ramdisk is LZ4_LEGACY; on-device decompress crashed (SIGSEGV), so it was "
                 "skipped.");
            LOGE("Restore or patch the boot image on a PC instead.");
            cleanup();
            return 1;
        }
    }
#endif  // #ifdef __ANDROID__
    if (unpack_result.exit_code != 0) {
        LOGE("magiskboot unpack failed");
        if (!unpack_result.stderr_str.empty()) {
            LOGE("stderr: %s", unpack_result.stderr_str.c_str());
        }
        cleanup();
        return 1;
    }

    // Find ramdisk
    std::string ramdisk;
    const std::vector<std::string> ramdisk_candidates = {workdir + "/ramdisk.cpio",
                                                         workdir + "/vendor_ramdisk/init_boot.cpio",
                                                         workdir + "/vendor_ramdisk/ramdisk.cpio"};

    for (const auto& candidate : ramdisk_candidates) {
        if (access(candidate.c_str(), R_OK) == 0) {
            ramdisk = candidate;
            break;
        }
    }

    if (ramdisk.empty()) {
        LOGE("No compatible ramdisk found");
        cleanup();
        return 1;
    }

    // Check if patched by Tamisu
    if (!is_tamisu_patched(magiskboot, workdir, ramdisk)) {
        LOGE("Boot image is not patched by Tamisu");
        cleanup();
        return 1;
    }

    std::string new_boot;
    bool from_backup = false;

    // Try to find backup
    auto backup_exists = exec_command_magiskboot(
        magiskboot, {"cpio", ramdisk, "exists " + std::string(BACKUP_FILENAME)}, workdir);
    if (backup_exists.exit_code == 0) {
        // Extract backup sha1
        const std::string backup_file = workdir + "/" + BACKUP_FILENAME;
        exec_command_magiskboot(
            magiskboot,
            {"cpio", ramdisk, "extract " + std::string(BACKUP_FILENAME) + " " + backup_file},
            workdir);

        auto sha_content = read_file(backup_file);
        if (sha_content) {
            const std::string sha = trim(*sha_content);
            const std::string backup_path =
                std::string(TAMISU_BACKUP_DIR) + TAMISU_BACKUP_FILE_PREFIX + sha;

            if (access(backup_path.c_str(), R_OK) == 0) {
                new_boot = backup_path;
                from_backup = true;
                clean_backup(sha);
            } else {
                printf("- Warning: no backup %s found!\n", backup_path.c_str());
            }
        }
    } else {
        printf("- Backup info is absent!\n");
    }

    // If no backup, manually remove Tamisu (do NOT wipe vendor modules)
    if (!from_backup) {
        std::error_code restore_ec;

        // Remove legacy flat tamisu.ko
        do_cpio_cmd(magiskboot, workdir, ramdisk, "rm tamisu.ko");

        // Strip tamisu.ko from every lib/modules/* that contains it, and restore
        // modules.load / modules.dep by removing only our lines. Never delete the
        // whole modules tree — stock ramdisks may share that directory with vendor.
        auto mod_entries =
            cpio_list_entries(magiskboot, workdir, ramdisk, "lib/modules", true);
        std::vector<std::string> mod_dirs;
        for (const auto& e : mod_entries) {
            if (e == "tamisu.ko" || ends_with(e, "/tamisu.ko")) {
                do_cpio_cmd(magiskboot, workdir, ramdisk, "rm " + e);
                auto slash = e.rfind('/');
                if (slash != std::string::npos) {
                    mod_dirs.push_back(e.substr(0, slash));
                }
            }
        }
        // Also consider uname-r dir even if listing missed it
        if (!kernel_release.empty()) {
            mod_dirs.push_back("lib/modules/" + kernel_release);
        }
        std::sort(mod_dirs.begin(), mod_dirs.end());
        mod_dirs.erase(std::unique(mod_dirs.begin(), mod_dirs.end()), mod_dirs.end());

        fs::create_directories(workdir + "/modroot_restore", restore_ec);

        for (const auto& dir : mod_dirs) {
            const std::string load_entry = dir + "/modules.load";
            const std::string dep_entry = dir + "/modules.dep";

            if (cpio_entry_exists(magiskboot, workdir, ramdisk, load_entry)) {
                const std::string out_rel = "modroot_restore/modules.load";
                if (cpio_extract_entry(magiskboot, workdir, ramdisk, load_entry, out_rel)) {
                    auto content = read_file(workdir + "/" + out_rel);
                    if (content) {
                        auto stripped = strip_tamisu_from_modules_load(*content);
                        if (stripped.empty()) {
                            do_cpio_cmd(magiskboot, workdir, ramdisk, "rm " + load_entry);
                        } else if (write_file(workdir + "/" + out_rel, stripped)) {
                            do_cpio_cmd(magiskboot, workdir, ramdisk,
                                        "add 0644 " + load_entry + " " + out_rel);
                        }
                    }
                }
            }

            if (cpio_entry_exists(magiskboot, workdir, ramdisk, dep_entry)) {
                const std::string out_rel = "modroot_restore/modules.dep";
                if (cpio_extract_entry(magiskboot, workdir, ramdisk, dep_entry, out_rel)) {
                    auto content = read_file(workdir + "/" + out_rel);
                    if (content) {
                        auto stripped = strip_tamisu_from_modules_dep(*content);
                        if (stripped.empty()) {
                            do_cpio_cmd(magiskboot, workdir, ramdisk, "rm " + dep_entry);
                        } else if (write_file(workdir + "/" + out_rel, stripped)) {
                            do_cpio_cmd(magiskboot, workdir, ramdisk,
                                        "add 0644 " + dep_entry + " " + out_rel);
                        }
                    }
                }
            }
        }

        // Remove kasumi.ko if present (experimental cpio embed)
        auto kasumi_exists =
            exec_command_magiskboot(magiskboot, {"cpio", ramdisk, "exists kasumi.ko"}, workdir);
        if (kasumi_exists.exit_code == 0) {
            do_cpio_cmd(magiskboot, workdir, ramdisk, "rm kasumi.ko");
        }

        // Restore init if init.real exists
        auto init_real_exists =
            exec_command_magiskboot(magiskboot, {"cpio", ramdisk, "exists init.real"}, workdir);
        if (init_real_exists.exit_code == 0) {
            do_cpio_cmd(magiskboot, workdir, ramdisk, "mv init.real init");
        }

        // Repack (must run in workdir where unpack output files are)
        // Output filename must match input format: boot -> new-boot.img, init_boot ->
        // new-init_boot.img
        const bool is_init_boot_restore =
            (!parsed.boot_image.empty() &&
             parsed.boot_image.find("init_boot") != std::string::npos) ||
            (!bootdevice.empty() && bootdevice.find("init_boot") != std::string::npos);
        const std::string out_img =
            workdir + "/" + (is_init_boot_restore ? "new-init_boot.img" : "new-boot.img");
        printf("- Repacking boot image\n");
        auto repack_result =
            exec_command_magiskboot(magiskboot, {"repack", bootimage, out_img}, workdir);
        if (repack_result.exit_code != 0) {
            LOGE("magiskboot repack failed");
            cleanup();
            return 1;
        }
        new_boot = out_img;
    }

    // Output restored image
    if (!parsed.boot_image.empty()) {
        std::string name = parsed.out_name;
        if (name.empty()) {
            const time_t now = time(nullptr);
            struct tm* tm_info = localtime(&now);
            std::array<char, 32> time_str{};
            (void)strftime(time_str.data(), time_str.size(), "%Y%m%d_%H%M%S", tm_info);
            name = std::string("tamisu_restore_") + time_str.data() + ".img";
        }

        const std::string output_image = "./" + name;

        std::ifstream src(new_boot, std::ios::binary);  // NOLINT(misc-const-correctness)
        std::ofstream dst(output_image, std::ios::binary);
        if (!src || !dst) {
            LOGE("Failed to write output file");
            cleanup();
            return 1;
        }
        dst << src.rdbuf();

        printf("- Output file is written to\n");
        printf("- %s\n", output_image.c_str());
    }

    // Flash if requested
    if (parsed.flash && !bootdevice.empty()) {
        if (from_backup) {
            printf("- Flashing new boot image from %s\n", new_boot.c_str());
        } else {
            printf("- Flashing new boot image\n");
        }
        if (!flash_boot(bootdevice, new_boot)) {
            LOGE("Failed to flash boot image");
            cleanup();
            return 1;
        }
    }

    cleanup();
    printf("- Done!\n");
    return 0;
}

// Read real kernel release from sysfs. Not spoofed by Kasumi uname hiding (uname(2) is).
// Prefer this for KMI so LKM selection uses the actual kernel, not spoofed version.
std::string read_kernel_release_from_sysfs() {
    std::ifstream f("/proc/sys/core/osrelease");
    std::string line;
    if (std::getline(f, line))
        return line;
    return "";
}

std::string get_current_kmi() {
    std::string full_version = read_kernel_release_from_sysfs();
    if (full_version.empty()) {
        struct utsname uts{};
        if (uname(&uts) != 0) {
            LOGE("Failed to get uname");
            return "";
        }
        full_version = uts.release;  // e.g. "6.6.66-android15-8-g29d86c5fc9dd"
    }

    // Extract major.minor (e.g. "6.6")
    const size_t dot1 = full_version.find('.');
    if (dot1 == std::string::npos)
        return "";
    size_t dot2 = full_version.find('.', dot1 + 1);
    if (dot2 == std::string::npos)
        dot2 = full_version.length();

    std::string major_minor = full_version.substr(0, dot2);

    // Try to find android version (e.g. "android15")
    const size_t android_pos = full_version.find("-android");
    if (android_pos != std::string::npos) {
        const size_t ver_start = android_pos + 8;
        size_t ver_end = full_version.find('-', ver_start);
        if (ver_end == std::string::npos)
            ver_end = full_version.length();

        const std::string android_ver = full_version.substr(ver_start, ver_end - ver_start);
        return "android" + android_ver + "-" + major_minor;
    }

    // Fallback: kernel release has no -androidXX suffix (self-built kernels,
    // non-GKI builds, etc.). Walk the embedded asset list and pick the KMI
    // whose major.minor matches. If multiple match (e.g. android13-6.1 and
    // android14-6.1 both exist), prefer the highest android version, since
    // KMI symbols only get added, not removed, across android releases on
    // the same kernel LTS branch.
    const auto supported = list_supported_kmi();
    std::string best;
    int best_android = -1;
    for (const auto& kmi : supported) {
        // KMI format: androidXX-X.Y
        const size_t dash = kmi.find('-');
        if (dash == std::string::npos) continue;
        const std::string kmi_minor = kmi.substr(dash + 1);
        if (kmi_minor != major_minor) continue;

        const std::string android_part = kmi.substr(0, dash);
        if (android_part.rfind("android", 0) != 0) continue;
        const std::string num_str = android_part.substr(7);
        int android_ver = 0;
        try {
            android_ver = std::stoi(num_str);
        } catch (...) {
            continue;
        }
        if (android_ver > best_android) {
            best_android = android_ver;
            best = kmi;
        }
    }
    if (!best.empty()) {
        LOGI("KMI fallback: matched %s from asset list (major.minor=%s)",
             best.c_str(), major_minor.c_str());
        return best;
    }

    return major_minor;
}

int boot_info_current_kmi() {
    const std::string kmi = get_current_kmi();
    if (kmi.empty()) {
        printf("Failed to get current KMI\n");
        return 1;
    }
    printf("%s\n", kmi.c_str());
    return 0;
}

int boot_info_supported_kmis() {
    auto supported = list_supported_kmi();
    if (supported.empty()) {
        printf("No embedded LKMs found\n");
        return 1;
    }
    for (const auto& kmi : supported) {
        printf("%s\n", kmi.c_str());
    }
    return 0;
}

int boot_info_is_ab_device() {
    auto ab_update = getprop("ro.build.ab_update");
    const bool is_ab = ab_update && trim(*ab_update) == "true";
    printf("%s\n", is_ab ? "true" : "false");
    return 0;
}

std::string get_slot_suffix(bool ota) {
    auto suffix = getprop("ro.boot.slot_suffix");
    if (!suffix || suffix->empty()) {
        return "";
    }

    if (ota) {
        // Toggle to other slot
        if (*suffix == "_a")
            return "_b";
        if (*suffix == "_b")
            return "_a";
    }

    return *suffix;
}

int boot_info_slot_suffix(bool ota) {
    const std::string suffix = get_slot_suffix(ota);
    printf("%s\n", suffix.c_str());
    return 0;
}

std::string choose_boot_partition(const std::string& kmi, bool ota,
                                  const std::string* override_partition, bool is_replace_kernel) {
    // If specific partition is specified, use it
    if (override_partition && !override_partition->empty()) {
        // Validate partition name
        if (*override_partition == "boot" || *override_partition == "init_boot" ||
            *override_partition == "vendor_boot") {
            const std::string slot = get_slot_suffix(ota);
            const std::string block_dev =
                flash::find_partition_block_device(*override_partition, slot);
            if (!block_dev.empty()) {
                return block_dev;
            }
            return "/dev/block/by-name/" + *override_partition + slot;
        }
        // Invalid partition name, fallback to auto-detect
    }

    const std::string slot = get_slot_suffix(ota);

    // Android 12 GKI doesn't have init_boot
    const bool skip_init_boot = kmi.find("android12-") == 0;

    // Check if init_boot exists
    const std::string init_boot = flash::find_partition_block_device("init_boot", slot);
    const bool init_boot_exist = !init_boot.empty();

    // Use init_boot if:
    // - Not replacing kernel (LKM mode)
    // - init_boot partition exists
    // - Not android12 (which doesn't have init_boot)
    if (!is_replace_kernel && init_boot_exist && !skip_init_boot) {
        return init_boot;
    }

    // Fallback to boot
    const std::string boot = flash::find_partition_block_device("boot", slot);
    if (!boot.empty()) {
        return boot;
    }
    return "/dev/block/by-name/boot" + slot;
}

// Return partition name only (without path and slot suffix)
// Used by boot-info default-partition command for manager
std::string get_default_partition_name(const std::string& kmi, bool is_replace_kernel) {
    const std::string slot = get_slot_suffix(false);

    // Android 12 GKI doesn't have init_boot
    const bool skip_init_boot = kmi.find("android12-") == 0;

    // Check if init_boot exists
    const bool init_boot_exist = !flash::find_partition_block_device("init_boot", slot).empty();

    // Use init_boot if:
    // - Not replacing kernel (LKM mode)
    // - init_boot partition exists
    // - Not android12 (which doesn't have init_boot)
    if (!is_replace_kernel && init_boot_exist && !skip_init_boot) {
        return "init_boot";
    }

    return "boot";
}

int boot_info_default_partition() {
    const std::string kmi = get_current_kmi();
    // Return partition name only, not full path (matching Rust behavior)
    const std::string partition = get_default_partition_name(kmi, false);
    printf("%s\n", partition.c_str());
    return 0;
}

int boot_info_available_partitions() {
    // Return base partition names (without slot suffix) like Rust version
    // Manager will add slot suffix based on user's choice
    const std::string slot = get_slot_suffix(false);

    const std::array<const char*, 3> candidates = {"boot", "init_boot", "vendor_boot"};

    for (const char* name : candidates) {
        if (!flash::find_partition_block_device(name, slot).empty()) {
            printf("%s\n", name);
        }
    }

    return 0;
}

}  // namespace tamisu_daemon
