#pragma once

#include <string>

namespace tamisu_daemon {

// Ensure BINARY_DIR exists and symlinks (tamisu_daemon, busybox) are created. Returns 0 on success.
int ensure_binaries(bool ignore_if_exist);

}  // namespace tamisu_daemon
