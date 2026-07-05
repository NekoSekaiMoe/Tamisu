#pragma once

#include <cstdint>
#include <string>

// Kernel uapi headers provide feature IDs, event constants, and ioctl numbers.
extern "C" {
#include "uapi/feature.h"
}
#include "uapi/supercall.h"  // EVENT_*, TAMISU_MARK_* are macros

namespace tamisu_daemon {

// Version info
constexpr const char* TAMISU_DAEMON_VERSION = "1.0.0";
constexpr int TAMISU_DAEMON_VERSION_CODE = 10000;
extern const char* const VERSION_CODE;
extern const char* const VERSION_NAME;

// Paths
constexpr const char* ADB_DIR = "/data/adb/";
constexpr const char* WORKING_DIR = "/data/tamisu/";
constexpr const char* BINARY_DIR = "/data/tamisu/bin/";
constexpr const char* LOG_DIR = "/data/tamisu/log/";

// Binary tool paths
constexpr const char* BUSYBOX_PATH = "/data/tamisu/bin/busybox";
constexpr const char* RESETPROP_PATH = "/data/tamisu/bin/resetprop";
constexpr const char* BOOTCTL_PATH = "/data/tamisu/bin/bootctl";

constexpr const char* TAMISURC_PATH = "/data/tamisu/.tamisurc";
constexpr const char* DAEMON_PATH = "/data/tamisu_daemon";
constexpr const char* MAGISKBOOT_PATH = "/data/tamisu/bin/magiskboot";

// Tamisu Zygisk runtime payload: tamisu_daemon stages these at post-fs-data; the kernel
// reads the first-stage/core libraries as tamisu_cred and hands them to target
// processes via memfd, so target processes never open these paths directly.
// Private to tamisu's lib dir to avoid colliding with other zygisk implementations
// under /data/adb/zygisk.
constexpr const char* YUKIZYGISK_DIR = "/data/tamisu/lib/zygisk/";
constexpr const char* ZCORE_PATH = "/data/tamisu/lib/zygisk/libzygisk.so";
constexpr const char* ZNCORE_PATH = "/data/tamisu/lib/zygisk/libzygisk_zncore.so";
// Split-out anonymous module loader; core dlopen's it (fd brokered by zygiskd).
constexpr const char* ZYUKILINKER_PATH = "/data/tamisu/lib/zygisk/libzygisk_linker.so";
// Runtime config, kept apart from the binary payload dir so the manager can
// rewrite it freely. zygiskd parses it and brokers it to core.
constexpr const char* YZCONFIG_DIR = "/data/tamisu/zygisk/";
constexpr const char* YZCONFIG_PATH = "/data/tamisu/zygisk/yzconfig.json";
constexpr const char* DAEMON_LINK_PATH = "/data/tamisu/bin/tamisu_daemon";

constexpr const char* MODULE_DIR = "/data/adb/modules/";
constexpr const char* PREINIT_DIR_WATCHDOG = "/metadata/watchdog/tamisu/";
constexpr const char* PREINIT_DIR_DEFAULT = "/metadata/tamisu/";
constexpr const char* MODULES_RC_FILE = "modules.rc";
constexpr const char* MODULES_RC_TMP_FILE = ".modules.rc.tmp";

constexpr const char* MODULE_WEB_DIR = "webroot";
constexpr const char* MODULE_ACTION_SH = "action.sh";
constexpr const char* DISABLE_FILE_NAME = "disable";
constexpr const char* UPDATE_FILE_NAME = "update";
constexpr const char* REMOVE_FILE_NAME = "remove";
constexpr const char* MODULE_INIT_RC_DIR = "initrc";

// Module config system
constexpr const char* MODULE_CONFIG_DIR = "/data/tamisu/module_configs/";
constexpr const char* PERSIST_CONFIG_NAME = "persist.config";
constexpr const char* TEMP_CONFIG_NAME = "tmp.config";

// Backup
constexpr const char* TAMISU_BACKUP_DIR = "/data/tamisu/";
constexpr const char* TAMISU_BACKUP_FILE_PREFIX = "tamisu_backup_";
constexpr const char* BACKUP_FILENAME = "stock_image.sha1";

// No need to redefine FeatureId, EVENT_*, TAMISU_MARK_* —
// they are all provided by uapi/feature.h and uapi/supercall.h.
// C++ callers can use the C enum tamisu_feature_id values directly or
// via the convenience wrappers in core/tamisuctl.hpp.

}  // namespace tamisu_daemon
