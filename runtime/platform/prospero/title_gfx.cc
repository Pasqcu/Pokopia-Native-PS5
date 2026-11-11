// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 title entry, stage 3 (graphics): the game on the HLE runtime port with nxrt's NVN front end on RADV (PS5_Vulkan),
// the picture on VideoOut through VK_KHR_display (runtime/nxrt_port/gfx/platform_ps5_vulkan.inc). The title folder:
//   /app0/game/exefs/{rtld,main,sdk,subsdk0}   /app0/game/romfs/...   /app0/game/shader_cache/   (scripts/make_ppsa.sh, deploy)
// The game runs on its own thread with a large stack; SDL's events (the dummy driver) are pumped there by play mode. The logs:
// /app0/logs/nxrt.log (the runtime's), /app0/logs/stderr.log (standard error: RADV's and Mesa's messages). /app0/env.txt, when
// present, holds NAME=VALUE lines set in the environment before anything reads it (RADV_DEBUG, ACO_DEBUG, MESA_*, ...).
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"
#include "nxrt/runtime.h"
#include "pkp/platform.h"
#include "pkp_boot.h"
#include "pkp_game.h"

#include "boot_screen.h"

extern "C" {
int ps5_klog_capture_stderr(const char* prefix);   // SDK fork platform layer (ps5platform/klog.h)
void pkp_boot_trace(const char* step);             // title_crt.cc: /app0/logs/boot_trace.txt and klog
}

namespace {

// The floating-point state is IEEE from title_crt.cc on (threads made through the platform's pthread_create inherit it).
void environment_setup() {
    if (FILE* env = std::fopen("/app0/env.txt", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, env)) {
            line[std::strcspn(line, "\r\n")] = '\0';
            char* eq = std::strchr(line, '=');
            if (eq && line[0] != '#') {
                *eq = '\0';
                setenv(line, eq + 1, 1);
            }
        }
        std::fclose(env);
    }
    // SDL3 takes its dummy video driver (the only one built for the PS5: events, virtual joysticks) only when named.
    setenv("SDL_VIDEO_DRIVER", "dummy", 0);
    if (std::freopen("/app0/logs/stderr.log", "w", stderr)) setvbuf(stderr, nullptr, _IOLBF, 0);
    else ps5_klog_capture_stderr("[pokopia:stderr] ");
}

void* game_thread(void*) {
    namespace screen = pkp::boot_screen;
    std::string err;
    pkp_boot_trace("game thread: graphics");
    screen::step(screen::Step::Graphics);
    screen::hand_over();
    if (!pkp::boot::start_graphics(&err)) {
        NXLOG_E("boot", "graphics: %s", err.c_str());
        nxrt::log_flush();
        screen::fail(("GRAFICA: " + err).c_str());
        return nullptr;
    }
    screen::on_renderer();
    screen::step(screen::Step::Modules);
    pkp_boot_trace("game thread: graphics up, loading the game's modules");
    if (!pkp::boot::load_modules("/app0/game/exefs", &err, true)) {
        NXLOG_E("boot", "%s", err.c_str());
        nxrt::log_flush();
        screen::fail(err.c_str());
        return nullptr;
    }
    pkp_boot_trace("game thread: running the game");
    screen::step(screen::Step::Game);
    pkp::boot::run_entry();
    screen::fail("LA FUNZIONE DI INGRESSO DEL GIOCO E' TERMINATA");
    return nullptr;
}

}  // namespace

