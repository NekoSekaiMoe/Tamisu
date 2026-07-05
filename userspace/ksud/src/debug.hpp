#pragma once

#include <string>
#include <vector>

namespace tamisu_daemon {

int debug_insmod(const std::string& module, const std::vector<std::string>& params);
int debug_mark(const std::vector<std::string>& args);
int debug_get_kernel_version();

}  // namespace tamisu_daemon
