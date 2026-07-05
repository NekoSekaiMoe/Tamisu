#include "cli.h"
#include "assets.h"
#include "boot/boot_patch.h"
#include "core/feature.h"
#include "core/hide_bootloader.h"
#include "core/tamisuctl.h"
#include "core/restorecon.h"
#include "debug.h"
#include "defs.h"
#include "flash/flash_partition.h"
#include "init_event.h"
#include "late_load.h"
#include "log.h"
#include "module/module.h"
#include "sepolicy/sepolicy.h"
#include "utils.h"

extern "C" {
#include "uapi/yukizygisk.h"
}

#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace tamisu_daemon {

void CliParser::add_option(const CliOption& opt) {
    options_.push_back(opt);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool CliParser::parse(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg.empty())
            continue;

        if (!subcommand_.empty()) {
            positional_args_.push_back(arg);
            continue;
        }

        // Check if it's an option
        if (arg[0] == '-') {
            bool found = false;  // NOLINT(misc-const-correctness) assigned in loop
            std::string opt_name;
            std::string opt_value;

            // Long option
            if (arg.size() > 1 && arg[1] == '-') {
                const std::string long_opt = arg.substr(2);
                const size_t eq_pos = long_opt.find('=');
                if (eq_pos != std::string::npos) {
                    opt_name = long_opt.substr(0, eq_pos);
                    opt_value = long_opt.substr(eq_pos + 1);
                } else {
                    opt_name = long_opt;
                }

                for (const auto& opt : options_) {
                    if (opt.long_name == opt_name) {
                        found = true;
                        if (opt.takes_value && opt_value.empty() && i + 1 < argc) {
                            opt_value = argv[++i];
                        }
                        parsed_options_[opt_name] = opt_value.empty() ? "true" : opt_value;
                        break;
                    }
                }
            }
            // Short option
            else {
                const char short_opt = arg[1];
                for (const auto& opt : options_) {
                    if (opt.short_name == short_opt) {
                        found = true;
                        opt_name = opt.long_name;
                        if (opt.takes_value && i + 1 < argc) {
                            opt_value = argv[++i];
                        }
                        parsed_options_[opt_name] = opt_value.empty() ? "true" : opt_value;
                        break;
                    }
                }
            }

            if (!found) {
                LOGE("Unknown option: %s", arg.c_str());
            }
        }
        // Positional argument
        else {
            if (subcommand_.empty()) {
                subcommand_ = arg;
            } else {
                positional_args_.push_back(arg);
            }
        }
    }

    return true;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::optional<std::string> CliParser::get_option(const std::string& name) const {
    auto it = parsed_options_.find(name);
    if (it != parsed_options_.end()) {
        return it->second;
    }

    // Return default value if exists
    for (const auto& opt : options_) {
        if (opt.long_name == name && !opt.default_value.empty()) {
            return opt.default_value;
        }
    }

    return std::nullopt;
}

bool CliParser::has_option(const std::string& name) const {
    return parsed_options_.find(name) != parsed_options_.end();
}

namespace {

void print_usage() {
    printf("Tamisu userspace daemon (zygisk-only)\n\n");
    printf("USAGE: tamisu_daemon <COMMAND>\n\n");
    printf("COMMANDS:\n");
    printf("  insmod         Load a kernel module with kallsyms access\n");
    printf("  late-load      Load tamisu.ko and execute late-load stage scripts\n");
    printf("  post-fs-data   Trigger post-fs-data event\n");
    printf("  services       Trigger service event\n");
    printf("  boot-completed Trigger boot-complete event\n");
    printf("  install        Install Tamisu userspace\n");
    printf("  uninstall      Uninstall Tamisu\n");
    printf("  sepolicy       SELinux policy patch tool\n");
    printf("  feature        Manage kernel features\n");
    printf("  initrc         Manage init.rc injection\n");
    printf("  boot-patch     Patch boot image\n");
    printf("  boot-restore   Restore boot image\n");
    printf("  boot-info      Show boot information\n");
    printf("  flash          Flash partition images\n");
    printf("  kernel         Kernel interface\n");
    printf("  debug          For developers\n");
    printf("  help           Show this help\n");
    printf("  version        Show version\n");
}

void print_version() {
    printf("tamisu_daemon version %s (code: %s)\n", VERSION_NAME, VERSION_CODE);
}

int cmd_initrc(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: tamisu_daemon initrc <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  refresh        Regenerate preinit modules.rc\n");
        return 1;
    }

