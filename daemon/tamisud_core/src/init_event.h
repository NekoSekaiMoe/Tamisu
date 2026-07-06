#pragma once

namespace tamisu_daemon {

int on_post_data_fs();
void on_services();
void on_boot_completed();

// Launch zygiskd if the zygisk feature is enabled. Called from the normal
// post-fs-data stage and from the late-load path.
void ensure_zygiskd_running_if_enabled();

}  // namespace tamisu_daemon
