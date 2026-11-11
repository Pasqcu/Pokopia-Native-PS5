// SPDX-License-Identifier: GPL-3.0-or-later
// The PS5 graphics title's boot screen (boot_screen.h). One painter draws the picture from the boot's state and the tail of
// nxrt's log (a log sink keeps the last lines); two canvases take it: the VideoOut frames the CPU writes (from start() to
// hand_over(), or after a failure without graphics) and the game's swapchain (pkp::boot::status_screen_start, from
// on_renderer() to the game's first frame).
#include "boot_screen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include <ps5platform/videoout.h>

#include "nxrt/log.h"
#include "pkp/platform.h"
#include "pkp_boot.h"
#include "screen_font.h"

extern "C" {
int sceSystemServiceHideSplashScreen(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type,
                                  int64_t* physical_address);
int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t physical_address, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelMunmap(void* addr, size_t len);
size_t sceKernelGetDirectMemorySize(void);
void pkp_boot_trace(const char* step);   // title_crt.cc
}

namespace pkp::boot_screen {
namespace {

using pkp::boot::StatusCanvas;

// 0xRRGGBB (sRGB)
constexpr uint32_t kBackground = 0x0B1020, kPanel = 0x141B2E, kBorder = 0x2A3555, kTitle = 0xF2F5FA, kText = 0xC8D0E0,
                   kDim = 0x8C96B0, kAccent = 0x4FB3FF, kBarBack = 0x26304D, kWarn = 0xFFC94D, kError = 0xFF5A5A;

struct StepInfo {
    const char* name;
    double from, to;   // the estimate's percentage at the start and at the end of the step
    double tau;        // seconds: the estimate covers 63% of the step after this long
};
constexpr StepInfo kSteps[] = {
    {"AVVIO DEL RUNTIME", 0, 10, 2},
    {"AVVIO DELLA GRAFICA (VULKAN / RADV)", 10, 30, 3},
    {"CARICAMENTO DEI MODULI DEL GIOCO", 30, 40, 5},
    {"AVVIO DEL GIOCO: ATTESA DEL PRIMO FOTOGRAMMA", 40, 99, 120},
};
// Seconds from the last step to the game's first frame in the previous boot: the last step's estimate follows them.
constexpr const char* kBootSecondsFile = "/app0/cache/boot_seconds.txt";
constexpr const char* kLogFile = "/DATA/HOMEBREW/PPSA97656/LOGS/NXRT.LOG";

// ---- the boot's state

std::mutex g_state_mu;   // the variables down to g_cpu
Step g_step = Step::Runtime;
uint64_t g_start_ns = 0, g_step_ns = 0, g_fail_ns = 0;
bool g_failed = false;
char g_error[160];
struct CpuMeter {
    uint64_t cpu_ns = 0, wall_ns = 0;
    int percent = -1;   // of one core; -1: unknown
} g_cpu;

double g_expected_s = 0;   // the previous boot's kBootSecondsFile, 0: none (set before any painter runs)
bool g_enabled = false;
bool g_swapchain_on = false;   // the game thread's
std::atomic<bool> g_first_frame{false};

void copy_text(char* dst, size_t cap, const char* src) {
    size_t n = 0;
    for (; src[n] && n + 1 < cap; ++n) dst[n] = src[n];
    dst[n] = '\0';
}

size_t append(char* buf, size_t cap, size_t len, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
size_t append(char* buf, size_t cap, size_t len, const char* fmt, ...) {
    if (len + 1 >= cap) return len;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    return n < 0 ? len : std::min(cap - 1, len + static_cast<size_t>(n));
}

int cpu_percent_locked(uint64_t now) {
#ifdef CLOCK_PROCESS_CPUTIME_ID
    timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return -1;
    const uint64_t cpu = static_cast<uint64_t>(ts.tv_sec) * 1000000000u + static_cast<uint64_t>(ts.tv_nsec);
    if (!g_cpu.wall_ns) {
        g_cpu.cpu_ns = cpu;
        g_cpu.wall_ns = now;
    } else if (now - g_cpu.wall_ns >= 200000000u) {
        g_cpu.percent = static_cast<int>(100.0 * static_cast<double>(cpu - g_cpu.cpu_ns) / static_cast<double>(now - g_cpu.wall_ns) + 0.5);
        g_cpu.cpu_ns = cpu;
        g_cpu.wall_ns = now;
    }
    return g_cpu.percent;
#else
    (void)now;
    return -1;
#endif
}

// ---- the tail of nxrt's log

constexpr int kLogLines = 6;
constexpr size_t kLineChars = 160;
struct LogTail {
    char line[kLogLines][kLineChars];
    nxrt::LogLevel level[kLogLines];
    int count, next;
    uint64_t lines, last_ns;
    char last_error[kLineChars];
};
std::mutex g_log_mu;
LogTail g_log;

// Every line nxrt writes, "[  0.094122 E t102042 -] platform: SDL_Init failed: ...", under the log's lock. Kept without
// the prefix. Nothing here logs.
void log_sink(nxrt::LogLevel level, const char* line, void*) {
    const char* msg = std::strstr(line, "] ");
    msg = msg ? msg + 2 : line;
    const uint64_t now = pkp::plat::now_ns();
    std::lock_guard<std::mutex> lk(g_log_mu);
    copy_text(g_log.line[g_log.next], kLineChars, msg);
    g_log.level[g_log.next] = level;
    g_log.next = (g_log.next + 1) % kLogLines;
    g_log.count = std::min(g_log.count + 1, kLogLines);
    ++g_log.lines;
    g_log.last_ns = now;
    if (level == nxrt::LogLevel::Error) copy_text(g_log.last_error, kLineChars, msg);
}

// ---- the painter

const uint8_t* glyph_rows(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const pkp::screen::Glyph& g : pkp::screen::kFont)
        if (g.c == c) return g.rows;
    return nullptr;
}

int32_t text_width(const char* s, int32_t dot) {
    const size_t n = std::strlen(s);
    return n ? static_cast<int32_t>(n * 6 - 1) * dot : 0;
}

// The 5x7 font, dots of `dot` pixels, up to max_x. A glyph goes out as few rectangles: the runs of a row, grown down over
// the rows below that have the same run.
void text(StatusCanvas& cv, int32_t x, int32_t y, int32_t dot, uint32_t rgb, const char* s, int32_t max_x) {
    const auto bit = [](int col) { return static_cast<uint8_t>(0x10 >> col); };
    for (; *s && x + 5 * dot <= max_x; ++s, x += 6 * dot) {
        const uint8_t* rows = glyph_rows(*s);
        if (!rows) continue;
        uint8_t used[7] = {};
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5;) {
                if (!(rows[r] & bit(c)) || (used[r] & bit(c))) {
                    ++c;
                    continue;
                }
                uint8_t run = 0;
                int c1 = c;
                for (; c1 < 5 && (rows[r] & bit(c1)) && !(used[r] & bit(c1)); ++c1) run |= bit(c1);
                int r1 = r + 1;
                while (r1 < 7 && (rows[r1] & run) == run && !(used[r1] & run)) ++r1;
                for (int k = r; k < r1; ++k) used[k] |= run;
                cv.fill(x + c * dot, y + r * dot, (c1 - c) * dot, (r1 - r) * dot, rgb);
                c = c1;
            }
    }
}

// The layout is made for 1920x1080 and scaled to the target, centred.
struct Layout {
    double k;
    int32_t ox, oy;
    int32_t x(double v) const { return ox + static_cast<int32_t>(std::lround(v * k)); }
    int32_t y(double v) const { return oy + static_cast<int32_t>(std::lround(v * k)); }
    int32_t len(double v) const { return std::max<int32_t>(1, static_cast<int32_t>(std::lround(v * k))); }
};

double estimate(Step step, double t) {
    const StepInfo& s = kSteps[static_cast<int>(step)];
    double f = 1 - std::exp(-t / s.tau);
    if (step == Step::Game && g_expected_s > 0) {
        const double e = g_expected_s;
        f = t < e ? 0.9 * t / e : 0.9 + 0.1 * (1 - std::exp(-(t - e) / e));
    }
    return s.from + (s.to - s.from) * f;
}

void paint(StatusCanvas& cv, uint32_t width, uint32_t height) {
    const uint64_t now = pkp::plat::now_ns();
    Step step;
    uint64_t start_ns, step_ns, end_ns;
    bool failed;
    char error[sizeof g_error];
    int cpu;
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        step = g_step;
        start_ns = g_start_ns;
        step_ns = g_step_ns;
        failed = g_failed;
        end_ns = failed ? g_fail_ns : now;   // the clock and the estimate stop with a failure
        std::memcpy(error, g_error, sizeof error);
        cpu = cpu_percent_locked(now);
    }
    LogTail log;
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        log = g_log;
    }

    const double k = std::min(width / 1920.0, height / 1080.0);
    const Layout L{k, static_cast<int32_t>((width - 1920.0 * k) / 2), static_cast<int32_t>((height - 1080.0 * k) / 2)};
    const int32_t w = static_cast<int32_t>(width), big = L.len(10), mid = L.len(3), small = L.len(2);
    cv.fill(0, 0, w, static_cast<int32_t>(height), kBackground);

    const char* title = "POKOPIA NATIVE";
    text(cv, w / 2 - text_width(title, big) / 2, L.y(250), big, kTitle, title, w);
    const char* sub = failed ? "AVVIO INTERROTTO" : "PS5 - AVVIO IN CORSO";
    text(cv, w / 2 - text_width(sub, mid) / 2, L.y(350), mid, failed ? kError : kDim, sub, w);

    const int32_t px = L.x(120), py = L.y(620), b = L.len(2);
    cv.fill(px, py, L.x(1800) - px, L.y(985) - py, kBorder);
    cv.fill(px + b, py + b, L.x(1800) - px - 2 * b, L.y(985) - py - 2 * b, kPanel);
    const int32_t left = L.x(150), right = L.x(1770);

    // the step, with the time since the start and a spinner on the right
    char buf[256];
    const unsigned secs = static_cast<unsigned>((end_ns - start_ns) / 1000000000u);
    char clock[32];
    std::snprintf(clock, sizeof clock, "%02u:%02u %c", secs / 60, secs % 60, failed ? ' ' : "|/-\\"[now / 250000000u % 4]);
    const int32_t clock_x = right - text_width(clock, mid);
    const int si = static_cast<int>(step);
    if (failed) std::snprintf(buf, sizeof buf, "ERRORE: %s", error);
    else std::snprintf(buf, sizeof buf, "FASE %d/4: %s", si + 1, kSteps[si].name);
    text(cv, left, L.y(645), mid, failed ? kError : kTitle, buf, clock_x - L.len(30));
    text(cv, clock_x, L.y(645), mid, kDim, clock, right + mid);

    // the estimate
    const double pct = estimate(step, static_cast<double>(end_ns - step_ns) / 1e9);
    const int32_t bar_w = L.x(1500) - left, bar_h = L.len(30);
    cv.fill(left, L.y(690), bar_w, bar_h, kBarBack);
    cv.fill(left, L.y(690), static_cast<int32_t>(bar_w * pct / 100.0), bar_h, failed ? kError : kAccent);
    std::snprintf(buf, sizeof buf, failed ? "%d%% - FERMO" : "%d%% (STIMA)", static_cast<int>(pct));
    text(cv, L.x(1530), L.y(695), mid, kTitle, buf, right + mid);

    // what the process does, or what stopped it
    if (failed) {
        text(cv, left, L.y(742), small, kError, log.last_error[0] ? log.last_error : error, right);
        std::snprintf(buf, sizeof buf, "LOG COMPLETO: %s", kLogFile);
        text(cv, left, L.y(768), small, kWarn, buf, right);
    } else {
        const pkp::plat::VmStats vm = pkp::plat::vm_stats();
        const pkp::plat::BigStats heap = pkp::plat::big_stats();
        const pkp::boot::GraphicsProgress gp = pkp::boot::graphics_progress();
        size_t n = 0;
        buf[0] = '\0';
        if (cpu >= 0) n = append(buf, sizeof buf, n, "CPU %d%%   ", cpu);
        n = append(buf, sizeof buf, n, "MEMORIA GUEST %llu MB   HEAP %llu MB   GPU: %llu SUBMIT, %llu DRAW, %llu TEXTURE, %llu PIPELINE",
                   static_cast<unsigned long long>((vm.region_backed + vm.section_backed) >> 20),
                   static_cast<unsigned long long>(heap.bytes >> 20), static_cast<unsigned long long>(gp.submits),
                   static_cast<unsigned long long>(gp.draws), static_cast<unsigned long long>(gp.textures),
                   static_cast<unsigned long long>(gp.pipelines));
        if (gp.shaders_pending > 0) append(buf, sizeof buf, n, ", %d SHADER IN CODA", gp.shaders_pending);
        text(cv, left, L.y(742), small, kText, buf, right);
        n = append(buf, sizeof buf, 0, "LOG: %llu RIGHE", static_cast<unsigned long long>(log.lines));
        if (log.lines) n = append(buf, sizeof buf, n, ", ULTIMA %llu S FA", static_cast<unsigned long long>((now - std::min(now, log.last_ns)) / 1000000000u));
        append(buf, sizeof buf, n, "   STATO: %s", pkp::boot::stage());
        text(cv, left, L.y(768), small, kDim, buf, right);
    }

    // the log's last lines
    cv.fill(left, L.y(796), right - left, L.len(2), kBorder);
    for (int i = 0; i < log.count; ++i) {
        const int at = (log.next - log.count + i + kLogLines) % kLogLines;
        const nxrt::LogLevel lv = log.level[at];
        text(cv, left, L.y(810 + 26 * i), small, lv == nxrt::LogLevel::Error ? kError : lv == nxrt::LogLevel::Warn ? kWarn : kDim,
             log.line[at], right);
    }
}

