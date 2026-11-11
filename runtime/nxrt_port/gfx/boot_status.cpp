// SPDX-License-Identifier: GPL-3.0-or-later
// The boot's status picture on the game's swapchain (include/pkp_boot.h: status_screen_start), from the end of start_graphics
// until the game's first frame. A thread of its own draws a few frames a second: the status screen's rectangles, cleared
// into the swapchain image with vkCmdClearAttachments in one dynamic-rendering pass (end_frame still runs the overlay hook).
// The hand-over needs no lock around the renderer: the NVN back end's present calls the frame listener (nxrt/diag.h) on its
// render thread before it touches the renderer, and the listener stops and joins this thread there.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/diag.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/overlay.h"
#include "pkp_boot.h"

namespace pkp::boot {
nxrt::gfx::Renderer* graphics_renderer();   // gfx_boot.cpp

namespace {
std::mutex g_mu;
std::condition_variable g_cv;
bool g_stop = false;
std::thread g_thread;
StatusScreen* g_screen = nullptr;

struct Rect {
    int32_t x, y, w, h;
    uint32_t rgb;
};

class Collect final : public StatusCanvas {
public:
    int32_t width = 0, height = 0;
    std::vector<Rect> rects;
    void fill(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) override {
        const int32_t x0 = std::max(x, 0), y0 = std::max(y, 0);
        const int32_t x1 = std::min(x + w, width), y1 = std::min(y + h, height);
        if (x1 > x0 && y1 > y0) rects.push_back({x0, y0, x1 - x0, y1 - y0, rgb});
    }
};

// The clear value of an sRGB colour: linear for sRGB and float targets, the code values themselves for UNORM ones. On an HDR
// target the picture sits at about 200 nits (scRGB: 1.0 is 80 nits; HDR10: PQ code values).
VkClearColorValue clear_color(uint32_t rgb, VkFormat format, bool hdr) {
    bool linear = false, sfloat = false;
    switch (format) {
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: linear = true; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: linear = sfloat = true; break;
        default: break;
    }
    const float gain = !hdr ? 1.f : sfloat ? 2.5f : 0.58f;
    VkClearColorValue v = {};
    for (int i = 0; i < 3; ++i) {
        float c = static_cast<float>((rgb >> (16 - 8 * i)) & 0xFF) / 255.f;
        if (linear) c = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        v.float32[i] = c * gain;
    }
    v.float32[3] = 1.f;
    return v;
}

void draw(nxrt::gfx::Renderer* r, StatusScreen* screen, Collect& c, std::vector<VkClearRect>& run) {
    nxrt::gfx::FrameInfo f;
    if (!r->begin_frame(&f)) return;
    c.width = static_cast<int32_t>(f.extent.width);
    c.height = static_cast<int32_t>(f.extent.height);
    c.rects.clear();
    screen->paint(f.extent.width, f.extent.height, c);

    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = f.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = clear_color(0, f.format, f.hdr_output);
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, f.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(f.cmd, &ri);
    // one clear per run of same-coloured rectangles, in the painter's order (clears inside a render pass keep rasterization order)
    for (size_t i = 0; i < c.rects.size();) {
        const uint32_t rgb = c.rects[i].rgb;
        run.clear();
        for (; i < c.rects.size() && c.rects[i].rgb == rgb; ++i) {
            const Rect& q = c.rects[i];
            run.push_back({{{q.x, q.y}, {static_cast<uint32_t>(q.w), static_cast<uint32_t>(q.h)}}, 0, 1});
        }
        VkClearAttachment ca = {};
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca.colorAttachment = 0;
        ca.clearValue.color = clear_color(rgb, f.format, f.hdr_output);
        vkCmdClearAttachments(f.cmd, 1, &ca, static_cast<uint32_t>(run.size()), run.data());
    }
    vkCmdEndRendering(f.cmd);
    r->end_frame();
}

void run(nxrt::gfx::Renderer* r, StatusScreen* screen) {
    nxrt::log_set_thread_name("boot-status");
    Collect c;
    std::vector<VkClearRect> rects;
    std::unique_lock<std::mutex> lk(g_mu);
    while (!g_stop) {
        lk.unlock();
        draw(r, screen, c, rects);
        lk.lock();
        g_cv.wait_for(lk, std::chrono::milliseconds(250), [] { return g_stop; });
    }
}

// The NVN back end's first present, on its render thread, before it touches the renderer.
void on_first_present() {
    std::thread t;
    StatusScreen* screen = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = true;
        t = std::move(g_thread);
        screen = g_screen;
        g_screen = nullptr;
    }
    g_cv.notify_all();
    if (t.joinable()) t.join();
    nxrt::diag_set_frame_listener(nullptr);
    if (screen) screen->first_frame();
}
}  // namespace

bool status_screen_start(StatusScreen* screen) {
    nxrt::gfx::Renderer* r = graphics_renderer();
    if (!screen || !r || r->headless() || nxrt::nvn::backend_stats().presents > 0) return false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_stop || g_thread.joinable()) return false;   // once
        nxrt::diag_set_frame_listener(on_first_present);
        g_screen = screen;
        g_thread = std::thread(run, r, screen);
    }
    const VkExtent2D e = r->extent();
    NXLOG_I("boot", "status screen on the swapchain (%ux%u, format %d) until the game's first frame", e.width, e.height,
            static_cast<int>(r->format()));
    return true;
}

GraphicsProgress graphics_progress() {
    GraphicsProgress p = {};
    if (nxrt::nvn::backend_installed()) {
        const nxrt::nvn::BackendStats b = nxrt::nvn::backend_stats();
        p.submits = b.submits;
        p.draws = b.draws;
        p.pipelines = b.pipelines_created;
        p.textures = b.textures_created;
    }
    p.shaders_pending = nxrt::gfx::shader_backlog();
    return p;
}

}  // namespace pkp::boot
