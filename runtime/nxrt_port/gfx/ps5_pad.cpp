// SPDX-License-Identifier: GPL-3.0-or-later
// PS5: the DualSense as player 1's pad (include/pkp_boot.h: ps5_pad_start). Nothing else brings it to the game: SDL's PS5 build has
// no joystick driver for it (no HIDAPI: SDL_JOYSTICK_DUMMY), and play mode polls its pads in its frame hook, which the port does not
// install (nxrt_register_game_pkp_core names no frame update). This input source wraps play mode's: the hid service's sampler
// thread calls poll() every 5 ms, which reads the pad (scePadReadState) and lays it over player 1 the way play mode's poll_pads
// does, with play mode's mapping and [pad] settings (play/pad_map.h): buttons OR, the stick with the larger deflection wins. The
// game's rumble goes to the pad's two motors (scePadSetVibration). The motion sensors are not passed on: the six-axis sample stays
// at rest, as before.
// libScePad's sample and calls as heydemoura/EglantinePS5 declares them (third_party/ps5_pad.hpp); its headless/pad.cpp opens the
// pad the same way.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"
#include "nxrt/play.h"
#include "pkp_boot.h"
#include "play/pad_map.h"
#include "play/play_types.h"

namespace pkp::ps5pad {

struct Data {                               // one sample, 120 bytes
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;                 // 0..255, 128 = centre, y down = +
    uint8_t l2, r2, reserved0[2];           // the analog triggers, 0..255
    float orientation[4];
    float acceleration[3];                  // in G
    float angular_velocity[3];              // rad/s
    uint8_t touch[24];
    int32_t connected;
    uint64_t timestamp_us;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t reserved1[2];
    uint8_t device_unique_data_length;
    uint8_t device_unique_data[12];
};
static_assert(sizeof(Data) == 120 && offsetof(Data, touch) == 0x34 && offsetof(Data, connected) == 0x4C &&
              offsetof(Data, timestamp_us) == 0x50, "libScePad's sample layout");

struct Vibration {
    uint8_t large_motor, small_motor;
};

}  // namespace pkp::ps5pad

extern "C" {
int32_t sceUserServiceInitialize(void* params);
int32_t sceUserServiceGetForegroundUser(int32_t* user);
int32_t sceUserServiceGetInitialUser(int32_t* user);
int32_t scePadInit();
int32_t scePadOpen(int32_t user, int32_t port_type, int32_t index, const void* params);
int32_t scePadGetHandle(int32_t user, int32_t port_type, int32_t index);
int32_t scePadClose(int32_t handle);
int32_t scePadReadState(int32_t handle, pkp::ps5pad::Data* data);
int32_t scePadSetVibrationMode(int32_t handle, int32_t mode);
int32_t scePadSetVibration(int32_t handle, const pkp::ps5pad::Vibration* vibration);
}

namespace pkp::boot {
namespace {

using nxrt::hid::InputSnapshot;
using nxrt::hid::InputSource;
using nxrt::hid::NpadInput;
using nxrt::hid::StickValue;

constexpr const char* kTag = "ps5pad";
constexpr int32_t kPortStandard = 0, kVibrationCompatible = 2;
constexpr uint32_t kCreate = 0x1, kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80,
                   kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000,
                   kTouchPad = 0x100000, kIntercepted = 0x80000000u;   // Intercepted: the system has the input (its menu is up)
constexpr int kPollsPerOpenTry = 200;       // without a pad handle, another try about once a second (a poll every 5 ms)
constexpr int kLoggedOpenFailures = 3, kLoggedChanges = 64, kLoggedRumbles = 8, kLoggedErrors = 8;

std::atomic<float> g_vib[2][2];             // the game's rumble, [hand][low, high] amplitude 0..1 (written on the hid service's IPC threads)

void vib_sink(uint32_t slot, int hand, const nxrt::hid::VibrationValue& v, void*) {
    if (slot != 0 || hand < 0 || hand > 1) return;
    g_vib[hand][0].store(v.amp_low, std::memory_order_relaxed);
    g_vib[hand][1].store(v.amp_high, std::memory_order_relaxed);
}

// The DualSense in SDL's gamepad terms (platform::GamepadRaw), the input of play mode's mapping. The touchpad click is BACK (Switch
// Minus) as well as Create, as in EglantinePS5. face_label stays {A, B, X, Y}: cross, circle, square, triangle.
nxrt::platform::GamepadRaw to_raw(const ps5pad::Data& d) {
    static constexpr struct {
        uint32_t pad, sdl;
    } kMap[] = {{kCross, 0}, {kCircle, 1}, {kSquare, 2}, {kTriangle, 3}, {kCreate, 4}, {kTouchPad, 4}, {kOptions, 6}, {kL3, 7},
                {kR3, 8}, {kL1, 9}, {kR1, 10}, {kUp, 11}, {kDown, 12}, {kLeft, 13}, {kRight, 14}, {kTouchPad, 20}};
    nxrt::platform::GamepadRaw r;
    for (const auto& m : kMap)
        if (d.buttons & m.pad) r.buttons |= 1u << m.sdl;
    auto stick = [](uint8_t v) { return static_cast<int16_t>(v * 257 - 32768); };   // as SDL's HIDAPI driver scales a DualSense stick
    auto trigger = [](uint8_t v) { return static_cast<int16_t>(v * 32767 / 255); };
    r.axes[0] = stick(d.lx);
    r.axes[1] = stick(d.ly);
    r.axes[2] = stick(d.rx);
    r.axes[3] = stick(d.ry);
    r.axes[4] = trigger(d.l2);
    r.axes[5] = trigger(d.r2);
    return r;
}

StickValue larger(StickValue a, StickValue b) {
    auto len2 = [](StickValue v) { return static_cast<int64_t>(v.x) * v.x + static_cast<int64_t>(v.y) * v.y; };
    return len2(a) >= len2(b) ? a : b;
}

class DualSenseSource final : public InputSource {
public:
    DualSenseSource(std::shared_ptr<InputSource> inner, const nxrt::play::PadSettings& cfg) : inner_(std::move(inner)), cfg_(cfg) {}

