#include "assets.h"
#include "defs.h"
#include "log.h"
#include "utils.h"

namespace tamisu_daemon {

// Assets are now embedded at compile time by embed_assets.py
// The generated assets_data.cpp contains:
// - list_assets()
// - get_asset()
// - copy_asset_to_file()
// - list_supported_kmi()
// - ensure_binaries()

// This file is kept for any additional asset-related utilities

}  // namespace tamisu_daemon
