#include "debug.hpp"
#include "core/ksucalls.hpp"
#include "kernelsu_loader.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <sys/utsname.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ksud {

namespace {

int get_version_impl() {
    struct utsname buf{};
    if (uname(&buf) != 0) {
        return -1;
    }
    int a = 0, b = 0;
    if (sscanf(buf.release, "%d.%d", &a, &b) >= 2) {
        return a * 100 + b;
    }
    return -1;
}

}  // namespace

int get_version() {
    return get_version_impl();
}

int debug_insmod(const std::string& module, const std::vector<std::string>& params) {
    std::error_code ec;
    const auto resolved_path = std::filesystem::canonical(module, ec);
    if (ec) {
        printf("Failed to resolve module path %s: %s\n", module.c_str(), ec.message().c_str());
        return 1;
    }

    std::string param_values;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i != 0) {
            param_values.push_back(' ');
        }
        param_values += params[i];
    }

    if (!kernelsu_loader::load_module(resolved_path.c_str(), param_values)) {
        printf("Failed to load kernel module: %s\n", resolved_path.c_str());
        return 1;
    }

    printf("Loaded kernel module: %s\n", resolved_path.c_str());
    return 0;
}

int debug_mark(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("Usage: ksud debug mark <get|mark|unmark|refresh> [PID]\n");
        return 1;
    }

    const std::string& cmd = args[0];
    const int32_t pid = args.size() > 1 ? std::stoi(args[1]) : 0;

    if (cmd == "get") {
        const uint32_t result = mark_get(pid);
        if (pid == 0) {
            printf("Total marked processes: %u\n", result);
        } else {
            printf("Process %d is %s\n", pid, result ? "marked" : "not marked");
        }
        return 0;
    } else if (cmd == "mark") {
        if (mark_set(pid) < 0) {
            printf("Failed to mark process %d\n", pid);
            return 1;
        }
        printf("Marked process %d\n", pid);
        return 0;
    } else if (cmd == "unmark") {
        if (mark_unset(pid) < 0) {
            printf("Failed to unmark process %d\n", pid);
            return 1;
        }
        printf("Unmarked process %d\n", pid);
        return 0;
    } else if (cmd == "refresh") {
        if (mark_refresh() < 0) {
            printf("Failed to refresh marks\n");
            return 1;
        }
        printf("Refreshed all process marks\n");
        return 0;
    }

    printf("Unknown mark command: %s\n", cmd.c_str());
    return 1;
}

}  // namespace ksud
