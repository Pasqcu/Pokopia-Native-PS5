// SPDX-License-Identifier: GPL-3.0-or-later
// Large host blocks of the PS5 title from direct memory (platform.h: big_alloc), and the allocator wrappers that route
// the title's own malloc / free / calloc / realloc / aligned allocations (C and C++: libc++'s operator new calls malloc)
// of 1 MiB and more there. scripts/make_ppsa.sh links with --wrap=<each>, so only references from the title's objects
// and static libraries are redirected; the system libraries keep their malloc, and a block they allocated (strdup, ...)
// goes back to them when the title frees it (it lies outside the arena).
//
// Why: malloc draws on flexible memory, ~448 MiB per title, and the executable's own segments are charged to it (the
// stage-1 log: 448 MiB minus a 49 MB eboot = 399 MiB free). With the whole game linked in (~294 MiB loaded) about
// 140 MiB remain, less than one module image plus its file buffer: the first console run aborted on std::bad_alloc
// while reading `main` (28 MB). Direct memory is the ~12 GiB pool the guest memory comes from.
//
// The arena: up to 16 GiB of address space reserved at a fixed address outside the guest windows, in 64 KiB granules; its
// tables (free bitmap, block length, physical address) live in its first granules, so they cost no flexible memory.
#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "pkp/platform.h"

extern "C" {
int sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type,
                                  int64_t* physical_address);
int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t physical_address, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelMunmap(void* addr, size_t len);
size_t sceKernelGetDirectMemorySize(void);
}

namespace pkp::plat {
namespace {

constexpr int kMapFixed = 0x10;
constexpr int kProtRW = 0x01 | 0x02;
constexpr int kMemoryTypes[] = {12, 0, 10};
constexpr uint64_t kGranule = 0x10000;
constexpr uint64_t kArenaMax = 16ull << 30;
constexpr uint64_t kArenaAt = 0x4000000000ull;   // below the guest windows (0x60_0000_0000 and up), far above the GPU window
constexpr uint64_t kGranules = kArenaMax / kGranule;   // table capacity; g_granules of them are in use

struct Tables {
    uint64_t used[kGranules / 64];   // bitmap
    uint32_t len[kGranules];         // at a block's first granule: its length in granules
    int64_t phys[kGranules];         // at a block's first granule: its direct memory
};
constexpr uint64_t kTableGranules = (sizeof(Tables) + kGranule - 1) / kGranule;

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
uint64_t g_base = 0;   // 0: not set up, ~0: unavailable
uint64_t g_size = 0, g_granules = 0;
Tables* g_t = nullptr;
uint64_t g_hint = kTableGranules;
uint64_t g_bytes = 0, g_blocks = 0, g_peak = 0;

int64_t alloc_direct(uint64_t len) {
    int64_t phys = 0;
    for (int t : kMemoryTypes)
        if (sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), len, kGranule, t, &phys) == 0) return phys;
    return -1;
}

bool map_at(uint64_t va, uint64_t len, int64_t phys) {
    void* a = reinterpret_cast<void*>(va);
    return sceKernelMapDirectMemory(&a, len, kProtRW, kMapFixed, phys, kGranule) == 0 && reinterpret_cast<uint64_t>(a) == va;
}

void unmap_keep_reserved(uint64_t va, uint64_t len) {
    sceKernelMunmap(reinterpret_cast<void*>(va), len);
    void* a = reinterpret_cast<void*>(va);
    sceKernelReserveVirtualRange(&a, len, kMapFixed, kGranule);
}

