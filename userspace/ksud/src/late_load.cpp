#include "late_load.hpp"

#include "assets.hpp"
#include "boot/boot_patch.hpp"
#include "core/feature.hpp"
#include "core/restorecon.hpp"
#include "defs.hpp"
#include "init_event.hpp"
#include "kernelsu_loader.hpp"
#include "log.hpp"
#include "module/metamodule.hpp"
#include "module/module.hpp"
#include "module/module_config.hpp"
#include "utils.hpp"

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace ksud::late_load {

namespace {

constexpr const char* kManagerPackage = "me.dabao1955.tamisu";
constexpr const char* kLateLoadTmpCandidates[] = {
    "/dev/kernelsu_XXXXXX",
    "/data/local/tmp/kernelsu_XXXXXX",
    "/data/adb/kernelsu_XXXXXX",
};

bool is_kernelsu_loaded() {
    // The kernel module is named "tamisu" (see kernel/Kbuild:
    // obj-$(CONFIG_TAMISU) += tamisu.o), so once loaded it appears as
    // /sys/module/tamisu. We check this name rather than "kernelsu" so
    // detection works regardless of whether upstream KernelSU is also
    // loaded on the same kernel.
    return access("/sys/module/tamisu", F_OK) == 0;
}

std::string get_kernelsu_load_params(bool allow_shell) {
    // allow_shell is the module_param consumed directly by tamisu.ko via the
    // insmod/modprobe cmdline (e.g. "allow_shell=1"). The legacy
    // /ksu_allow_shell ramdisk marker file is an upstream KernelSU convention
    // we do not implement, so we only honour the explicit parameter here.
    if (allow_shell) {
        LOGW("late-load: loading tamisu.ko with allow_shell=1");
        return "allow_shell=1";
    }

    return "";
}

bool extract_and_load_kernelsu(bool allow_shell) {
    const std::string kmi = get_current_kmi();
    if (kmi.empty()) {
        LOGE("late-load: failed to detect current KMI");
        return false;
    }

    const std::string asset_name = kmi + "_kernelsu.ko";
    std::array<char, PATH_MAX> tmp_path{};
    int tmp_fd = -1;
    for (const char* candidate : kLateLoadTmpCandidates) {
        std::snprintf(tmp_path.data(), tmp_path.size(), "%s", candidate);
        tmp_fd = mkstemp(tmp_path.data());
        if (tmp_fd >= 0) {
            LOGI("late-load: using temp module path %s", tmp_path.data());
            break;
        }
        LOGW("late-load: mkstemp failed at %s: %s", candidate, strerror(errno));
    }
    if (tmp_fd < 0) {
        LOGE("late-load: failed to allocate temp module file");
        return false;
    }
    close(tmp_fd);

    const bool copied = copy_asset_to_file(asset_name, tmp_path.data());
    if (!copied) {
        LOGE("late-load: no embedded module matches %s", asset_name.c_str());
        unlink(tmp_path.data());
        return false;
    }

    LOGI("late-load: loading %s via relocated init_module", asset_name.c_str());
    const bool loaded =
        ksud::kernelsu_loader::load_module(tmp_path.data(), get_kernelsu_load_params(allow_shell));
    unlink(tmp_path.data());
    return loaded;
}

void run_stage_scripts(const std::string& stage, bool block) {
    if (has_magisk()) {
        LOGW("Magisk detected, skip %s", stage.c_str());
        return;
    }

    if (is_safe_mode()) {
        LOGW("safe mode, skip %s scripts", stage.c_str());
        return;
    }

    exec_common_scripts(stage + ".d", block);
    metamodule_exec_stage_script(stage, block);
    exec_stage_script(stage, block);
}

void restart_manager() {
    const std::string activity = std::string(kManagerPackage) + "/.ui.MainActivity";
    (void)exec_command({"am", "force-stop", kManagerPackage});
    (void)exec_command({"am", "start", "-n", activity});
}

bool run_finalizer_command(const std::vector<std::string>& args, const char* prefix) {
    const auto result = exec_command(args);
    if (result.exit_code == 0) {
        return true;
    }

    LOGE("%s failed: %s", prefix, args.front().c_str());
    if (!result.stdout_str.empty()) {
        LOGE("stdout: %s", result.stdout_str.c_str());
    }
    if (!result.stderr_str.empty()) {
        LOGE("stderr: %s", result.stderr_str.c_str());
    }
    return false;
}

void run_post_late_load_cleanup() {
    LOGI("Running post-late-load finalization");

    if (!run_finalizer_command({"setenforce", "1"}, "late-load cleanup")) {
        LOGE("Failed to re-enable SELinux enforcing after late-load");
    }
}

}  // namespace

int run(bool post_magica, bool allow_shell) {
    (void)post_magica;
    LOGI("late-load command triggered");
    int result = 0;

    if (!is_kernelsu_loaded() && !extract_and_load_kernelsu(allow_shell)) {
        result = 1;
        goto finalize;
    }

    ksud::umask(0);

    clear_all_temp_configs();

    if (install(std::nullopt, std::nullopt) != 0) {
        LOGE("late-load: install() failed");
        result = 1;
        goto finalize;
    }

    if (handle_updated_modules() != 0) {
        LOGW("late-load: handle_updated_modules failed");
    }

    if (prune_modules() != 0) {
        LOGW("late-load: prune_modules failed");
    }

    if (!restorecon()) {
        LOGW("late-load: restorecon failed");
    }

    if (load_sepolicy_rule() != 0) {
        LOGW("late-load: load_sepolicy_rule failed");
    }

    if (init_features() != 0) {
        LOGW("late-load: init_features failed");
    }

    run_stage_scripts("late-load", true);

    if (load_system_prop() != 0) {
        LOGW("late-load: load_system_prop failed");
    }

    if (metamodule_exec_mount_script() != 0) {
        LOGW("late-load: metamodule_exec_mount_script failed");
    }

    run_stage_scripts("post-mount", true);
    on_services();
    on_boot_completed();

    restart_manager();

finalize:
    run_post_late_load_cleanup();

    return result;
}

}  // namespace ksud::late_load