    std::shared_ptr<InputSource> inner() const { return inner_; }

    void open_pad() {
        std::lock_guard<std::mutex> lk(mu_);
        open();
    }

    void close_pad() {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
        if (handle_ < 0) return;
        const ps5pad::Vibration stop{0, 0};
        scePadSetVibration(handle_, &stop);
        scePadClose(handle_);
        handle_ = -1;
    }

    void poll(InputSnapshot& out) override {
        if (inner_) {
            inner_->poll(out);
        } else {
            out.npad[0].connected = true;
            out.npad[0].style = nxrt::hid::NpadStyle::FullKey;
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return;
        if (handle_ < 0) {
            if (++polls_since_open_ < kPollsPerOpenTry) return;
            polls_since_open_ = 0;
            open();
            if (handle_ < 0) return;
        }
        ps5pad::Data d;
        std::memset(&d, 0, sizeof d);
        const int32_t r = scePadReadState(handle_, &d);
        if (r < 0) {
            if (read_errors_++ < kLoggedErrors) NXLOG_W(kTag, "scePadReadState(handle %d) = %#x", handle_, static_cast<unsigned>(r));
            return;
        }
        const bool connected = d.connected != 0, intercepted = (d.buttons & kIntercepted) != 0;
        if (!have_sample_) {
            have_sample_ = true;
            NXLOG_I(kTag, "first sample: connected %d (pads %u), buttons %08x, sticks %u,%u %u,%u, triggers %u %u", d.connected,
                    d.connected_count, d.buttons, d.lx, d.ly, d.rx, d.ry, d.l2, d.r2);
        } else {
            if (connected != connected_) NXLOG_I(kTag, "DualSense %s", connected ? "connected" : "disconnected");
            if (intercepted != intercepted_) NXLOG_I(kTag, "input %s the system", intercepted ? "taken by" : "back from");
        }
        if (connected && !connected_) sent_large_ = sent_small_ = -1;   // a pad that comes (back) gets the current rumble
        connected_ = connected;
        intercepted_ = intercepted;
        if (connected) rumble();
        if (!connected || intercepted) return;   // player 1 stays as play mode reports it: idle
        NpadInput m;
        nxrt::play::map_pad(to_raw(d), cfg_, &m);
        NpadInput& n = out.npad[0];
        n.buttons |= m.buttons;
        n.left = larger(n.left, m.left);
        n.right = larger(n.right, m.right);
        if (d.buttons != last_buttons_ && changes_logged_ < kLoggedChanges) {
            ++changes_logged_;
            NXLOG_I(kTag, "buttons %08x -> Switch %llx (left %d,%d right %d,%d)", d.buttons, static_cast<unsigned long long>(m.buttons),
                    m.left.x, m.left.y, m.right.x, m.right.y);
        }
        last_buttons_ = d.buttons;
    }

private:
    // Player 1's pad: the foreground user's (the initial user's when that fails). Caller holds mu_.
    void open() {
        int32_t user = -1;
        int32_t r = sceUserServiceGetForegroundUser(&user);
        if (r < 0 || user < 0) r = sceUserServiceGetInitialUser(&user);
        if (r < 0 || user < 0) {
            if (open_failures_++ < kLoggedOpenFailures) NXLOG_W(kTag, "no user for player 1 (%#x)", static_cast<unsigned>(r));
            return;
        }
        int32_t h = scePadOpen(user, kPortStandard, 0, nullptr);
        if (h < 0) h = scePadGetHandle(user, kPortStandard, 0);   // a handle the process holds already
        if (h < 0) {
            if (open_failures_++ < kLoggedOpenFailures) NXLOG_W(kTag, "scePadOpen(user %d) = %#x", user, static_cast<unsigned>(h));
            return;
        }
        handle_ = h;
        sent_large_ = sent_small_ = -1;
        const int32_t vm = scePadSetVibrationMode(h, kVibrationCompatible);
        NXLOG_I(kTag, "player 1: user %d, pad handle %d (vibration mode %#x)", user, h, static_cast<unsigned>(vm));
    }

    // The game's rumble on the two motors: the low band on the large one, the high band on the small one (play mode's map_rumble,
    // both hands' strongest). Sent when it changes; the pad keeps a value until the next. Caller holds mu_.
    void rumble() {
        const float lo_amp = std::max(g_vib[0][0].load(std::memory_order_relaxed), g_vib[1][0].load(std::memory_order_relaxed));
        const float hi_amp = std::max(g_vib[0][1].load(std::memory_order_relaxed), g_vib[1][1].load(std::memory_order_relaxed));
        uint16_t lo = 0, hi = 0;
        nxrt::play::map_rumble(lo_amp, hi_amp, cfg_, &lo, &hi);
        const int large = lo >> 8, small = hi >> 8;
        if (large == sent_large_ && small == sent_small_) return;
        sent_large_ = large;
        sent_small_ = small;
        const ps5pad::Vibration v{static_cast<uint8_t>(large), static_cast<uint8_t>(small)};
        const int32_t r = scePadSetVibration(handle_, &v);
        if (rumbles_logged_ < kLoggedRumbles || (r < 0 && rumble_errors_ < kLoggedErrors)) {
            ++rumbles_logged_;
            if (r < 0) ++rumble_errors_;
            NXLOG_I(kTag, "rumble large %d small %d: %#x", large, small, static_cast<unsigned>(r));
        }
    }

    const std::shared_ptr<InputSource> inner_;
    const nxrt::play::PadSettings cfg_;
    std::mutex mu_;
    int32_t handle_ = -1;
    bool closed_ = false;
    int polls_since_open_ = 0, open_failures_ = 0, read_errors_ = 0;
    bool have_sample_ = false, connected_ = false, intercepted_ = false;
    uint32_t last_buttons_ = 0;
    int changes_logged_ = 0;
    int sent_large_ = -1, sent_small_ = -1, rumbles_logged_ = 0, rumble_errors_ = 0;
};

std::shared_ptr<DualSenseSource> g_source;

}  // namespace

void ps5_pad_start() {
    if (g_source) return;
    const int32_t us = sceUserServiceInitialize(nullptr);   // non-zero when it runs already: not an error here
    const int32_t pi = scePadInit();
    NXLOG_I(kTag, "sceUserServiceInitialize %#x, scePadInit %#x", static_cast<unsigned>(us), static_cast<unsigned>(pi));
    nxrt::play::Settings s;
    std::string err;
    const std::string path = nxrt::play::settings_path();
    if (!nxrt::play::load_settings(path, &s, &err) && !err.empty())
        NXLOG_W(kTag, "%s: %s: the default [pad] settings apply", path.c_str(), err.c_str());
    auto src = std::make_shared<DualSenseSource>(nxrt::hid::input_source(), s.pad);
    src->open_pad();
    nxrt::hid::set_vibration_sink(&vib_sink, nullptr);   // play mode's sink feeds its own pad poll, which does not run here
    nxrt::hid::set_input_source(src);
    g_source = std::move(src);
}

void ps5_pad_stop() {
    if (!g_source) return;
    nxrt::hid::set_vibration_sink(nullptr);
    if (nxrt::hid::input_source() == g_source) nxrt::hid::set_input_source(g_source->inner());
    g_source->close_pad();
    g_source.reset();
}

}  // namespace pkp::boot