int main() {
    mkdir("/app0/logs", 0777);
    mkdir("/app0/save", 0777);
    pkp_boot_trace("main: environment");
    environment_setup();
    pkp::boot_screen::start();   // hides the system's splash screen, shows the status screen on VideoOut
    pkp_boot_trace("main: runtime init");
    nxrt_register_game_pkp_core();
    nxrt::InitOptions opt;
    opt.log_path = "/app0/logs/nxrt.log";
    opt.watchdog = false;
    opt.memguard = false;
    opt.load_symbols = false;
    if (!nxrt::init(opt)) {
        nxrt::log_flush();
        pkp::boot_screen::fail("AVVIO DEL RUNTIME (NXRT::INIT) NON RIUSCITO");
        for (;;) pkp::plat::sleep_us(1000000);   // the error stays on the screen (close the title from the home screen)
    }
    nxrt::config_set("fs", "exefs", "/app0/game/exefs");
    nxrt::config_set("fs", "romfs", "/app0/game/romfs");
    nxrt::config_set("fs", "save_dir", "/app0/save");
    nxrt::config_set("process", "program_id", "0x0400A3A01EE4C000");
    nxrt::config_set("boot", "gfx", "window");   // VideoOut
    if (!nxrt::config_has("nvn", "shader_cache")) nxrt::config_set("nvn", "shader_cache", "/app0/game/shader_cache/spirv");
    // What the graphics shell keeps in the working folder on a PC (relative paths): the title's folder here.
    mkdir("/app0/cache", 0777);
    for (const auto& [section, key, path] : {std::array<const char*, 3>{"nvn", "pipeline_cache", "/app0/cache/pipeline_cache.bin"},
                                             std::array<const char*, 3>{"nvn", "pipeline_keys", "/app0/cache/pipeline_keys.bin"},
                                             std::array<const char*, 3>{"nvn", "shader_order", "/app0/cache/shader_order.txt"},
                                             std::array<const char*, 3>{"play", "controls", "/app0/controls.ini"}})
        if (!nxrt::config_has(section, key)) nxrt::config_set(section, key, path);
    // What the console needs from upstream's PC defaults (each one can still be set in /app0/env.txt):
    //   threads.host_stack_mb 16: a guest thread's host stack is direct memory, committed whole here. Upstream's 64 MiB (a lazy
    //   reservation on Windows) took 4.7 GiB at the 74 threads of the game's start and exhausted direct memory: the console
    //   itself went down. 16 MiB is the port's floor for a host stack (src/win32_shim.cpp).
    //   applet.swkbd_utf8: the game reads the software keyboard's answer as UTF-8 although its config asks for UTF-16 (the
    //   UTF-16 answer showed only its first letter).
    //   nvn.dump_missing: the shader blocks the offline cache has no translation for (they are not in the RomFS) go to
    //   /app0/cache/shader_missing, for scripts/sync_missing_shaders.sh to translate and send back.
    for (const auto& [section, key, value] : {std::array<const char*, 3>{"threads", "host_stack_mb", "16"},
                                              std::array<const char*, 3>{"applet", "swkbd_utf8", "true"},
                                              std::array<const char*, 3>{"nvn", "dump_missing", "/app0/cache/shader_missing"}})
        if (!nxrt::config_has(section, key)) nxrt::config_set(section, key, value);
    {
        const pkp::plat::MemInfo mi = pkp::plat::mem_info();
        NXLOG_I("boot", "Pokopia Native: PS5 title with graphics (RADV on VideoOut); memory: direct %llu MiB, flexible available %llu MiB",
                (unsigned long long)(mi.direct_total >> 20), (unsigned long long)(mi.flexible_available >> 20));
        NXLOG_I("boot", "host image: nxrt::crash_install at %p (to symbolise crash addresses with build/ppsa/llvm-pie.elf)",
                (void*)&nxrt::crash_install);
        nxrt::log_flush();
    }
    pthread_attr_t a;
    pthread_attr_init(&a);
    constexpr size_t kStack = 64u << 20;   // lifted code nests deeply
    if (void* stk = pkp::plat::big_alloc(kStack)) pthread_attr_setstack(&a, stk, kStack);
    else pthread_attr_setstacksize(&a, kStack);
    pthread_t t;
    pkp_boot_trace("main: starting the game thread");
    pthread_create(&t, &a, game_thread, nullptr);
    pthread_join(t, nullptr);
    NXLOG_I("boot", "the game thread ended");
    nxrt::log_flush();
    for (;;) pkp::plat::sleep_us(1000000);   // a title does not return to the system (close it from the home screen)
}
