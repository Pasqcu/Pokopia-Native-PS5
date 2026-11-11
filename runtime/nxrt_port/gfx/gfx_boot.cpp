// SPDX-License-Identifier: GPL-3.0-or-later
// Graphics start-up of the port's boot (include/pkp_boot.h: start_graphics), the same sequence as pokopia-recomp's game exe
// (game-build/src/pkp_main.cpp): nxrt's NVN front end, then its Vulkan back end on a window (SDL3) or headless (an offscreen
// image ring), then play mode (input, frame limiter, overlay). [boot] gfx = window | headless | none.
// The window is created on the calling thread, which must be the one that then runs the game: SDL (and macOS's Cocoa) pump
// its events there, at every SVC of the game's main thread and at each frame boundary (play.cpp).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#if defined(__PROSPERO__)
#include <SDL3/SDL_hints.h>
#endif

#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/nvn.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/platform.h"
#include "nxrt/play.h"
#include "pkp_boot.h"

namespace pkp::boot {
namespace {
std::unique_ptr<nxrt::gfx::Device> g_device;
std::unique_ptr<nxrt::gfx::Renderer> g_renderer;
bool g_play = false;
}  // namespace

bool start_graphics(std::string* err) {
    nxrt::nvn::install();   // nvnBootstrapLoader and the NVN entry points
    const std::string mode = nxrt::config_get_str("boot", "gfx", "window");
    if (mode == "none") {
        NXLOG_I("boot", "graphics: [boot] gfx = none: the NVN entry points stay logging stubs");
        return true;
    }
    const bool headless = mode != "window";
    nxrt::play::DisplayConfig disp;
    if (!headless) {
#if defined(__APPLE__)
        // SDL looks for the Vulkan loader by bare name (and in an app bundle's Frameworks) and misses Homebrew's; volk finds it.
        // Name it unless the user did (the loader then finds MoltenVK through its ICD file).
        if (!std::getenv("SDL_VULKAN_LIBRARY"))
            for (const char* lib : {"/usr/local/lib/libvulkan.1.dylib", "/opt/homebrew/lib/libvulkan.1.dylib", "/usr/local/lib/libMoltenVK.dylib",
                                    "/opt/homebrew/lib/libMoltenVK.dylib"})
                if (access(lib, R_OK) == 0) {
                    setenv("SDL_VULKAN_LIBRARY", lib, 0);
                    break;
                }
#endif
#if defined(__PROSPERO__)
        // SDL3 reads the environment on FreeBSD through dlsym(RTLD_DEFAULT, "environ"), which a PS5 title does not get: SDL
        // sees no variables at all. The video driver goes in as a hint: the dummy one (the only one built for the PS5), or
        // what the title's environment names (/app0/env.txt).
        if (!SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) {
            const char* v = std::getenv("SDL_VIDEO_DRIVER");
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, v && *v ? v : "dummy");
        }
#endif
        disp = nxrt::play::display();
        nxrt::platform::init();
        nxrt::platform::WindowDesc wd;
        wd.title = nxrt::game::info().title;
        wd.width = disp.width;
        wd.height = disp.height;
        if (!nxrt::platform::create_window(wd)) {
            if (err) *err = "cannot create the game window (see the log)";
            return false;
        }
    }
    nxrt::gfx::DeviceDesc dd;
    dd.headless = headless ? 1 : 0;
    std::string e;
    g_device = nxrt::gfx::Device::create(dd, &e);
    if (!g_device) {
        if (err) *err = "graphics device: " + e;
        return false;
    }
    nxrt::gfx::RendererDesc rd;
    rd.headless_width = static_cast<uint32_t>(nxrt::config_get_int("boot", "headless_width", 1280));
    rd.headless_height = static_cast<uint32_t>(nxrt::config_get_int("boot", "headless_height", 720));
    if (!headless) rd.present_mode = disp.present_mode;
    g_renderer = nxrt::gfx::Renderer::create(*g_device, rd, &e);
    if (!g_renderer) {
        if (err) *err = "renderer: " + e;
        return false;
    }
    nxrt::nvn::BackendDesc bd;
    bd.device = g_device.get();
    bd.renderer = g_renderer.get();
    if (!headless) {
        bd.limiter = nxrt::play::limiter();
        bd.keep_aspect = disp.keep_aspect;
    }
    if (!nxrt::nvn::install_backend(bd)) {
        if (err) *err = "the NVN back end failed to start (see the log)";
        return false;
    }
    NXLOG_I("boot", "graphics: NVN back end running on %s (%s)", g_device->gpu().name.c_str(), headless ? "headless" : "window");
    nxrt::play::StartOptions po;
    po.device = g_device.get();
    po.renderer = g_renderer.get();
    po.window = !headless;
    po.overlay = !headless;
    nxrt::play::start(po);
    g_play = true;
#if defined(__PROSPERO__)
    ps5_pad_start();   // the DualSense over play mode's input source (gfx/ps5_pad.cpp)
#endif
    return true;
}

void request_screenshot(const char* png_path) {
    if (g_renderer && png_path) g_renderer->request_screenshot(png_path);
}

nxrt::gfx::Renderer* graphics_renderer() { return g_renderer.get(); }   // gfx/boot_status.cpp

std::string graphics_stats() {
    if (!nxrt::nvn::backend_installed()) return std::string();
    const nxrt::nvn::BackendStats b = nxrt::nvn::backend_stats();
    char line[512];
    std::snprintf(line, sizeof line,
                  "nvn back end: submits %llu lists %llu draws %llu (skipped %llu) presents %llu pipelines %llu textures %llu (failed %llu) unsupported %llu",
                  (unsigned long long)b.submits, (unsigned long long)b.lists_replayed, (unsigned long long)b.draws,
                  (unsigned long long)b.draws_skipped, (unsigned long long)b.presents, (unsigned long long)b.pipelines_created,
                  (unsigned long long)b.textures_created, (unsigned long long)b.textures_failed, (unsigned long long)b.unsupported_ops);
    return line;
}

void stop_graphics() {
#if defined(__PROSPERO__)
    ps5_pad_stop();
#endif
    if (g_play) nxrt::play::stop();
    g_play = false;
}

}  // namespace pkp::boot