    const std::string& subcmd = args[0];
    if (subcmd == "refresh") {
        return regenerate_preinit_rc();
    }

    printf("Unknown initrc subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_yukizygisk(const std::vector<std::string>& args) {
    if (args.empty() || args[0] != "reload") {
        printf("Usage: tamisu_daemon yukizygisk reload\n");
        return 1;
    }
    // Fires TAMISU_IOCTL_YZ_RELOAD -> kernel multicasts YZ_EV_RELOAD -> zygiskd
    // re-reads yzconfig.json. Applies on the next specialize, no reboot.
    const int rc = tamisu_daemon::tamisuctl(TAMISU_IOCTL_YZ_RELOAD, nullptr);
    printf(rc == 0 ? "yzconfig reload signalled\n" : "yzconfig reload failed\n");
    return rc == 0 ? 0 : 1;
}

int cmd_feature(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: tamisu_daemon feature <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  get <ID>        Get feature value\n");
        printf("  set <ID> <VAL>  Set feature value\n");
        printf("  list            List all features\n");
        printf("  check <ID>      Check feature status\n");
        printf("  load            Load config from file\n");
        printf("  save            Save config to file\n");
        printf("  hide-bl         Show bootloader hiding status\n");
        printf("  hide-bl enable  Enable bootloader hiding\n");
        printf("  hide-bl disable Disable bootloader hiding\n");
        printf("  hide-bl run     Run bootloader hiding now\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "get" && args.size() > 1) {
        return feature_get(args[1]);
    } else if (subcmd == "set" && args.size() > 2) {
        return feature_set(args[1], std::stoull(args[2]));
    } else if (subcmd == "list") {
        feature_list();
        return 0;
    } else if (subcmd == "check" && args.size() > 1) {
        return feature_check(args[1]);
    } else if (subcmd == "load") {
        return feature_load_config();
    } else if (subcmd == "save") {
        return feature_save_config();
    } else if (subcmd == "hide-bl") {
        // Bootloader hiding subcommand
        if (args.size() > 1) {
            const std::string& action = args[1];
            if (action == "enable") {
                set_bl_hiding_enabled(true);
                printf("Bootloader hiding enabled. Will take effect on next boot.\n");
                return 0;
            } else if (action == "disable") {
                set_bl_hiding_enabled(false);
                printf("Bootloader hiding disabled.\n");
                return 0;
            } else if (action == "run") {
                hide_bootloader_status();
                printf("Bootloader hiding executed.\n");
                return 0;
            }
        }
        // Show status
        const bool enabled = is_bl_hiding_enabled();
        printf("Bootloader hiding: %s\n", enabled ? "enabled" : "disabled");
        return 0;
    }

    printf("Unknown feature subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_debug(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: tamisu_daemon debug <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  insmod <KO> [PARAMS...]  Load a kernel module (legacy alias)\n");
        printf("  version            Get kernel version\n");
        printf("  mark <get|mark|unmark|refresh> [PID]\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "insmod" && args.size() > 1) {
        return debug_insmod(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    } else if (subcmd == "version") {
        printf("Kernel Version: %d\n", debug_get_kernel_version());
        return 0;
    } else if (subcmd == "mark" && args.size() > 1) {
        return debug_mark(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    printf("Unknown debug subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_insmod(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: tamisu_daemon insmod <KO> [PARAMS...]\n");
        return 1;
    }

    return debug_insmod(args[0], std::vector<std::string>(args.begin() + 1, args.end()));
}

int cmd_kernel(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: tamisu_daemon kernel <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  notify-module-mounted  Notify module mounted\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "notify-module-mounted") {
        report_module_mounted();
        return 0;
    }

    printf("Unknown kernel subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_sepolicy(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: tamisu_daemon sepolicy <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  patch <POLICY>   Patch sepolicy\n");
        printf("  apply <FILE>     Apply sepolicy from file\n");
        printf("  check <POLICY>   Check sepolicy\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "patch" && args.size() > 1) {
        return sepolicy_live_patch(args[1]);
    } else if (subcmd == "apply" && args.size() > 1) {
        return sepolicy_apply_file(args[1]);
    } else if (subcmd == "check" && args.size() > 1) {
        return sepolicy_check_rule(args[1]);
    }

    printf("Unknown sepolicy subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_boot_info(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("USAGE: tamisu_daemon boot-info <SUBCOMMAND>\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  current-kmi         Show current KMI\n");
        printf("  supported-kmis      Show supported KMIs\n");
        printf("  is-ab-device        Check A/B device\n");
        printf("  default-partition   Show default partition\n");
        printf("  available-partitions List partitions\n");
        printf("  slot-suffix [-u]    Show slot suffix\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "current-kmi") {
        return boot_info_current_kmi();
    } else if (subcmd == "supported-kmis") {
        return boot_info_supported_kmis();
    } else if (subcmd == "is-ab-device") {
        return boot_info_is_ab_device();
    } else if (subcmd == "default-partition") {
        return boot_info_default_partition();
    } else if (subcmd == "available-partitions") {
        return boot_info_available_partitions();
    } else if (subcmd == "slot-suffix") {
        const bool ota = args.size() > 1 && (args[1] == "-u" || args[1] == "--ota");
        return boot_info_slot_suffix(ota);
    }

    printf("Unknown boot-info subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_flash_new(const std::vector<std::string>& args) {
    using namespace flash;

    if (args.empty()) {
        printf("USAGE: tamisu_daemon flash <SUBCOMMAND> [OPTIONS]\n\n");
        printf("SUBCOMMANDS:\n");
        printf("  image <IMAGE> <PARTITION>  Flash image to partition\n");
        printf("  backup <PARTITION> <OUT>   Backup partition to file\n");
        printf("  list [--slot SLOT] [--all] List available partitions\n");
        printf("  info <PARTITION>           Show partition info\n");
        printf("  slots                      Show slot information (A/B devices)\n");
        printf("  map <SLOT>                 Map logical partitions for inactive slot\n");
        printf("  avb                        Show AVB/dm-verity status\\n");
        printf("  avb disable                Disable AVB/dm-verity\\n");
        printf("  kernel [--slot SLOT]       Show kernel version\\n");
        printf("  boot-info                  Show boot slot information\\n");
        printf("\nOPTIONS:\n");
        printf("  --slot <a|b|_a|_b>         Target specific slot (for A/B devices)\n");
        printf("                             Default: current active slot\n");
        printf("  --all                      List all partitions (not just common ones)\n");
        printf("\nEXAMPLES:\n");
        printf("  tamisu_daemon flash image boot.img boot\n");
        printf("  tamisu_daemon flash image boot.img boot --slot _b\n");
        printf("  tamisu_daemon flash backup boot /sdcard/boot-backup.img --slot _a\n");
        printf("  tamisu_daemon flash list\n");
        printf("  tamisu_daemon flash list --all\n");
        printf("  tamisu_daemon flash slots\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    // Parse common options
    std::string target_slot;
    bool scan_all = false;
    std::vector<std::string> filtered_args;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--slot" && i + 1 < args.size()) {
            target_slot = args[++i];
            // Normalize slot format (_a, a -> _a)
            if (!target_slot.empty() && target_slot[0] != '_') {
                target_slot.insert(0, 1, '_');
            }
        } else if (args[i] == "--all") {
            scan_all = true;
        } else {
            filtered_args.push_back(args[i]);
        }
    }

    if (filtered_args[0] == "image" && filtered_args.size() >= 3) {
        const std::string image_path = filtered_args[1];
        const std::string partition = filtered_args[2];

        printf("Flashing %s to %s", image_path.c_str(), partition.c_str());
        if (!target_slot.empty()) {
            printf(" (slot: %s)", target_slot.c_str());
        }
        printf("...\n");

        if (tamisu_daemon::flash::flash_partition(image_path, partition, target_slot)) {
            printf("Flash successful!\n");
            return 0;
        } else {
            printf("Flash failed!\n");
            return 1;
        }

    } else if (filtered_args[0] == "backup" && filtered_args.size() >= 3) {
        const std::string partition = filtered_args[1];
        const std::string output = filtered_args[2];

        printf("Backing up %s to %s", partition.c_str(), output.c_str());
        if (!target_slot.empty()) {
            printf(" (slot: %s)", target_slot.c_str());
        }
        printf("...\n");

        if (tamisu_daemon::flash::backup_partition(partition, output, target_slot)) {
            printf("Backup successful!\n");
            return 0;
        } else {
            printf("Backup failed!\n");
            return 1;
        }

    } else if (filtered_args[0] == "list") {
        const std::string slot =
            target_slot.empty() ? tamisu_daemon::flash::get_current_slot_suffix() : target_slot;
        auto partitions = tamisu_daemon::flash::get_available_partitions(scan_all, slot);

        if (scan_all) {
            printf("All partitions");
        } else {
            printf("Common partitions");
        }
        if (tamisu_daemon::flash::is_ab_device() && !slot.empty()) {
            printf(" (slot: %s)", slot.c_str());
        }
        printf(":\n");

        for (const auto& p : partitions) {
            auto info = tamisu_daemon::flash::get_partition_info(p, slot);
            const char* type = info.is_logical ? "logical" : "physical";
            const char* marker = "";
            if (tamisu_daemon::flash::is_dangerous_partition(p)) {
                marker = " [DANGEROUS]";
            }
            printf("  %-20s [%s, %lu bytes]%s\n", p.c_str(), type, (unsigned long)info.size,
                   marker);
        }
        return 0;

    } else if (filtered_args[0] == "info" && filtered_args.size() >= 2) {
        const std::string partition = filtered_args[1];
        const std::string slot =
            target_slot.empty() ? tamisu_daemon::flash::get_current_slot_suffix() : target_slot;
        auto info = tamisu_daemon::flash::get_partition_info(partition, slot);

        if (!info.exists) {
            printf("Partition %s not found\n", partition.c_str());
            return 1;
        }

        printf("Partition: %s\n", info.name.c_str());
        printf("Block device: %s\n", info.block_device.c_str());
        printf("Type: %s\n", info.is_logical ? "logical" : "physical");
        printf("Size: %lu bytes (%.2f MB)\n", (unsigned long)info.size,
               info.size / 1024.0 / 1024.0);

        if (tamisu_daemon::flash::is_ab_device()) {
            printf("Slot: %s\n", slot.c_str());
        }
        return 0;

    } else if (filtered_args[0] == "slots") {
        // Show slot information for A/B devices
        if (!tamisu_daemon::flash::is_ab_device()) {
            printf("This device is not A/B partitioned\n");
            return 0;
        }

        const std::string current_slot = tamisu_daemon::flash::get_current_slot_suffix();
        const std::string other_slot = (current_slot == "_a") ? "_b" : "_a";

        printf("Slot Information:\n");
        printf("  Current slot: %s\n", current_slot.c_str());
        printf("  Other slot:   %s\n", other_slot.c_str());

        // Try to get bootctl info if available
        auto result = exec_command({"getprop", "ro.boot.slot_suffix"});
        if (result.exit_code == 0) {
            printf("  Property ro.boot.slot_suffix: %s\n", trim(result.stdout_str).c_str());
        }

        return 0;

    } else if (filtered_args[0] == "map" && filtered_args.size() >= 2) {
        std::string slot = filtered_args[1];
        // Normalize slot format
        if (!slot.empty() && slot[0] != '_') {
            slot = "_" + slot;
        }

        printf("Mapping logical partitions for slot %s...\n", slot.c_str());
        if (tamisu_daemon::flash::map_logical_partitions(slot)) {
            printf("Mapping successful!\n");
            printf("You can now use 'tamisu_daemon flash list --slot %s --all' to see mapped partitions\n",
                   slot.c_str());
            return 0;
        } else {
            printf("Mapping failed or no partitions to map\n");
            return 1;
        }

    } else if (filtered_args[0] == "avb") {
        if (filtered_args.size() >= 2 && filtered_args[1] == "disable") {
            printf("Disabling AVB/dm-verity...\n");
            if (tamisu_daemon::flash::patch_vbmeta_disable_verification()) {
                printf("AVB/dm-verity disabled successfully!\n");
                printf("Reboot required for changes to take effect.\n");
                return 0;
            } else {
                printf("Failed to disable AVB/dm-verity\n");
                return 1;
            }
        } else {
            const std::string status = tamisu_daemon::flash::get_avb_status();
            if (status.empty()) {
                printf("Failed to get AVB status\n");
                return 1;
            }
            printf("AVB/dm-verity status: %s\n", status.c_str());
            return 0;
        }

    } else if (filtered_args[0] == "kernel") {
        const std::string version = tamisu_daemon::flash::get_kernel_version(target_slot);
        if (version.empty()) {
            printf("Failed to get kernel version\n");
            return 1;
        }
        printf("Kernel version: %s\n", version.c_str());
        return 0;

    } else if (filtered_args[0] == "boot-info") {
        const std::string info = tamisu_daemon::flash::get_boot_slot_info();
        printf("%s\n", info.c_str());
        return 0;
    }

    printf("Unknown flash subcommand: %s\n", subcmd.c_str());
    printf("Run 'tamisu_daemon flash' for usage\n");
    return 1;
}

int cmd_late_load(const std::vector<std::string>& args) {
    bool allow_shell = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--allow-shell") {
            allow_shell = true;
            continue;
        }

        printf("Unknown late-load option: %s\n", args[i].c_str());
        printf("Usage: tamisu_daemon late-load [--allow-shell]\n");
        return 1;
    }

    return late_load::run(false, allow_shell);
}

}  // namespace

int cli_run(int argc, char** argv) {
    // Initialize logging
    log_init("Tamisu");

    if (argc < 2) {
        print_usage();
        return 0;
    }

    const std::string cmd = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; i++) {
        args.push_back(argv[i]);
    }

