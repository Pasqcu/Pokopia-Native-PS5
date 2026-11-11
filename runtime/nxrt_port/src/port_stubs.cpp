// SPDX-License-Identifier: GPL-3.0-or-later
// Functions of the upstream files the port leaves out (CMakeLists.txt): each answers the way its caller expects when the
// feature is unavailable.
#include <memory>
#include <vector>
#include <string>

#include "nxrt/audio_sink.h"
#include "nxrt/extras.h"
#include "nxrt/movie.h"
#include "nxrt/services_api.h"
#include "nxrt/tt.h"
#include "services_internal.h"
#include "tt_internal.h"

namespace nxrt {

namespace audio {
#if !defined(__PROSPERO__)   // the PS5 has a device sink under this name: libSceAudioOut (src/ps5_audio.cpp)
std::shared_ptr<AudioSink> make_xaudio2_sink() { return nullptr; }   // the auto sink selection falls back to the null sink
#endif
std::vector<OutputDevice> list_output_devices() { return {}; }       // no MMDevice enumeration: the default device only
}

namespace media {
bool install_movie_hle() { return false; }   // Switch 1 movie:: on Media Foundation; Pokopia (Switch 2) uses video_codec.cpp
MovieStats movie_stats() { return MovieStats{}; }
}  // namespace media

namespace services {
std::shared_ptr<ipc::Service> make_screenshot_app() { return nullptr; }   // caps:su screenshots (WIC): not registered
namespace swkbdui {
#if !defined(__PROSPERO__)   // the PS5 asks the system's on-screen keyboard (src/ps5_swkbd.cpp)
bool win32_prompt(const Request&, std::string*) { return false; }   // the keyboard prompt answers "cancelled"
#endif
}
}  // namespace services

// test tooling's game probes (tt_game.cpp: SEH memory probes): unavailable, every probe reports failure
namespace tt {
bool read_state(GameState*) { return false; }
void set_call_context(::Ctx*) {}
void set_probe_layout(const ProbeLayout&) {}
void install_game_hooks() {}
namespace detail {
bool dump_objects(const std::string&, const std::string&, std::string* err) { if (err) *err = "not available"; return false; }
bool probe_positions(::Ctx*, const std::string&, const std::string&, std::string* err) { if (err) *err = "not available"; return false; }
bool poke_memory(const std::string&, std::string* err) { if (err) *err = "not available"; return false; }
bool peek_memory(const std::string&, const std::string&, std::string* err) { if (err) *err = "not available"; return false; }
bool cast_rays(::Ctx*, const std::string&, const std::string&, std::string* err) { if (err) *err = "not available"; return false; }
bool watch_start(::Ctx*, int) { return false; }
void watch_stop() {}
void watch_flush(uint64_t) {}
bool give_item(int, int) { return false; }
uint64_t game_base() { return 0; }
void set_game_base(uint64_t) {}
bool warp(::Ctx*, const uint8_t[16], const uint8_t[16]) { return false; }
LoaderState loader_state(uint32_t* pending) { if (pending) *pending = 0; return LoaderState{}; }
bool grant(int64_t, int32_t, int32_t) { return false; }
bool call_guest(::Ctx*, uint64_t, const std::string&, uint64_t*, std::string* err) { if (err) *err = "not available"; return false; }
bool read_mem(uint64_t, void*, size_t) { return false; }
}  // namespace detail
}  // namespace tt

}  // namespace nxrt