// ---- the picture on VideoOut, drawn by the CPU: the sequence of the boilerplate's demo renderer, which the stage 1 and 2
// titles ran on the console (third_party/ps5-native-app-boilerplate/src/demo_renderer.cpp)

constexpr uint32_t kWidth = 1920, kHeight = 1080, kTilesPerRow = kWidth / 128;
constexpr size_t kFrameBytes = 0x1000000, kMemoryBytes = 2 * kFrameBytes, kAlignment = 0x200000;
constexpr size_t kUsedBytes = static_cast<size_t>((kHeight + 127) / 128) * kTilesPerRow << 16;
constexpr uint64_t kPixelFormat = UINT64_C(0x8000000022000000);   // bytes R, G, B, A
constexpr int kMemoryWcGarlic = 3, kProtCpuGpuRw = 0x33;

struct VideoOut {
    int handle = -1;
    int64_t phys = 0;
    uint8_t* mem = nullptr;
    int shown = 0;
    int64_t flips = 0;
    std::thread thread;
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false;   // under mu
} g_vo;

// Tiling 0 (PS5_VIDEO_OUT_TILING_64KB_R_X): 64 KiB tiles of 128x128 pixels, 15 to a row; inside a tile the byte offset
// is the XOR of a part from y and a part from x.
uint32_t g_row_bits[128], g_col_bits[128];

