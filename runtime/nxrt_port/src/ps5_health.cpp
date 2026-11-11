// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 title: a health line in the log and both logs forced onto the disk, for the crashes that take the whole console
// down. nxrt's log is flushed at a warning or every 100 lines, and a kernel panic also loses what the file system had not
// written yet: after such a crash the last half minute of nxrt.log and stderr.log was missing, the part that tells what
// the game was doing. PKP_HEALTH_MS=<milliseconds> in /app0/env.txt turns this on (unset or 0: off): every period the
// logs are flushed and fsync'ed, and once a second a line tells the memory left: flexible memory (the host heap's budget,
// about 130 MiB after the executable), direct memory, the big heap and the lazily backed guest memory.
#if defined(__PROSPERO__)
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "nxrt/log.h"
#include "pkp/platform.h"

extern "C" {
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end, size_t alignment, int64_t* phys_out, size_t* size_out);
}

namespace {
constexpr const char* kTag = "health";
constexpr unsigned long long kMiB = 1ull << 20;

void sync_file(const char* path) {   // opened every time: the runtime replaces nxrt.log when it starts
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

void* health_thread(void*) {
    const char* e = std::getenv("PKP_HEALTH_MS");
    int period = e ? std::atoi(e) : 0;
    if (period <= 0) return nullptr;
    if (period < 20) period = 20;
    NXLOG_I(kTag, "on: the logs are flushed and synced every %d ms, a memory line every second", period);
    for (int since_line = 1000;; since_line += period) {
        if (since_line >= 1000) {
            since_line = 0;
            const pkp::plat::MemInfo mi = pkp::plat::mem_info();
            const pkp::plat::VmStats vs = pkp::plat::vm_stats();
            const pkp::plat::BigStats bs = pkp::plat::big_stats();
            int64_t phys = 0;
            size_t direct_free = 0;
            const int r = sceKernelAvailableDirectMemorySize(0, static_cast<int64_t>(sceKernelGetDirectMemorySize()), 0, &phys, &direct_free);
            NXLOG_I(kTag, "flexible free %llu MiB; direct largest free block %llu MiB (%#x); big heap %llu MiB in %llu blocks (peak %llu MiB); "
                          "guest backed %llu MiB in regions, %llu of %llu MiB committed in sections, %llu faults",
                    mi.flexible_available / kMiB, static_cast<unsigned long long>(direct_free) / kMiB, static_cast<unsigned>(r), bs.bytes / kMiB,
                    static_cast<unsigned long long>(bs.blocks), bs.peak_bytes / kMiB, vs.region_backed / kMiB, vs.section_backed / kMiB,
                    vs.section_committed / kMiB, static_cast<unsigned long long>(vs.faults));
        }
        nxrt::log_flush();
        sync_file("/app0/logs/nxrt.log");
        ::fsync(::fileno(stderr));   // /app0/logs/stderr.log: RADV's lines and the warnings
        ::usleep(static_cast<useconds_t>(period) * 1000);
    }
    return nullptr;
}

}  // namespace

// Starts the thread, once. Called from the running game, not from a constructor (the title's threads get their stacks from
// the platform layer): for now by the audio device sink when the game opens its output (ps5_audio.cpp), about ten seconds
// in; the title's main (runtime/platform/prospero/title_gfx.cc) is the place for it.
extern "C" void pkp_health_start(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, [] {
        pthread_t t;
        if (pthread_create(&t, nullptr, health_thread, nullptr) == 0) pthread_detach(t);
    });
}
#endif
