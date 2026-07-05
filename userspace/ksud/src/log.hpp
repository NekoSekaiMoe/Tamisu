#pragma once

#include <cstdarg>
#include <string>

namespace tamisu_daemon {

enum class LogLevel {
    VERBOSE,
    DEBUG,
    INFO,
    WARN,
    ERROR,
};

void log_init(const char* tag);
void log_set_level(LogLevel level);
/** Flush stderr (log stream). */
void log_flush();
void log_v(const char* fmt, ...);
void log_d(const char* fmt, ...);
void log_i(const char* fmt, ...);
void log_w(const char* fmt, ...);
void log_e(const char* fmt, ...);

// Helper macros
#define LOGV(...) tamisu_daemon::log_v(__VA_ARGS__)
#define LOGD(...) tamisu_daemon::log_d(__VA_ARGS__)
#define LOGI(...) tamisu_daemon::log_i(__VA_ARGS__)
#define LOGW(...) tamisu_daemon::log_w(__VA_ARGS__)
#define LOGE(...) tamisu_daemon::log_e(__VA_ARGS__)

}  // namespace tamisu_daemon