class TiledCanvas final : public StatusCanvas {
public:
    explicit TiledCanvas(uint8_t* frame) : frame_(frame) {}
    void fill(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) override {
        const int32_t x0 = std::max(x, 0), y0 = std::max(y, 0);
        const int32_t x1 = std::min(x + w, static_cast<int32_t>(kWidth)), y1 = std::min(y + h, static_cast<int32_t>(kHeight));
        const uint32_t px = 0xff000000u | (rgb & 0xffu) << 16 | (rgb & 0xff00u) | (rgb >> 16 & 0xffu);
        for (int32_t yy = y0; yy < y1; ++yy) {
            uint8_t* tiles = frame_ + (static_cast<size_t>(yy >> 7) * kTilesPerRow << 16);
            const uint32_t row = g_row_bits[yy & 127];
            for (int32_t xx = x0; xx < x1; ++xx)
                *reinterpret_cast<uint32_t*>(tiles + (static_cast<size_t>(xx >> 7) << 16) + (row ^ g_col_bits[xx & 127])) = px;
        }
    }

private:
    uint8_t* frame_;
};

// The display reads the frame from memory: the CPU's writes go out of the caches first.
void flush(const uint8_t* p, size_t n) {
    for (const uint8_t* end = p + n; p < end; p += 64) __asm__ volatile("clflush (%0)" : : "r"(p) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

void videoout_paint(int index) {
    uint8_t* frame = g_vo.mem + static_cast<size_t>(index) * kFrameBytes;
    TiledCanvas cv(frame);
    paint(cv, kWidth, kHeight);
    flush(frame, kUsedBytes);
}

void videoout_loop() {
    std::unique_lock<std::mutex> lk(g_vo.mu);
    for (;;) {
        g_vo.cv.wait_for(lk, std::chrono::milliseconds(250), [] { return g_vo.stop; });
        if (g_vo.stop) break;
        lk.unlock();
        if (sceVideoOutIsFlipPending(g_vo.handle) <= 0) {   // the other frame is not on screen: draw it, then show it
            const int next = g_vo.shown ^ 1;
            videoout_paint(next);
            if (sceVideoOutSubmitFlip(g_vo.handle, next, PS5_VIDEO_OUT_FLIP_VSYNC, ++g_vo.flips) >= 0) g_vo.shown = next;
        }
        lk.lock();
    }
}

// Gives VideoOut back: the painter stopped, no flip pending, the frames unregistered, the handle closed; the memory is
// freed once VideoOut let go of it.
void videoout_close(char* how, size_t how_len) {
    {
        std::lock_guard<std::mutex> lk(g_vo.mu);
        g_vo.stop = true;
    }
    g_vo.cv.notify_all();
    if (g_vo.thread.joinable()) g_vo.thread.join();
    if (g_vo.handle < 0) return;
    for (int i = 0; i < 100 && sceVideoOutIsFlipPending(g_vo.handle) > 0; ++i) pkp::plat::sleep_us(1000);
    int unregistered = sceVideoOutUnregisterBuffers(g_vo.handle, 0);
    for (int i = 0; unregistered == PS5_VIDEO_OUT_ERROR_BUSY && i < 10; ++i) {
        sceVideoOutWaitVblank(g_vo.handle);
        unregistered = sceVideoOutUnregisterBuffers(g_vo.handle, 0);
    }
    const int closed = sceVideoOutClose(g_vo.handle);
    if (unregistered >= 0 || closed >= 0) {
        sceKernelMunmap(g_vo.mem, kMemoryBytes);
        sceKernelReleaseDirectMemory(g_vo.phys, kMemoryBytes);
    }
    std::snprintf(how, how_len, "unregister 0x%x, close 0x%x", static_cast<unsigned>(unregistered), static_cast<unsigned>(closed));
    g_vo.handle = -1;
    g_vo.mem = nullptr;
    g_vo.phys = 0;
}

bool videoout_open(char* why, size_t why_len) {
    const int h = sceVideoOutOpen(PS5_VIDEO_OUT_USER_SYSTEM, 0, 0, nullptr);
    if (h < 0) {
        std::snprintf(why, why_len, "sceVideoOutOpen 0x%x", static_cast<unsigned>(h));
        return false;
    }
    int64_t phys = 0;
    int r = sceKernelAllocateDirectMemory(0, static_cast<int64_t>(sceKernelGetDirectMemorySize()), kMemoryBytes, kAlignment,
                                          kMemoryWcGarlic, &phys);
    if (r < 0) {
        std::snprintf(why, why_len, "sceKernelAllocateDirectMemory 0x%x", static_cast<unsigned>(r));
        sceVideoOutClose(h);
        return false;
    }
    void* mem = nullptr;
    r = sceKernelMapDirectMemory(&mem, kMemoryBytes, kProtCpuGpuRw, 0, phys, kAlignment);
    if (r < 0) {
        std::snprintf(why, why_len, "sceKernelMapDirectMemory 0x%x", static_cast<unsigned>(r));
        sceKernelReleaseDirectMemory(phys, kMemoryBytes);
        sceVideoOutClose(h);
        return false;
    }
    g_vo.handle = h;
    g_vo.phys = phys;
    g_vo.mem = static_cast<uint8_t*>(mem);
    for (uint32_t i = 0; i < 128; ++i) {
        g_row_bits[i] = ((i << 4) & 0x70u) ^ ((i << 5) & 0xf00u) ^ ((i << 9) & 0x1000u) ^ ((i << 8) & 0x4000u);
        g_col_bits[i] = ((i << 2) & 0xcu) ^ ((i << 5) & 0x380u) ^ ((i << 4) & 0x400u) ^ ((i << 6) & 0x800u) ^ ((i << 9) & 0xa000u);
    }
    videoout_paint(0);
    videoout_paint(1);
    sceVideoOutSetFlipRate(h, 0);
    alignas(16) uint8_t attribute[PS5_VIDEO_OUT_ATTRIBUTE_BYTES] = {};
    sceVideoOutSetBufferAttribute2(attribute, kPixelFormat, PS5_VIDEO_OUT_TILING_64KB_R_X, kWidth, kHeight, 0, 0, 0);
    ps5_video_out_buffer frames[2] = {{g_vo.mem, nullptr, {nullptr, nullptr}}, {g_vo.mem + kFrameBytes, nullptr, {nullptr, nullptr}}};
    r = sceVideoOutRegisterBuffers2(h, 0, 0, frames, 2, attribute, 0, nullptr);
    if (r >= 0) r = sceVideoOutSubmitFlip(h, 0, PS5_VIDEO_OUT_FLIP_VSYNC, 0);
    if (r < 0) {
        std::snprintf(why, why_len, "VideoOut buffers 0x%x", static_cast<unsigned>(r));
        char how[64];
        videoout_close(how, sizeof how);
        return false;
    }
    g_vo.shown = 0;
    g_vo.stop = false;
    g_vo.thread = std::thread(videoout_loop);
    return true;
}

// ---- the picture on the game's swapchain

class SwapchainScreen final : public pkp::boot::StatusScreen {
public:
    void paint(uint32_t width, uint32_t height, StatusCanvas& canvas) override { boot_screen::paint(canvas, width, height); }
    void first_frame() override {
        g_first_frame = true;
        Step step;
        uint64_t start_ns, step_ns;
        {
            std::lock_guard<std::mutex> lk(g_state_mu);
            step = g_step;
            start_ns = g_start_ns;
            step_ns = g_step_ns;
        }
        const uint64_t now = pkp::plat::now_ns();
        const double game_s = static_cast<double>(now - step_ns) / 1e9;
        if (step == Step::Game)
            if (FILE* f = std::fopen(kBootSecondsFile, "w")) {
                std::fprintf(f, "%.1f\n", game_s);
                std::fclose(f);
            }
        NXLOG_I("boot", "boot screen: the game's first frame, %.1f s after the start (%.1f s after the game's entry)",
                static_cast<double>(now - start_ns) / 1e9, game_s);
    }
} g_swapchain_screen;

}  // namespace

void start() {
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        g_start_ns = g_step_ns = pkp::plat::now_ns();
    }
    const int hidden = sceSystemServiceHideSplashScreen();
    if (FILE* f = std::fopen(kBootSecondsFile, "r")) {
        double s = 0;
        if (std::fscanf(f, "%lf", &s) == 1 && s >= 1 && s <= 3600) g_expected_s = s;
        std::fclose(f);
    }
    nxrt::log_set_sink(log_sink, nullptr);
    const char* env = std::getenv("PKP_BOOT_SCREEN");
    g_enabled = !(env && std::strcmp(env, "0") == 0);
    char note[192], why[96] = "";
    if (!g_enabled) std::snprintf(note, sizeof note, "boot screen: off (PKP_BOOT_SCREEN=0); splash hidden (0x%x)", static_cast<unsigned>(hidden));
    else if (videoout_open(why, sizeof why)) std::snprintf(note, sizeof note, "boot screen: on VideoOut; splash hidden (0x%x)", static_cast<unsigned>(hidden));
    else std::snprintf(note, sizeof note, "boot screen: VideoOut failed (%s); splash hidden (0x%x)", why, static_cast<unsigned>(hidden));
    pkp_boot_trace(note);
}

