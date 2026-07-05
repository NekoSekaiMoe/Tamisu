#include "../assets.h"
#include "restorecon.h"

#include <sys/stat.h>
#include <filesystem>
#include <string>

#include "../defs.h"
#include "../log.h"
#include "../utils.h"

namespace tamisu_daemon {

// Hand-written asset helpers.
// Stage Tamisu Zygisk payloads when embedded.
int ensure_zygisk(bool ignore_if_exist) {
    struct Payload {
        const char* asset;
        const char* dest;
    };
    static const Payload payload[] = {
        {"libzygisk.so", ZCORE_PATH},
        {"libzygisk_zncore.so", ZNCORE_PATH},
        {"libzygisk_linker.so", ZYUKILINKER_PATH},
    };

    bool embedded = false;
    for (const auto& p : payload) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (get_asset(p.asset, data, size)) {
            embedded = true;
            break;
        }
    }
    if (!embedded) {
        return 0;
    }

    if (!ensure_dir_exists(YUKIZYGISK_DIR)) {
        LOGE("zygisk: failed to create %s", YUKIZYGISK_DIR);
        return 1;
    }

    for (const auto& p : payload) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!get_asset(p.asset, data, size)) {
            continue;
        }

        (void)ignore_if_exist;
        if (!copy_asset_to_file(p.asset, p.dest)) {
            LOGE("zygisk: failed to stage %s", p.dest);
            continue;
        }
        chmod(p.dest, 0644);
        lsetfilecon(p.dest, SYSTEM_LIB_CON);
        LOGI("zygisk: staged %s", p.dest);
    }
    return 0;
}

}  // namespace tamisu_daemon
