// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 title entry, stage 2 (kernel HLE): boots the game on the ported nxrt runtime, exactly like the host boot
// (runtime/nxrt_port/boot_main.cc), with the user's files in the title folder:
//   /app0/game/exefs/{rtld,main,sdk,subsdk0}   /app0/game/romfs/...   (copied by scripts/make_ppsa.sh --stage boot)
// The game runs on its own host thread; after 20 s the main thread shows the runtime's state on screen (VideoOut of the
// ps5-native-app-boilerplate) while the game keeps running. The full log: /app0/logs/nxrt.log.
// There is no GPU back end yet: the boot is expected to stop where the game first waits for NVN or the display.
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "demo_renderer.hpp"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"
#include "nxrt/runtime.h"
#include "pkp_boot.h"
#include "pkp_game.h"
#include "pkp/platform.h"
#include "screen_font.h"

// The boilerplate renderer checks its unique_ptr bridge through this hook (app_cpp_runtime.cpp, which the stage-2 title
// does not link: libc++ provides operator new here).
extern "C" bool ps5ObserveOwnedAllocation(const void* address) noexcept { return address != nullptr; }

namespace {

char g_lines[40][120];
int g_nlines;
const char* g_fail = nullptr;   // start-up failure before the game thread runs

void add_line(const char* s) {
    if (g_nlines < 40) std::snprintf(g_lines[g_nlines++], sizeof g_lines[0], "%s", s);
}

void* game_thread(void*) {
    std::string err;
    if (!pkp::boot::load_modules("/app0/game/exefs", &err)) {
        NXLOG_E("boot", "%s", err.c_str());
        nxrt::log_flush();
        return nullptr;
    }
    pkp::boot::run_entry();
    return nullptr;
}

using namespace pkp::screen;

void draw_scene(Canvas& cv) noexcept {
    cv.clear(rgb(12, 28, 30));
    cv.rectangle(0, 0, 1920, 150, rgb(18, 60, 58));
    cv.rectangle(0, 150, 1920, 6, rgb(120, 200, 110));
    draw_text(cv, 60, 30, "POKOPIA NATIVE ON PS5", 8, rgb(240, 250, 240));
    draw_text(cv, 64, 102, "STAGE 2: THE GAME ON THE HORIZON KERNEL HLE - RUNTIME STATE AFTER 20 S", 3, rgb(170, 220, 190));
    const char* st = g_fail ? g_fail : pkp::boot::stage();
    draw_text(cv, 60, 190, st, 4, std::strncmp(st, "FAIL", 4) ? rgb(120, 230, 140) : rgb(255, 110, 100));
    unsigned y = 260;
    for (int i = 0; i < g_nlines && y < 1040; i++, y += 27) draw_text(cv, 60, y, g_lines[i], 3, rgb(210, 220, 225));
    draw_text(cv, 60, 1048, "LOG: /APP0/LOGS/NXRT.LOG", 2, rgb(140, 160, 160));
}

}  // namespace

int main() {
    mkdir("/app0/logs", 0777);
    nxrt_register_game_pkp_core();
    nxrt::InitOptions opt;
    opt.log_path = "/app0/logs/nxrt.log";
    opt.watchdog = false;
    opt.memguard = false;
    opt.load_symbols = false;
    const bool up = nxrt::init(opt);
    nxrt::config_set("fs", "exefs", "/app0/game/exefs");
    nxrt::config_set("fs", "romfs", "/app0/game/romfs");
    mkdir("/app0/save", 0777);
    nxrt::config_set("fs", "save_dir", "/app0/save");   // upstream's default is a Windows-style relative folder
    nxrt::config_set("process", "program_id", "0x0400A3A01EE4C000");
    NXLOG_I("boot", "Pokopia Native: PS5 boot on the HLE runtime port");
    {
        const pkp::plat::MemInfo mi = pkp::plat::mem_info();
        NXLOG_I("boot", "memory: direct %llu MiB, flexible available %llu MiB (the executable is charged to it), page %llu KiB",
                (unsigned long long)(mi.direct_total >> 20), (unsigned long long)(mi.flexible_available >> 20),
                (unsigned long long)(mi.page_size >> 10));
        NXLOG_I("boot", "host image: nxrt::crash_install at %p (to symbolise crash addresses with build/ppsa/llvm-pie.elf)",
                (void*)&nxrt::crash_install);
    }
    if (up) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        constexpr size_t kStack = 64u << 20;   // lifted code nests deeply
        if (void* stk = pkp::plat::big_alloc(kStack)) pthread_attr_setstack(&a, stk, kStack);   // direct memory, not flexible
        else pthread_attr_setstacksize(&a, kStack);
        pthread_t t;
        pthread_create(&t, &a, game_thread, nullptr);
        pkp::plat::sleep_us(20u * 1000000u);
        const pkp::plat::VmStats vs = pkp::plat::vm_stats();
        const pkp::plat::BigStats bs = pkp::plat::big_stats();
        const pkp::plat::MemInfo mi = pkp::plat::mem_info();
        char line[160];
        std::snprintf(line, sizeof line, "GUEST MEMORY: %llu MIB BACKED (HEAP %llu OF %llu MIB COMMITTED), %llu FAULTS",
                      (unsigned long long)((vs.region_backed + vs.section_backed) >> 20), (unsigned long long)(vs.section_backed >> 20),
                      (unsigned long long)(vs.section_committed >> 20), (unsigned long long)vs.faults);
        add_line(line);
        std::snprintf(line, sizeof line, "HOST: BIG BLOCKS %llu MIB (PEAK %llu), FLEXIBLE FREE %llu MIB",
                      (unsigned long long)(bs.bytes >> 20), (unsigned long long)(bs.peak_bytes >> 20),
                      (unsigned long long)(mi.flexible_available >> 20));
        add_line(line);
        NXLOG_I("boot", "after 20 s: %s; big blocks %llu MiB (peak %llu MiB, %llu blocks); flexible available %llu MiB", g_lines[g_nlines - 2],
                (unsigned long long)(bs.bytes >> 20), (unsigned long long)(bs.peak_bytes >> 20), (unsigned long long)bs.blocks,
                (unsigned long long)(mi.flexible_available >> 20));
    } else {
        g_fail = "FAIL: runtime start-up (address space)";
    }
    nxrt::log_flush();
    // the runtime's own summary, one line per screen row
    const std::string s = nxrt::summary();
    size_t p = 0;
    while (p < s.size() && g_nlines < 40) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        add_line(s.substr(p, e - p).c_str());
        p = e + 1;
    }
    ps5::demo::run(draw_scene, g_fail ? g_fail : pkp::boot::stage());
}
