// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 title entry (bring-up stage 1): runs the recompiled sdk code of the game natively on the console's Zen 2 cores, probes
// the HLE runtime's address-space layout, writes a log, and shows the results on screen (VideoOut from the
// ps5-native-app-boilerplate, fetched into third_party/) with a toast notification. The process stays alive afterwards:
// close it from the PS5 home screen.
//
// Game files: the user's own exefs NSOs, copied by scripts/make_ppsa.sh into <TITLE>/game/exefs/ (never distributed).
// Logs: /app0/logs/pokopia_native.log (the title folder, readable over FTP) or, if /app0 is read-only, /download0.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "demo_renderer.hpp"
#include "pkp/rt.h"
#include "screen_font.h"

namespace {

using namespace pkp::screen;

pkp::SelfTestReport g_cpu, g_layout;
char g_summary[96];
char g_logline[160];

Color line_color(const char* l) {
    if (!std::strncmp(l, "PASS", 4) || !std::strncmp(l, "OK", 2)) return rgb(120, 230, 140);
    if (!std::strncmp(l, "FAIL", 4)) return rgb(255, 110, 100);
    if (!std::strncmp(l, "SKIP", 4)) return rgb(240, 200, 90);
    return rgb(210, 220, 225);
}

void draw_scene(Canvas& cv) noexcept {
    cv.clear(rgb(12, 28, 30));
    cv.rectangle(0, 0, 1920, 150, rgb(18, 60, 58));
    cv.rectangle(0, 150, 1920, 6, rgb(120, 200, 110));
    draw_text(cv, 60, 30, "POKOPIA NATIVE ON PS5", 8, rgb(240, 250, 240));
    draw_text(cv, 64, 102, "STAGE 1: RECOMPILED GAME CODE ON ZEN 2 - CPU SELF-TEST AND MEMORY PROBE", 3, rgb(170, 220, 190));
    unsigned y = 190;
    for (int i = 0; i < g_cpu.nlines && y < 1000; i++, y += 27) draw_text(cv, 60, y, g_cpu.lines[i], 3, line_color(g_cpu.lines[i]));
    y += 12;
    for (int i = 0; i < g_layout.nlines && y < 1040; i++, y += 27)
        draw_text(cv, 60, y, g_layout.lines[i], 3, line_color(g_layout.lines[i]));
    draw_text(cv, 60, 1048, g_logline, 2, rgb(140, 160, 160));
}

}  // namespace

int main() {
    mkdir("/app0/logs", 0777);
    const char* candidates[] = {"/app0/logs/pokopia_native.log", "/download0/pokopia_native.log"};
    for (const char* p : candidates) {
        const int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) { close(fd); pkp::log_set_file(p); break; }
    }
    pkp::log("Pokopia Native on PS5: bring-up title, stage 1 (recompiled sdk on Zen 2)");
    pkp::run_selftest(&g_cpu, "/app0/game/exefs");
    pkp::run_layout_probe(&g_layout);
    std::snprintf(g_summary, sizeof g_summary, "Pokopia Native: CPU %d/%d OK, memory %s", g_cpu.passed,
                  g_cpu.passed + g_cpu.failed, g_layout.nlines ? g_layout.lines[g_layout.nlines - 1] + 14 : "-");
    std::snprintf(g_logline, sizeof g_logline, "LOG: %s", pkp::log_file() ? pkp::log_file() : "NONE (APP0 AND DOWNLOAD0 NOT WRITABLE)");
    pkp::log("%s", g_summary);
    ps5::demo::run(draw_scene, g_summary);
}