bool ready_locked() {
    if (g_base == ~0ull) return false;
    if (g_base) return true;
    // the largest range the kernel grants at the fixed address (8 GiB was granted in the stage-1 probe)
    for (uint64_t size : {kArenaMax, kArenaMax / 2, kArenaMax / 4}) {
        void* a = reinterpret_cast<void*>(kArenaAt);
        if (sceKernelReserveVirtualRange(&a, size, kMapFixed, kGranule) != 0) continue;
        if (reinterpret_cast<uint64_t>(a) != kArenaAt) {
            sceKernelMunmap(a, size);
            continue;
        }
        const uint64_t tl = kTableGranules * kGranule;
        const int64_t phys = alloc_direct(tl);
        if (phys < 0 || !map_at(kArenaAt, tl, phys)) {
            if (phys >= 0) sceKernelReleaseDirectMemory(phys, tl);
            sceKernelMunmap(a, size);
            break;
        }
        g_t = reinterpret_cast<Tables*>(kArenaAt);
        std::memset(g_t, 0, sizeof(Tables));
        for (uint64_t g = 0; g < kTableGranules; g++) g_t->used[g / 64] |= 1ull << (g % 64);
        g_size = size;
        g_granules = size / kGranule;
        g_base = kArenaAt;
        return true;
    }
    g_base = ~0ull;
    return false;
}

bool is_used(uint64_t g) { return (g_t->used[g / 64] >> (g % 64)) & 1; }
void set_used(uint64_t g0, uint64_t n, bool on) {
    for (uint64_t g = g0; g < g0 + n; g++) {
        if (on) g_t->used[g / 64] |= 1ull << (g % 64);
        else g_t->used[g / 64] &= ~(1ull << (g % 64));
    }
}

// First fit from the hint, wrapping once. Returns the first granule or 0 (granule 0 holds the tables, never free).
uint64_t find_run(uint64_t n) {
    for (int pass = 0; pass < 2; pass++) {
        uint64_t g = pass == 0 ? g_hint : kTableGranules;
        const uint64_t stop = pass == 0 ? g_granules : g_hint;
        while (g + n <= g_granules && g < stop + n) {
            if (!(g % 64) && g_t->used[g / 64] == ~0ull) {   // a full word: skip it whole
                g += 64;
                continue;
            }
            uint64_t k = 0;
            while (k < n && !is_used(g + k)) k++;
            if (k == n) return g;
            g += k + 1;
        }
    }
    return 0;
}

}  // namespace

