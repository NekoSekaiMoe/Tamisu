#pragma once

#include <string>

namespace tamisu_daemon::tamisu_loader {

bool load_module(const char* path, const std::string& param_values = "");

}  // namespace tamisu_daemon::tamisu_loader
