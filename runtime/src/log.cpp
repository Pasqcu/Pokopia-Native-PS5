// SPDX-License-Identifier: GPL-3.0-or-later
// One formatted line per call to the platform console and, once opened, to the log file. No thread_local and no
// locking: the bring-up runs on one thread (the PS5 SDK's -femulated-tls has no runtime for thread_local here).
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "pkp/platform.h"
#include "pkp/rt.h"

namespace pkp {

namespace {
char g_log_path[256];
}

void log_set_file(const char* path) {
    std::snprintf(g_log_path, sizeof g_log_path, "%s", path ? path : "");
}

const char* log_file() { return g_log_path[0] ? g_log_path : nullptr; }

void log(const char* fmt, ...) {
    char line[1024];
    const uint64_t t = plat::now_ns();
    int n = std::snprintf(line, sizeof line, "[%6llu.%03llu] ", (unsigned long long)(t / 1000000000ull),
                          (unsigned long long)(t / 1000000ull % 1000ull));
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof line - 2) len = sizeof line - 2;
    if (len == 0 || line[len - 1] != '\n') line[len++] = '\n';
    line[len] = 0;
    plat::log_console(line, len);
    if (g_log_path[0]) plat::append_file(g_log_path, line, len);
}

}  // namespace pkp
