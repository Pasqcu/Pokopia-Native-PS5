// SPDX-License-Identifier: GPL-3.0-or-later
// The PS5's audio output (libSceAudioOut, the main port) as nxrt's host audio sink (include/nxrt/audio_sink.h), in the
// place XAudio2 has on Windows: the "auto" sink selection asks make_xaudio2_sink() for the device sink and falls back to
// the null sink without one. A thread hands the port one grain (256 frames, 5.3 ms at 48 kHz) at a time:
// sceAudioOutOutput returns when the port has taken it, so the queue drains at the device's rate and the audio renderer
// paces itself by the queue level (has_device_clock). [audio] sink = null (NXRT_AUDIO_SINK=null in /app0/env.txt) turns
// the output off.
#if defined(__PROSPERO__)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/log.h"

extern "C" {
int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int32_t sceAudioOutClose(int32_t handle);
int32_t sceAudioOutOutput(int32_t handle, const void* ptr);
void pkp_health_start(void);   // src/ps5_health.cpp
}

namespace nxrt::audio {
namespace {
constexpr const char* kTag = "audio";
constexpr int32_t kUserSystem = 0xff;   // SCE_USER_SERVICE_USER_ID_SYSTEM: the main port is not a player's
constexpr int32_t kPortMain = 0;        // SCE_AUDIO_OUT_PORT_TYPE_MAIN
constexpr uint32_t kS16Stereo = 1;      // SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO
constexpr uint32_t kS16Eight = 2;       // SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH: FL FR FC LFE SL SR + two extended channels
constexpr uint32_t kGrain = 256;        // frames per output call: the smallest grain the port takes
constexpr uint32_t kQueueMs = 250;      // room in the queue; the renderer keeps about 40 ms in it
constexpr int kLoggedErrors = 3;

class SceAudioOutSink final : public AudioSink {
public:
    ~SceAudioOutSink() override { close(); }

    bool open(uint32_t sample_rate, uint32_t channels) override {
        close();
        if (sample_rate != 48000 || (channels != 2 && channels != 6)) {
            NXLOG_W(kTag, "sceAudioOut: the main port plays 48000 Hz in 2 or 6 channels, not %u Hz in %u", sample_rate, channels);
            return false;
        }
        static std::once_flag init;
        std::call_once(init, [] {
            const int32_t r = sceAudioOutInit();   // "already initialised" is an answer too: the open tells
            if (r != 0) NXLOG_I(kTag, "sceAudioOutInit = %#x", static_cast<unsigned>(r));
        });
        const int32_t h = sceAudioOutOpen(kUserSystem, kPortMain, 0, kGrain, sample_rate, channels == 2 ? kS16Stereo : kS16Eight);
        if (h < 0) {
            NXLOG_W(kTag, "sceAudioOutOpen(main port, grain %u, %u Hz, %u channels) = %#x", kGrain, sample_rate, channels, static_cast<unsigned>(h));
            return false;
        }
        std::lock_guard<std::mutex> lk(mu_);
        handle_ = h;
        channels_ = channels;
        out_channels_ = channels == 2 ? 2 : 8;
        capacity_ = sample_rate * kQueueMs / 1000;
        ring_.assign(static_cast<size_t>(capacity_) * channels_, 0);
        head_ = count_ = 0;
        started_ = dry_ = stop_ = false;
        low_ = UINT32_MAX;
        stats_.sample_rate = sample_rate;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (handle_ < 0) return;
            stop_ = true;
        }
        if (thread_.joinable()) thread_.join();   // at most one grain away
        sceAudioOutClose(handle_);
        SinkStats s;
        {
            std::lock_guard<std::mutex> lk(mu_);
            handle_ = -1;
            s = stats_locked();
        }
        if (s.frames_written) NXLOG_I(kTag, "%s", format_stats(name(), s).c_str());   // not for the probe of the sink selection
    }

    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.frames_written += frames;
        if (handle_ < 0 || !pcm) {
            stats_.frames_dropped += frames;
            return 0;
        }
        const uint32_t take = std::min(frames, capacity_ - count_);
        const uint32_t tail = (head_ + count_) % capacity_;
        const uint32_t first = std::min(take, capacity_ - tail);
        std::memcpy(&ring_[static_cast<size_t>(tail) * channels_], pcm, static_cast<size_t>(first) * channels_ * sizeof(int16_t));
        std::memcpy(&ring_[0], pcm + static_cast<size_t>(first) * channels_, static_cast<size_t>(take - first) * channels_ * sizeof(int16_t));
        count_ += take;
        stats_.frames_accepted += take;
        stats_.frames_dropped += frames - take;
        stats_.queued_peak_frames = std::max(stats_.queued_peak_frames, count_);
        if (take) started_ = true;
        return take;
    }

    uint32_t queued_frames() override {
        std::lock_guard<std::mutex> lk(mu_);
        return count_;
    }
    bool has_device_clock() const override { return true; }
    const char* name() const override { return "sceAudioOut"; }
    SinkStats stats() override {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_locked();
    }

private:
    SinkStats stats_locked() const {
        SinkStats s = stats_;
        s.queued_low_frames = low_ == UINT32_MAX ? 0 : low_;
        return s;
    }

    // One grain per turn: what the queue holds (silence for the rest), in the port's channel layout, at the player's volume.
    void run() {
        std::vector<int16_t> grain(static_cast<size_t>(kGrain) * out_channels_);
        int errors = 0;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (stop_) return;
                const uint32_t n = std::min(count_, kGrain);
                if (started_) {
                    low_ = std::min(low_, count_);
                    if (n < kGrain && !dry_) stats_.underruns++;   // counted when the queue runs dry, not for every grain it stays so
                    dry_ = n < kGrain;
                }
                std::fill(grain.begin(), grain.end(), int16_t{0});
                for (uint32_t i = 0; i < n; ++i) {
                    const int16_t* src = &ring_[static_cast<size_t>((head_ + i) % capacity_) * channels_];
                    std::memcpy(&grain[static_cast<size_t>(i) * out_channels_], src, channels_ * sizeof(int16_t));   // 6 of 8: the extended pair stays silent
                }
                head_ = (head_ + n) % capacity_;
                count_ -= n;
            }
            const float v = effective_volume();
            if (v < 0.999f)
                for (int16_t& s : grain) s = static_cast<int16_t>(static_cast<float>(s) * v);
            const int32_t r = sceAudioOutOutput(handle_, grain.data());   // returns when the port has taken the grain
            if (r < 0) {
                if (errors++ < kLoggedErrors) NXLOG_W(kTag, "sceAudioOutOutput(handle %d) = %#x", handle_, static_cast<unsigned>(r));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));   // no device clock to wait on
            }
        }
    }

    std::mutex mu_;
    std::thread thread_;
    std::vector<int16_t> ring_;   // capacity_ frames of channels_ samples
    SinkStats stats_;
    int32_t handle_ = -1;
    uint32_t channels_ = 2, out_channels_ = 2, capacity_ = 0, head_ = 0, count_ = 0, low_ = UINT32_MAX;
    bool started_ = false, dry_ = false, stop_ = false;
};
}  // namespace

// The device sink of this platform, under the name the sink selection asks for (src/services/audio_sink.cpp).
std::shared_ptr<AudioSink> make_xaudio2_sink() {
    pkp_health_start();   // src/ps5_health.cpp: the first call the running game makes into the port's PS5 files
    return std::make_shared<SceAudioOutSink>();
}

}  // namespace nxrt::audio
#endif