    LOGI("command: %s", cmd.c_str());

    // Dispatch commands
    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        print_usage();
        return 0;
    } else if (cmd == "version" || cmd == "-v" || cmd == "-V" ||
               cmd == "--version") {
        print_version();
        return 0;
    } else if (cmd == "insmod") {
        return cmd_insmod(args);
    } else if (cmd == "late-load") {
        return cmd_late_load(args);
    } else if (cmd == "post-fs-data") {
        return on_post_data_fs();
    } else if (cmd == "services") {
        on_services();
        return 0;
    } else if (cmd == "boot-completed") {
        on_boot_completed();
        return 0;
    } else if (cmd == "install") {
        std::optional<std::string> magiskboot;
        for (size_t i = 0; i < args.size(); i++) {
            if (args[i] == "--magiskboot" && i + 1 < args.size()) {
                magiskboot = args[i + 1];
            }
        }
        return install(magiskboot);
    } else if (cmd == "uninstall") {
        std::optional<std::string> magiskboot;
        for (size_t i = 0; i < args.size(); i++) {
            if (args[i] == "--magiskboot" && i + 1 < args.size()) {
                magiskboot = args[i + 1];
            }
        }
        return uninstall(magiskboot);
    } else if (cmd == "sepolicy") {
        return cmd_sepolicy(args);
    } else if (cmd == "feature") {
        return cmd_feature(args);
    } else if (cmd == "yukizygisk") {
        return cmd_yukizygisk(args);
    } else if (cmd == "initrc") {
        return cmd_initrc(args);
    } else if (cmd == "boot-patch") {
        return boot_patch(args);
    } else if (cmd == "boot-restore") {
        return boot_restore(args);
    } else if (cmd == "boot-info") {
        return cmd_boot_info(args);
    } else if (cmd == "kernel") {
        return cmd_kernel(args);
    } else if (cmd == "debug") {
        return cmd_debug(args);
    } else if (cmd == "flash") {
        return cmd_flash_new(args);
    }

    printf("Unknown command: %s\n", cmd.c_str());
    print_usage();
    return 1;
}

}  // namespace tamisu_daemon
