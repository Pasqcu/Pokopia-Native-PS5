// SPDX-License-Identifier: GPL-3.0-or-later
// Host boot of the game on the ported HLE runtime (stage M4): the same sequence as pokopia-recomp's game exe
// (game-build/src/pkp_main.cpp), without graphics:
//   1. start nxrt (configuration, log, address space, SVC table, services);
//   2. map the four NSOs at the bases the lifter baked into the code and register the lifted module tables;
//   3. enter rtld at offset 0 with the registers a new Horizon process gets (x0 = 0, x1 = main thread handle).
// Everything after that (rtld, nn::init, nninitStartup, nnMain) is the game's own recompiled code.
//
//   pkp_boot <exefs dir> [romfs dir] [--seconds N] [--gfx window|headless|none] [--shaders DIR]
//   (N: stop after N seconds and print the runtime summary; default 60. --gfx: builds with the graphics shell, default window)
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/runtime.h"
#include "pkp/platform.h"
#include "pkp_boot.h"
#include "pkp_game.h"

int main(int argc, char** argv) {
    std::string exefs = "Pokemon Pokopia [0400A3A01EE4C000]/updates/327680/exefs", romfs;
    int seconds = 60;
    int shot_every = 0;
    std::string gfx = "window", shaders = "../shader_cache/spirv";   // from build/run: build/shader_cache (scripts/build_shader_cache.sh)
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--gfx") && i + 1 < argc) gfx = argv[++i];
        else if (!std::strcmp(argv[i], "--shaders") && i + 1 < argc) shaders = argv[++i];
        else if (!std::strcmp(argv[i], "--shot-every") && i + 1 < argc) shot_every = std::atoi(argv[++i]);   // seconds: shot_N.png
        else if (positional++ == 0) exefs = argv[i];
        else romfs = argv[i];
    }

    nxrt_register_game_pkp_core();
    nxrt::InitOptions opt;
    opt.log_path = "pkp_boot.log";
    opt.watchdog = false;
    opt.memguard = false;
    opt.load_symbols = false;
    opt.crash_handler = std::getenv("PKP_NO_CRASH_HANDLER") == nullptr;   // a debugger wants the signal itself
    if (!nxrt::init(opt)) { std::fprintf(stderr, "runtime start-up failed (address-space layout)\n"); return 1; }
    nxrt::config_set("fs", "exefs", exefs.c_str());
    if (!romfs.empty()) nxrt::config_set("fs", "romfs", romfs.c_str());
    nxrt::config_set("fs", "save_dir", "save");   // next to the log (upstream's default is a Windows-style relative folder)
    nxrt::config_set("process", "program_id", "0x0400A3A01EE4C000");
    NXLOG_I("boot", "Pokopia Native: host boot on the HLE runtime port, exefs '%s'", exefs.c_str());

    if (seconds > 0) {
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            const pkp::plat::VmStats vs = pkp::plat::vm_stats();
            if (vs.lazy)
                NXLOG_I("boot", "PS5 memory simulation: %llu MiB backed in reservations, heap %llu of %llu MiB committed backed, %llu faults",
                        (unsigned long long)(vs.region_backed >> 20), (unsigned long long)(vs.section_backed >> 20),
                        (unsigned long long)(vs.section_committed >> 20), (unsigned long long)vs.faults);
#if defined(PKP_HAVE_GFX)
            if (const std::string g = pkp::boot::graphics_stats(); !g.empty()) NXLOG_I("boot", "%s", g.c_str());
#endif
            NXLOG_I("boot", "deadline of %d s reached: state of the runtime\n%s", seconds, nxrt::summary().c_str());
            nxrt::log_flush();
            std::fflush(nullptr);
            _exit(0);
        }).detach();
    }

#if defined(PKP_HAVE_GFX)
    if (shot_every > 0) {
        std::thread([shot_every] {
            for (int n = 0;; n++) {
                std::this_thread::sleep_for(std::chrono::seconds(shot_every));
                char path[32];
                std::snprintf(path, sizeof path, "shot_%02d.png", n);
                pkp::boot::request_screenshot(path);
                if (const std::string g = pkp::boot::graphics_stats(); !g.empty()) NXLOG_I("boot", "%s: %s", path, g.c_str());
            }
        }).detach();
    }
#endif
    std::string err;
    bool nvn_front_end = false;
#if defined(PKP_HAVE_GFX)
    nxrt::config_set("boot", "gfx", gfx.c_str());
    if (!nxrt::config_has("nvn", "shader_cache")) nxrt::config_set("nvn", "shader_cache", shaders.c_str());
    if (gfx != "none") {
        if (!pkp::boot::start_graphics(&err)) { NXLOG_E("boot", "graphics: %s", err.c_str()); nxrt::log_flush(); return 1; }
        nvn_front_end = true;
    }
#endif
    if (!pkp::boot::load_modules(exefs, &err, nvn_front_end)) { NXLOG_E("boot", "%s", err.c_str()); nxrt::log_flush(); return 1; }
    const uint64_t x0 = pkp::boot::run_entry();
    NXLOG_I("boot", "state of the runtime\n%s", nxrt::summary().c_str());
    nxrt::kernel::main_thread_release();
    nxrt::shutdown();
    return (int)(x0 & 0xff);
}
