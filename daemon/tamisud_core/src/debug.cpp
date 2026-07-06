#include "debug.h"
#include "core/tamisuctl.h"
#include "log.h"
#include "utils.h"

#include <sys/utsname.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tamisu_daemon {

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

int debug_get_kernel_version() {
    return get_version_impl();
}

int debug_mark(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("Usage: tamisu_daemon debug mark <get|mark|unmark|refresh> [PID]\n");
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
        printf("Refreshed all marks\n");
        return 0;
    }

    printf("Unknown mark command: %s\n", cmd.c_str());
    return 1;
}

}  // namespace tamisu_daemon
