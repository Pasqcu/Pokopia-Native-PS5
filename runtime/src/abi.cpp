// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime side of nxrecomp's generated-code ABI (ctx.h "runtime-provided"): indirect dispatch, imports between
// modules, host (HLE) functions at placeholder addresses, traps, SVCs and system registers; and guest calls from the host.
//
// Dispatch layout (Zen 2): the per-module function tables are SoA in the generated code (addresses sorted, one FnEntry
// per function); in front of them a direct-mapped cache of 4096 {addr, fn} pairs = 64 KiB, two entries per 64-byte line,
// sized to stay in L2 (512 KiB per core). Hits take one predictable branch.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pkp/platform.h"
#include "pkp/rt.h"

namespace pkp {

namespace {

struct HleSlot {
    const char* name;
    HostFn fn;
    uint32_t calls;
};
constexpr int kMaxHle = 2048;
HleSlot g_hle[kMaxHle];
int g_nhle;

struct alignas(16) CacheEntry {
    uint64_t addr;
    void (*fn)(Ctx*);
};
constexpr unsigned kCacheSize = 4096;
alignas(64) CacheEntry g_cache[kCacheSize];

alignas(64) Ctx g_ctx;   // single guest thread during bring-up; one cache line boundary of its own
uint64_t g_arena_next, g_stack_top, g_tls;
bool g_env;
char g_trap[192];
uint32_t g_svc_calls, g_unknown_mrs;

void (*lookup_slow(uint64_t addr))(Ctx*) {
    for (int m = 0; m < module_count(); m++) {
        const NxModule* d = module_at(m)->desc;
        if (addr < d->base || addr >= d->text_end || d->nfns == 0) continue;
        int lo = 0, hi = d->nfns;
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (d->fns[mid].addr < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < d->nfns && d->fns[lo].addr == addr) return d->fns[lo].fn;
        return nullptr;
    }
    return nullptr;
}

inline void (*lookup(uint64_t addr))(Ctx*) {
    CacheEntry& e = g_cache[(addr >> 2) & (kCacheSize - 1)];
    if (__builtin_expect(e.addr == addr, 1)) return e.fn;
    void (*f)(Ctx*) = lookup_slow(addr);
    if (f) { e.addr = addr; e.fn = f; }
    return f;
}

void hle_call(Ctx* c, int i) {
    HleSlot& s = g_hle[i];
    if (s.fn) { s.calls++; s.fn(c); return; }
    if (s.calls++ < 3)
        log("[hle] %s not implemented (x0=%#llx x1=%#llx lr=%#llx): returning 0", s.name, (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[30]);
    c->x[0] = 0;
}

}  // namespace

uint64_t hle_register(const char* name, HostFn fn) {
    for (int i = 0; i < g_nhle; i++)
        if (!std::strcmp(g_hle[i].name, name)) {
            if (fn) g_hle[i].fn = fn;
            return kHleBase + kHleStride * (uint64_t)i;
        }
    if (g_nhle == kMaxHle) { log("[hle] table full"); std::abort(); }
    g_hle[g_nhle] = HleSlot{name, fn, 0};
    return kHleBase + kHleStride * (uint64_t)g_nhle++;
}

uint64_t arena_alloc(uint64_t size, uint64_t align) {
    uint64_t a = (g_arena_next + align - 1) & ~(align - 1);
    if (a + size > kArenaBase + kArenaSize) { log("[guest] arena exhausted"); std::abort(); }
    g_arena_next = a + size;
    std::memset(reinterpret_cast<void*>(a), 0, size);
    return a;
}

bool guest_env_init() {
    if (g_env) return true;
    char how[160];
    if (!plat::vm_reserve(kArenaBase, kArenaSize, how, sizeof how) ||
        !plat::vm_commit(kArenaBase, kArenaSize, plat::kReadWrite, how, sizeof how)) {
        log("[guest] cannot map the arena at %#llx: %s", (unsigned long long)kArenaBase, how);
        return false;
    }
    g_arena_next = kArenaBase;
    const uint64_t stack = arena_alloc(0x100000, 0x4000);
    g_stack_top = stack + 0x100000;
    g_tls = arena_alloc(0x1000, 0x1000);   // Horizon TLS block: IPC buffer at +0, thread pointer at +0x1F8
    g_env = true;
    log("[guest] arena %#llx (%s), stack top %#llx, TLS %#llx", (unsigned long long)kArenaBase, how,
        (unsigned long long)g_stack_top, (unsigned long long)g_tls);
    return true;
}

Ctx* guest_ctx() { return &g_ctx; }

uint64_t call_guest(uint64_t addr, const uint64_t* args, int nargs) {
    Ctx* c = &g_ctx;
    std::memset(c->x, 0, sizeof c->x);
    for (int i = 0; i < nargs && i < 8; i++) c->x[i] = args[i];
    c->x[31] = g_stack_top - 0x100;
    c->x[30] = 0;
    c->tpidrro = g_tls;
    c->tpidr = 0;
    c->ex_addr = ~0ull;
    c->in_import = nullptr;
    void (*f)(Ctx*) = lookup(addr);
    if (!f) {
        std::snprintf(g_trap, sizeof g_trap, "no lifted function at %#llx", (unsigned long long)addr);
        __builtin_trap();
    }
    f(c);
    return c->x[0];
}

const char* last_trap() { return g_trap[0] ? g_trap : nullptr; }
void clear_trap() { g_trap[0] = 0; }
uint32_t svc_calls() { return g_svc_calls; }

}  // namespace pkp

using namespace pkp;

extern "C" {

void call_indirect(Ctx* c, uint64_t addr) {
    if (__builtin_expect(addr - kHleBase < kHleStride * (uint64_t)kMaxHle, 0)) {
        const int i = (int)((addr - kHleBase) / kHleStride);
        if (i < g_nhle) { hle_call(c, i); return; }
    }
    void (*f)(Ctx*) = lookup(addr);
    if (__builtin_expect(f != nullptr, 1)) { f(c); return; }
    std::snprintf(g_trap, sizeof g_trap, "indirect branch to %#llx (no lifted function; lr=%#llx, in f_%llx)",
                  (unsigned long long)addr, (unsigned long long)c->x[30], (unsigned long long)c->fn);
    __builtin_trap();
}

void host_import(Ctx* c, uint32_t import_id) {
    const uint32_t mi = NX_IMPORT_MODULE(import_id), si = NX_IMPORT_SYM(import_id);
    const NxModule* d = nullptr;
    for (int m = 0; m < module_count(); m++)
        if (module_at(m)->desc->index == mi) d = module_at(m)->desc;
    const char* name = d && (int)si < d->nsyms ? d->syms[si].name : nullptr;
    const uint64_t addr = name ? find_symbol(name) : 0;
    if (addr) { call_indirect(c, addr); return; }
    call_indirect(c, hle_register(name ? name : "?", nullptr));
}

void guest_trap(const char* what, uint64_t addr) {
    std::snprintf(g_trap, sizeof g_trap, "guest trap: %s at %#llx", what ? what : "?", (unsigned long long)addr);
    __builtin_trap();
}

void guest_unimpl(uint64_t pc, uint32_t insn, const char* what) {
    std::snprintf(g_trap, sizeof g_trap, "untranslated instruction %08x (%s) at %#llx", insn, what ? what : "?",
                  (unsigned long long)pc);
    __builtin_trap();
}

// The Horizon kernel is the next stage (nxrt's svc table ported to this platform layer). During bring-up an SVC is logged
// and answered with "not implemented" (0xF601 = kernel module 1, description 123), which SDK code checks and reports.
void guest_svc(Ctx* c, uint32_t number) {
    if (g_svc_calls++ < 16)
        log("[svc] 0x%02x from f_%llx (x0=%#llx x1=%#llx): not implemented in the bring-up runtime", number,
            (unsigned long long)c->fn, (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
    c->x[0] = 0xF601;
}

uint64_t guest_mrs(Ctx* c, uint32_t sysreg) {
    if (g_unknown_mrs++ < 8) log("[sysreg] mrs %#x in f_%llx -> 0", sysreg, (unsigned long long)c->fn);
    return 0;
}

void guest_msr(Ctx* c, uint32_t sysreg, uint64_t value) { (void)c; (void)sysreg; (void)value; }
void nx_site_inline(Ctx* c, uint64_t addr) { (void)c; (void)addr; }
int nx_site_midhook(Ctx* c, uint64_t addr) { (void)c; (void)addr; return 0; }
int nx_hook_entry(Ctx* c, uint64_t addr) { (void)c; (void)addr; return 0; }
void stx_diag(Ctx* c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {
    (void)c; (void)a; (void)expected; (void)seen; (void)v; (void)bits;
}
// No C++ exception crosses the self-test's guest calls; the unwinder is part of the HLE runtime port (nxrt src/eh).
void nx_catch_call(Ctx* c, void (*body)(Ctx*, uint64_t)) { body(c, 0); }

}  // extern "C"
