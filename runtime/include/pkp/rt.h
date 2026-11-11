// SPDX-License-Identifier: GPL-3.0-or-later
// Pokopia Native on PS5: bring-up runtime. Loads the game's NSO modules at the bases the lifter used, applies their
// relocations, links them to each other by symbol name, and runs recompiled guest code natively. It implements the
// generated-code ABI of nxrecomp (third_party/pokopia-recomp/recomp/include/nxrecomp/ctx.h: call_indirect, host_import,
// guest_svc, ...). The full Horizon HLE (kernel, IPC services, NVN on Vulkan) is the next stage: docs/ROADMAP.md.
#pragma once

#include <cstddef>
#include <cstdint>

#include <nxrecomp/ctx.h>

namespace pkp {

// ---- log: one line per call, to the console sinks and the log file (if open)
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_set_file(const char* path);
const char* log_file();

// ---- address space of the bring-up (the lifted bases are fixed; see scripts/lift.sh)
constexpr uint64_t kMainBase = 0x7100000000ull;
constexpr uint64_t kSdkBase = 0x7500000000ull;
constexpr uint64_t kSubsdk0Base = 0x7600000000ull;
constexpr uint64_t kRtldBase = 0x7700000000ull;
constexpr uint64_t kArenaBase = 0x7E00000000ull;   // stacks, TLS blocks, test buffers (bump allocated)
constexpr uint64_t kArenaSize = 0x0008000000ull;   // 128 MiB
constexpr uint64_t kHleBase = 0x7F00000000ull;     // placeholder addresses of host functions (never mapped)
constexpr uint64_t kHleStride = 16;

// ---- modules
struct LoadedModule {
    const NxModule* desc;      // the lifted code's descriptor (nxmod_<name>)
    uint64_t base, image_size, bss_size, mapped_size;
    uint32_t relocs, relative, bound, unresolved, tls;
    char build_id[41];
};
// Map and relocate one NSO at desc->base; false with a reason in the log.
bool load_module(const NxModule* desc, const char* nso_path);
// After every module is loaded: bind GLOB_DAT / JUMP_SLOT / ABS64 relocations by name across the loaded modules.
void link_modules();
int module_count();
const LoadedModule* module_at(int i);
// Address of a defined dynamic symbol in any loaded module, 0 if none.
uint64_t find_symbol(const char* name);

// ---- host functions callable from guest code (an indirect call to their placeholder address runs them)
using HostFn = void (*)(Ctx* c);
uint64_t hle_register(const char* name, HostFn fn);

// ---- guest execution
uint64_t arena_alloc(uint64_t size, uint64_t align = 16);   // guest-visible zeroed memory
bool guest_env_init();                                      // arena, a 1 MiB guest stack, a TLS block
Ctx* guest_ctx();                                           // the calling thread's context (fresh registers each call_guest)
// Calls the guest function at `addr` with integer arguments x0..x7 (and the context's v registers as set by the caller);
// returns x0. The function runs until its own `ret`.
uint64_t call_guest(uint64_t addr, const uint64_t* args, int nargs);
const char* last_trap();   // what the last guest trap / failed dispatch reported, or nullptr
void clear_trap();
uint32_t svc_calls();

// ---- the CPU self-test (runtime/src/selftest.cpp): lines of results, a summary
struct SelfTestReport {
    int passed, failed, skipped;
    char lines[48][112];
    int nlines;
    double sha_mb_s_guest, sha_mb_s_host;
};
void run_selftest(SelfTestReport* r, const char* exefs_dir);

// ---- address-space probe of the nxrt layout (main, sdk, heap, alias, stacks): which reservations the platform grants
void run_layout_probe(SelfTestReport* r);

}  // namespace pkp