void step(Step s) {
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_step = s;
    g_step_ns = pkp::plat::now_ns();
}

void hand_over() {
    if (g_vo.handle < 0) return;
    char how[64] = "";
    videoout_close(how, sizeof how);
    NXLOG_I("boot", "boot screen: VideoOut given back for the graphics (%s)", how);
}

void on_renderer() {
    if (!g_enabled) return;
    g_swapchain_on = pkp::boot::status_screen_start(&g_swapchain_screen);
    if (!g_swapchain_on) NXLOG_W("boot", "boot screen: not on the swapchain (no window renderer, or the game presents already)");
}

void fail(const char* what) {
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        if (!g_failed) {
            g_failed = true;
            g_fail_ns = pkp::plat::now_ns();
            copy_text(g_error, sizeof g_error, what);
        }
    }
    pkp_boot_trace("boot screen: the boot stopped");
    if (!g_enabled) return;
    if (g_swapchain_on && !g_first_frame) return;   // the swapchain's picture shows the error
    if (!g_swapchain_on && pkp::boot::status_screen_start(&g_swapchain_screen)) {
        g_swapchain_on = true;
        return;
    }
    // no window renderer (start_graphics failed) or the game's pictures were up: the error on VideoOut, if it is free
    char why[96];
    if (g_vo.handle < 0 && !videoout_open(why, sizeof why))
        NXLOG_W("boot", "boot screen: the error cannot go on VideoOut (%s)", why);
}

}  // namespace pkp::boot_screen
