// SPDX-License-Identifier: GPL-3.0-or-later
// The boot sequence shared by the host boot (runtime/nxrt_port/boot_main.cc) and the PS5 title
// (runtime/platform/prospero/title_boot.cc), after nxrt::init: the same steps as pokopia-recomp's game exe
// (game-build/src/pkp_main.cpp) without graphics.
#pragma once

#include <cstdint>
#include <string>

namespace pkp::boot {

// Maps the four NSOs of <exefs_dir> at the bases the lifter baked into the code, registers the lifted module tables,
// puts the NVN gate in place of nvnBootstrapLoader and protects the images (the game's own rtld relocates them).
// False with *err on the first failure.
// nvn_front_end: nvnBootstrapLoader goes to nxrt's NVN front end (start_graphics ran) instead of the NVN gate.
bool load_modules(const std::string& exefs_dir, std::string* err, bool nvn_front_end = false);

// nxrt's NVN front end and Vulkan back end ([boot] gfx = window | headless | none), before load_modules; builds with the
// graphics shell only (runtime/nxrt_port/gfx/gfx_boot.cpp). On the thread that will run the game.
bool start_graphics(std::string* err);
void stop_graphics();
void request_screenshot(const char* png_path);   // the next finished frame, read back as a PNG (no-op without graphics)
std::string graphics_stats();                    // the NVN back end's counters, one line ("" without graphics)

// PS5: the DualSense as player 1's pad, laid over play mode's hid input source (runtime/nxrt_port/gfx/ps5_pad.cpp, PS5 builds
// only); start_graphics and stop_graphics call these.
void ps5_pad_start();
void ps5_pad_stop();

// A status picture on the game's swapchain from the end of start_graphics until the game presents its first frame
// (runtime/nxrt_port/gfx/boot_status.cpp; the PS5 graphics title's boot screen draws it). A thread of its own paints it a
// few times a second through the renderer, which nothing else uses until then; the NVN back end's first present stops that
// thread before it touches the renderer, then calls first_frame().
struct StatusCanvas {
    virtual ~StatusCanvas() = default;
    virtual void fill(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) = 0;   // rgb: 0xRRGGBB (sRGB)
};
struct StatusScreen {
    virtual ~StatusScreen() = default;
    virtual void paint(uint32_t width, uint32_t height, StatusCanvas& canvas) = 0;
    virtual void first_frame() {}   // on the game's render thread, once
};
bool status_screen_start(StatusScreen* screen);   // false: no window renderer, or the game presents already

// The graphics back end's progress for a status screen (zeros without graphics).
struct GraphicsProgress {
    uint64_t submits, draws, pipelines, textures;
    int shaders_pending;   // shader compile jobs waiting or running
};
GraphicsProgress graphics_progress();

// Runs the game on the calling thread: rtld at offset 0 with the registers of a new Horizon process (x0 = 0,
// x1 = main thread handle). Returns the entry function's x0 if it ever returns.
uint64_t run_entry();

// Where the boot is, for status screens and logs (a static string).
const char* stage();

// True once the game asked for NVN. Until the graphics back end exists (stage M6) the gate logs the call and parks
// that game thread for good (its other threads keep running), instead of letting the console driver code fault.
bool nvn_reached();

}  // namespace pkp::boot