void* big_alloc(size_t size) {
    if (!size) return nullptr;
    const uint64_t n = (size + kGranule - 1) / kGranule;
    if (n >= kGranules) return nullptr;   // (and find_run bounds it by the granted size)
    pthread_mutex_lock(&g_mu);
    void* out = nullptr;
    if (ready_locked()) {
        const uint64_t g = find_run(n);
        if (g) {
            const uint64_t va = g_base + g * kGranule, len = n * kGranule;
            const int64_t phys = alloc_direct(len);
            if (phys >= 0 && map_at(va, len, phys)) {
                set_used(g, n, true);
                g_t->len[g] = static_cast<uint32_t>(n);
                g_t->phys[g] = phys;
                g_hint = g + n;
                g_bytes += len;
                g_blocks++;
                if (g_bytes > g_peak) g_peak = g_bytes;
                out = reinterpret_cast<void*>(va);
            } else if (phys >= 0) {
                sceKernelReleaseDirectMemory(phys, len);
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
    return out;
}

bool big_owns(const void* p) {
    const uint64_t a = reinterpret_cast<uint64_t>(p);
    return g_t && a >= kArenaAt + kTableGranules * kGranule && a < kArenaAt + g_size;
}

size_t big_size(const void* p) {
    if (!big_owns(p)) return 0;
    const uint64_t g = (reinterpret_cast<uint64_t>(p) - kArenaAt) / kGranule;
    return static_cast<size_t>(g_t->len[g]) * kGranule;
}

void big_free(void* p) {
    if (!big_owns(p)) return;
    pthread_mutex_lock(&g_mu);
    const uint64_t g = (reinterpret_cast<uint64_t>(p) - kArenaAt) / kGranule, n = g_t->len[g];
    if (n) {
        const uint64_t va = kArenaAt + g * kGranule, len = n * kGranule;
        unmap_keep_reserved(va, len);
        sceKernelReleaseDirectMemory(g_t->phys[g], len);
        set_used(g, n, false);
        g_t->len[g] = 0;
        g_t->phys[g] = -1;
        if (g < g_hint) g_hint = g;
        g_bytes -= len;
        g_blocks--;
    }
    pthread_mutex_unlock(&g_mu);
}

BigStats big_stats() {
    pthread_mutex_lock(&g_mu);
    const BigStats s{g_bytes, g_blocks, g_peak};
    pthread_mutex_unlock(&g_mu);
    return s;
}

}  // namespace pkp::plat

#if !defined(PKP_PS5_PLATFORM_LAYER)   // with the SDK fork the platform layer's heap wraps malloc (CMakeLists.txt)
// ---------------------------------------------------------------- the title's allocator (linker --wrap)
extern "C" {
void* __real_malloc(size_t);
void __real_free(void*);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
int __real_posix_memalign(void**, size_t, size_t);
void* __real_memalign(size_t, size_t);
void* __real_aligned_alloc(size_t, size_t);
size_t __real_malloc_usable_size(void*);
}

namespace {
constexpr size_t kBig = 1u << 20;   // from here on: direct memory (a 64 KiB granule wastes at most 6 %)

void* big_or_null(size_t n) { return pkp::plat::big_alloc(n); }
}  // namespace

extern "C" {

void* __wrap_malloc(size_t n) {
    if (n >= kBig)
        if (void* p = big_or_null(n)) return p;
    if (void* p = __real_malloc(n)) return p;
    return n ? big_or_null(n) : nullptr;   // flexible memory exhausted: a granule rather than a failure
}

void __wrap_free(void* p) {
    if (!p) return;
    if (pkp::plat::big_owns(p)) pkp::plat::big_free(p);
    else __real_free(p);
}

void* __wrap_calloc(size_t a, size_t b) {
    size_t n = 0;
    if (__builtin_mul_overflow(a, b, &n)) return nullptr;
    if (n >= kBig)
        if (void* p = big_or_null(n)) return std::memset(p, 0, n);   // recycled direct memory is not zero
    if (void* p = __real_calloc(a, b)) return p;
    if (void* p = n ? big_or_null(n) : nullptr) return std::memset(p, 0, n);
    return nullptr;
}

size_t __wrap_malloc_usable_size(void* p) {
    if (!p) return 0;
    return pkp::plat::big_owns(p) ? pkp::plat::big_size(p) : __real_malloc_usable_size(p);
}

void* __wrap_realloc(void* p, size_t n) {
    if (!p) return __wrap_malloc(n);
    if (!n) {
        __wrap_free(p);
        return nullptr;
    }
    const bool big = pkp::plat::big_owns(p);
    if (big && n <= pkp::plat::big_size(p) && n >= kBig / 2) return p;   // still fits, not worth moving down
    if (!big && n < kBig)
        if (void* q = __real_realloc(p, n)) return q;
    const size_t old = big ? pkp::plat::big_size(p) : __real_malloc_usable_size(p);
    void* q = __wrap_malloc(n);
    if (!q) return nullptr;
    std::memcpy(q, p, old < n ? old : n);
    __wrap_free(p);
    return q;
}

int __wrap_posix_memalign(void** out, size_t align, size_t n) {
    if (n >= kBig && align <= 0x10000)
        if (void* p = big_or_null(n)) {
            *out = p;
            return 0;
        }
    return __real_posix_memalign(out, align, n);
}

void* __wrap_memalign(size_t align, size_t n) {
    if (n >= kBig && align <= 0x10000)
        if (void* p = big_or_null(n)) return p;
    return __real_memalign(align, n);
}

void* __wrap_aligned_alloc(size_t align, size_t n) {
    if (n >= kBig && align <= 0x10000)
        if (void* p = big_or_null(n)) return p;
    return __real_aligned_alloc(align, n);
}

}  // extern "C"

#endif  // !PKP_PS5_PLATFORM_LAYER
