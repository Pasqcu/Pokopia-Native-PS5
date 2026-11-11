// SPDX-License-Identifier: GPL-3.0-or-later
// The paged VM of paged_vm.h: page tables of the reservations and of the shared-memory sections, the rounding rules,
// lazy backing of sections from the fault handler. One spin lock guards every table; the fault handler takes it too,
// so nothing done under it touches memory that could still be lazily backed.
#include "paged_vm.h"

#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pkp::plat::pv {
namespace {

Backend g_b{};
std::atomic<bool> g_active{false};
uint64_t g_page = 0x4000;
constexpr uint64_t kChunk = 0x10000;   // section backing unit (a multiple of the host page)

std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
struct Guard {
    Guard() {
        while (g_lock.test_and_set(std::memory_order_acquire)) sched_yield();
    }
    ~Guard() { g_lock.clear(std::memory_order_release); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// A line on stderr, also from under the lock (no allocation).
__attribute__((format(printf, 1, 2))) void say(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) (void)!write(2, line, n < static_cast<int>(sizeof line) ? static_cast<size_t>(n) : sizeof line - 1);
}

uint64_t down(uint64_t v) { return v & ~(g_page - 1); }
uint64_t up(uint64_t v) { return (v + g_page - 1) & ~(g_page - 1); }

// Rule 2: a grant rounds outwards, a restriction inwards. False when nothing is left to change.
bool round_range(uint64_t addr, uint64_t size, Prot p, uint64_t* lo, uint64_t* hi) {
    if (p == kReadWrite) {
        *lo = down(addr);
        *hi = up(addr + size);
    } else {
        *lo = up(addr);
        *hi = down(addr + size);
    }
    return *lo < *hi;
}

// ---------------------------------------------------------------- reservations
struct Region {
    uint64_t base = 0, size = 0;
    int64_t* phys = nullptr;   // per page, -1 = not backed (tables made with the first commit or protect)
    uint8_t* prot = nullptr;   // per page
};
constexpr int kMaxRegions = 128;
Region g_regions[kMaxRegions];
int g_nregions = 0;

// ---------------------------------------------------------------- sections
struct View {
    uint64_t addr = 0, off = 0, size = 0;
    uint8_t* prot = nullptr;   // per page of the view
};
// nxrt's NVN back end imports a pool into the GPU through private views of the heap, one per 80 MiB chunk: the game's
// 3500 MiB pool alone takes 55. With room for 16, the next chunk was imported through the heap's own view instead.
constexpr int kMaxViews = 256, kMaxSections = 8;
struct Section {
    bool used = false;
    uint64_t size = 0, nchunks = 0;
    int64_t* phys = nullptr;        // per chunk, -1 = not backed
    uint8_t* committed = nullptr;   // per chunk
    View views[kMaxViews];
    int nviews = 0;
};
Section g_sections[kMaxSections];

uint64_t g_region_backed = 0, g_section_backed = 0, g_section_committed = 0, g_faults = 0;

Region* region_at(uint64_t a) {
    for (int i = 0; i < g_nregions; i++)
        if (a - g_regions[i].base < g_regions[i].size) return &g_regions[i];
    return nullptr;
}

bool ensure_tables(Region& r) {
    if (r.phys) return true;
    const uint64_t n = r.size / g_page;
    r.phys = static_cast<int64_t*>(std::malloc(n * sizeof(int64_t)));
    r.prot = static_cast<uint8_t*>(std::malloc(n));
    if (!r.phys || !r.prot) {
        std::free(r.phys);
        std::free(r.prot);
        r.phys = nullptr;
        r.prot = nullptr;
        return false;
    }
    for (uint64_t i = 0; i < n; i++) r.phys[i] = -1;
    std::memset(r.prot, kNone, n);
    return true;
}

uint64_t page_of(const Region& r, uint64_t a) { return (a - r.base) / g_page; }

// Protection of the pages [lo, hi) of a region (page aligned, inside it): the table always, the backend for backed runs.
void region_apply(Region& r, uint64_t lo, uint64_t hi, Prot p) {
    uint64_t run = 0;
    bool in_run = false;
    for (uint64_t a = lo; a < hi; a += g_page) {
        const uint64_t i = page_of(r, a);
        r.prot[i] = static_cast<uint8_t>(p);
        const bool backed = r.phys[i] >= 0;
        if (backed && !in_run) {
            run = a;
            in_run = true;
        } else if (!backed && in_run) {
            g_b.protect(run, a - run, p);
            in_run = false;
        }
    }
    if (in_run) g_b.protect(run, hi - run, p);
}

// Backs the pages [a, b) of a region that have no backing: one allocation for the run, page by page when the pool is fragmented.
bool back_run(Region& r, uint64_t a, uint64_t b, char* how, size_t how_len) {
    const uint64_t len = b - a;
    const int64_t phys = g_b.alloc(len, g_page);
    if (phys >= 0) {
        if (!g_b.map(a, len, phys)) {
            g_b.release(phys, len);
            std::snprintf(how, how_len, "mapping %llu KiB at %llx failed", (unsigned long long)(len >> 10), (unsigned long long)a);
            return false;
        }
        std::memset(reinterpret_cast<void*>(a), 0, len);   // recycled backing is not guaranteed to be zero
        for (uint64_t x = a; x < b; x += g_page) {
            r.phys[page_of(r, x)] = phys + static_cast<int64_t>(x - a);
            r.prot[page_of(r, x)] = kReadWrite;
        }
        g_region_backed += len;
        return true;
    }
    for (uint64_t x = a; x < b; x += g_page) {
        const int64_t p = g_b.alloc(g_page, g_page);
        if (p < 0) {
            std::snprintf(how, how_len, "out of backing memory (%llu KiB asked)", (unsigned long long)(len >> 10));
            return false;
        }
        if (!g_b.map(x, g_page, p)) {
            g_b.release(p, g_page);
            std::snprintf(how, how_len, "mapping a page at %llx failed", (unsigned long long)x);
            return false;
        }
        std::memset(reinterpret_cast<void*>(x), 0, g_page);
        r.phys[page_of(r, x)] = p;
        r.prot[page_of(r, x)] = kReadWrite;
        g_region_backed += g_page;
    }
    return true;
}

Section* view_at(uint64_t a, View** out) {
    for (Section& s : g_sections) {
        if (!s.used) continue;
        for (int v = 0; v < s.nviews; v++)
            if (a - s.views[v].addr < s.views[v].size) {
                *out = &s.views[v];
                return &s;
            }
    }
    return nullptr;
}

// Protection of the pages [lo, hi) of a view: the table always, the backend where the chunk is backed.
void view_apply(Section& s, View& v, uint64_t lo, uint64_t hi, Prot p) {
    if (lo < v.addr) lo = v.addr;
    if (hi > v.addr + v.size) hi = v.addr + v.size;
    uint64_t run = 0;
    bool in_run = false;
    for (uint64_t a = lo; a < hi; a += g_page) {
        v.prot[(a - v.addr) / g_page] = static_cast<uint8_t>(p);
        const bool backed = s.phys[(v.off + (a - v.addr)) / kChunk] >= 0;
        if (backed && !in_run) {
            run = a;
            in_run = true;
        } else if (!backed && in_run) {
            g_b.protect(run, a - run, p);
            in_run = false;
        }
    }
    if (in_run) g_b.protect(run, hi - run, p);
}

// A private address range where back_chunk zeroes a new chunk before any view shows it. It keeps the last chunk zeroed
// mapped and the next one is mapped over it, so the range never goes back to the system (where another mmap could take it).
uint64_t g_window = 0;

bool zero_in_window(int64_t phys) {   // recycled backing is not guaranteed to be zero
    if (!g_window && !(g_window = g_b.reserve_any(kChunk, kChunk))) return false;
    if (!g_b.map(g_window, kChunk, phys)) {   // a host that does not map over a mapping: back to reserved space first
        static bool told = false;
        if (!told) {
            static const char m[] = "pkp: paged VM: the zeroing window is unmapped between chunks\n";
            (void)!write(2, m, sizeof m - 1);
            told = true;
        }
        g_b.unmap_to_reserved(g_window, kChunk);
        if (!g_b.map(g_window, kChunk, phys)) return false;
    }
    std::memset(reinterpret_cast<void*>(g_window), 0, kChunk);
    return true;
}

// Rule 3: backs chunk c of a section and maps it in every view, each view with its own page protections. The chunk is
// zeroed in the window first: zeroed through a view, it would wipe what another thread writes there meanwhile (that access
// does not fault, the chunk is mapped already), which left the game a null pointer in its first scene on the PS5.
bool back_chunk(Section& s, uint64_t c) {
    const int64_t phys = g_b.alloc(kChunk, kChunk);
    if (phys < 0) {
        static const char m[] = "pkp: out of backing memory for a shared-memory section (the heap)\n";
        (void)!write(2, m, sizeof m - 1);
        return false;
    }
    if (!zero_in_window(phys)) {
        g_b.release(phys, kChunk);
        static const char m[] = "pkp: zeroing a heap chunk failed\n";
        (void)!write(2, m, sizeof m - 1);
        return false;
    }
    const uint64_t o = c * kChunk;
    for (int k = 0; k < s.nviews; k++) {
        View& v = s.views[k];
        if (o < v.off || o >= v.off + v.size) continue;
        const uint64_t va = v.addr + (o - v.off);
        if (!g_b.map(va, kChunk, phys)) {
            static const char m[] = "pkp: mapping a heap chunk failed\n";
            (void)!write(2, m, sizeof m - 1);
            return false;
        }
        for (uint64_t x = 0; x < kChunk; x += g_page) {
            const uint8_t pp = v.prot[(va + x - v.addr) / g_page];
            if (pp != kReadWrite) g_b.protect(va + x, g_page, static_cast<Prot>(pp));
        }
    }
    s.phys[c] = phys;
    g_section_backed += kChunk;
    return true;
}

bool handle_fault_locked(uint64_t a, bool write) {
    View* v = nullptr;
    Section* s = view_at(a, &v);
    if (!s) return false;
    const uint8_t p = v->prot[(a - v->addr) / g_page];
    if (p == kNone || (write && p != kReadWrite)) return false;   // a real protection fault
    const uint64_t c = (v->off + (a - v->addr)) / kChunk;
    if (!s->committed[c]) return false;                           // an access to uncommitted memory
    if (s->phys[c] >= 0) return true;                             // another thread backed it meanwhile
    return back_chunk(*s, c);
}

struct sigaction g_prev_segv, g_prev_bus;
void on_signal(int sig, siginfo_t* si, void* uc) {
    if (handle_fault(si, uc)) return;
    const struct sigaction& p = sig == SIGBUS ? g_prev_bus : g_prev_segv;
    if (p.sa_flags & SA_SIGINFO) {
        if (p.sa_sigaction) {
            p.sa_sigaction(sig, si, uc);
            return;
        }
    } else if (p.sa_handler == SIG_IGN) {
        return;
    } else if (p.sa_handler != SIG_DFL && p.sa_handler != nullptr) {
        p.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);   // the access repeats and the default action ends the process
}

}  // namespace

void init(const Backend& b) {
    Guard g;
    if (g_active.load()) return;
    g_b = b;
    g_page = b.page;
    struct sigaction sa {};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);
    g_active.store(true);
}

bool active() { return g_active.load(); }

bool vm_reserve(uint64_t addr, uint64_t size, char* how, size_t how_len) {
    size = up(size);
    if (addr & (g_page - 1)) {
        std::snprintf(how, how_len, "address %llx is not host-page aligned", (unsigned long long)addr);
        return false;
    }
    Guard g;
    if (g_nregions == kMaxRegions) {
        std::snprintf(how, how_len, "region table full");
        return false;
    }
    if (!g_b.reserve(addr, size, how, how_len)) return false;
    g_regions[g_nregions++] = Region{addr, size, nullptr, nullptr};
    return true;
}

bool vm_commit(uint64_t addr, uint64_t size, Prot prot, char* how, size_t how_len) {
    if (!size) return true;
    Guard g;
    Region* r = region_at(addr);
    if (!r || addr + size > r->base + r->size) {
        std::snprintf(how, how_len, "commit outside a reservation");
        return false;
    }
    if (!ensure_tables(*r)) {
        std::snprintf(how, how_len, "out of host memory for a page table");
        return false;
    }
    const uint64_t lo = down(addr), hi = up(addr + size), end = addr + size;
    // Rule 1: pages a neighbour backed already keep their backing; the newly committed part reads zero.
    for (uint64_t a = lo; a < hi; a += g_page) {
        const uint64_t i = page_of(*r, a);
        if (r->phys[i] < 0) continue;
        const uint64_t z0 = a < addr ? addr : a, z1 = a + g_page > end ? end : a + g_page;
        const Prot was = static_cast<Prot>(r->prot[i]);
        if (was != kReadWrite) g_b.protect(a, g_page, kReadWrite);
        std::memset(reinterpret_cast<void*>(z0), 0, z1 - z0);
        if (was != kReadWrite) g_b.protect(a, g_page, was);
    }
    for (uint64_t a = lo; a < hi;) {
        if (r->phys[page_of(*r, a)] >= 0) {
            a += g_page;
            continue;
        }
        uint64_t b = a;
        while (b < hi && r->phys[page_of(*r, b)] < 0) b += g_page;
        if (!back_run(*r, a, b, how, how_len)) return false;
        a = b;
    }
    uint64_t plo = 0, phi = 0;
    if (round_range(addr, size, prot, &plo, &phi)) region_apply(*r, plo, phi, prot);
    std::snprintf(how, how_len, "paged VM (%llu KiB pages)", (unsigned long long)(g_page >> 10));
    return true;
}

bool vm_protect(uint64_t addr, uint64_t size, Prot prot) {
    if (!size) return true;
    Guard g;
    uint64_t lo = 0, hi = 0;
    const bool any = round_range(addr, size, prot, &lo, &hi);
    View* v = nullptr;
    if (Section* s = view_at(addr, &v)) {
        if (any) view_apply(*s, *v, lo, hi, prot);
        return true;
    }
    Region* r = region_at(addr);
    if (!r) return any ? g_b.protect(lo, hi - lo, prot) : true;   // memory this layer does not manage
    if (!any) return true;
    if (!ensure_tables(*r)) return false;
    if (lo < r->base) lo = r->base;
    if (hi > r->base + r->size) hi = r->base + r->size;
    region_apply(*r, lo, hi, prot);
    return true;
}

void vm_decommit(uint64_t addr, uint64_t size) {
    Guard g;
    Region* r = region_at(addr);
    if (!r || !r->phys) return;
    uint64_t lo = up(addr), hi = down(addr + size);   // rule 4: whole pages only
    if (lo < r->base) lo = r->base;
    if (hi > r->base + r->size) hi = r->base + r->size;
    for (uint64_t a = lo; a < hi;) {
        if (r->phys[page_of(*r, a)] < 0) {
            r->prot[page_of(*r, a)] = kNone;
            a += g_page;
            continue;
        }
        uint64_t b = a;
        while (b < hi && r->phys[page_of(*r, b)] >= 0) b += g_page;
        g_b.unmap_to_reserved(a, b - a);
        for (uint64_t x = a; x < b; x += g_page) {
            g_b.release(r->phys[page_of(*r, x)], g_page);
            r->phys[page_of(*r, x)] = -1;
            r->prot[page_of(*r, x)] = kNone;
            g_region_backed -= g_page;
        }
        a = b;
    }
}

void vm_release(uint64_t addr, uint64_t size) {
    Guard g;
    for (int k = 0; k < g_nregions; k++) {
        Region& r = g_regions[k];
        if (r.base != addr) continue;
        g_b.unreserve(r.base, r.size);
        if (r.phys)
            for (uint64_t i = 0; i < r.size / g_page; i++)
                if (r.phys[i] >= 0) {
                    g_b.release(r.phys[i], g_page);
                    g_region_backed -= g_page;
                }
        std::free(r.phys);
        std::free(r.prot);
        g_regions[k] = g_regions[--g_nregions];
        return;
    }
    g_b.unreserve(addr, up(size));
}

int shared_memory_create(uint64_t size) {
    Guard g;
    for (int h = 0; h < kMaxSections; h++) {
        Section& s = g_sections[h];
        if (s.used) continue;
        const uint64_t n = (size + kChunk - 1) / kChunk;
        s.phys = static_cast<int64_t*>(std::malloc(n * sizeof(int64_t)));
        s.committed = static_cast<uint8_t*>(std::calloc(n, 1));
        if (!s.phys || !s.committed) {
            std::free(s.phys);
            std::free(s.committed);
            s = Section{};
            return -1;
        }
        for (uint64_t i = 0; i < n; i++) s.phys[i] = -1;
        s.size = n * kChunk;
        s.nchunks = n;
        s.nviews = 0;
        s.used = true;
        return h;
    }
    return -1;
}

void* shared_memory_map(int handle, uint64_t off, uint64_t size, uint64_t at) {
    Guard g;
    if (handle < 0 || handle >= kMaxSections || !g_sections[handle].used) return nullptr;
    Section& s = g_sections[handle];
    size = (size + kChunk - 1) & ~(kChunk - 1);
    if (off % kChunk || !size || off + size > s.size) return nullptr;
    static int told = 0;   // under the lock: the first refusals go to stderr
    char how[160] = "";
    bool ok = false;
    if (s.nviews == kMaxViews) {
        std::snprintf(how, sizeof how, "no room for another view (%d)", kMaxViews);
    } else if (at % g_page) {
        std::snprintf(how, sizeof how, "address %llx not host-page aligned", (unsigned long long)at);
    } else if (at) {
        ok = g_b.reserve(at, size, how, sizeof how);
    } else {
        at = g_b.reserve_any(size, kChunk);
        ok = at != 0;
        if (!ok) std::snprintf(how, sizeof how, "no address space");
    }
    if (!ok) {
        if (told < 4) {
            told++;
            say("pkp: paged VM: a view of section %d (%llu MiB at offset %llx) refused: %s\n", handle,
                (unsigned long long)(size >> 20), (unsigned long long)off, how);
        }
        return nullptr;
    }
    View& v = s.views[s.nviews];
    v.prot = static_cast<uint8_t*>(std::malloc(size / g_page));
    if (!v.prot) {
        g_b.unreserve(at, size);
        return nullptr;
    }
    std::memset(v.prot, kReadWrite, size / g_page);   // a view's own access; the caller narrows it
    v.addr = at;
    v.off = off;
    v.size = size;
    s.nviews++;
    for (uint64_t c = off / kChunk; c < (off + size) / kChunk; c++)   // chunks backed through another view appear here at once
        if (s.phys[c] >= 0) g_b.map(at + (c * kChunk - off), kChunk, s.phys[c]);
    return reinterpret_cast<void*>(at);
}

void shared_memory_unmap(uint64_t addr, uint64_t size) {
    Guard g;
    for (Section& s : g_sections)
        for (int k = 0; s.used && k < s.nviews; k++)
            if (s.views[k].addr == addr) {
                g_b.unreserve(addr, s.views[k].size);
                std::free(s.views[k].prot);
                s.views[k] = s.views[--s.nviews];
                return;
            }
    g_b.unreserve(addr, up(size));
}

bool shared_memory_commit(int handle, uint64_t off, uint64_t size) {
    Guard g;
    if (handle < 0 || handle >= kMaxSections || !g_sections[handle].used) return false;
    Section& s = g_sections[handle];
    if (!size) return true;
    uint64_t c1 = (off + size + kChunk - 1) / kChunk;
    if (c1 > s.nchunks) c1 = s.nchunks;
    for (uint64_t c = off / kChunk; c < c1; c++)
        if (!s.committed[c]) {
            s.committed[c] = 1;
            g_section_committed += kChunk;
        }
    return true;   // backed on first access (rule 3)
}

void shared_memory_destroy(int handle) {
    Guard g;
    if (handle < 0 || handle >= kMaxSections || !g_sections[handle].used) return;
    Section& s = g_sections[handle];
    for (int k = 0; k < s.nviews; k++) {
        g_b.unreserve(s.views[k].addr, s.views[k].size);
        std::free(s.views[k].prot);
    }
    if (g_window) g_b.unmap_to_reserved(g_window, kChunk);   // it may still map one of these chunks
    for (uint64_t c = 0; c < s.nchunks; c++)
        if (s.phys[c] >= 0) {
            g_b.release(s.phys[c], kChunk);
            g_section_backed -= kChunk;
        }
    for (uint64_t c = 0; c < s.nchunks; c++)
        if (s.committed[c]) g_section_committed -= kChunk;
    std::free(s.phys);
    std::free(s.committed);
    s = Section{};
}

// Rule 3 for memory a system call or the GPU is about to use: backs the committed chunks of the views over [p, p+n) under
// the lock, a few at a time, without touching the memory. What the range cannot have is left as it is and told on stderr
// (the first times): a chunk not committed, a page with no access in its view. A read of each page did this before, and
// such a page ended the game on the PS5 (a pool chunk imported through the heap's own view, one page of it with no
// access); now the system call answers EFAULT there, the GPU import fails.
void prefault(const void* p, size_t n) {
    if (!g_active.load() || !n) return;
    const uint64_t a = reinterpret_cast<uint64_t>(p), e = a + n;
    struct {
        uint64_t chunks = 0, at = 0;
        const char* why = nullptr;
        View view{};
        int section = 0, views = 0;
    } bad;
    for (uint64_t x = a; x < e;) {
        Guard g;   // the fault handlers of other threads wait on this lock
        for (int k = 0; k < 16 && x < e; k++) {
            View* v = nullptr;
            Section* s = view_at(x, &v);
            if (!s) {   // not in a view: on to the next view that starts inside the range
                uint64_t next = e;
                for (const Section& t : g_sections)
                    for (int i = 0; t.used && i < t.nviews; i++)
                        if (t.views[i].addr > x && t.views[i].addr < next) next = t.views[i].addr;
                x = next;
                continue;
            }
            const uint64_t c = (v->off + (x - v->addr)) / kChunk;
            uint64_t hi = v->addr + ((c + 1) * kChunk - v->off);   // where chunk c ends in this view
            if (hi > e) hi = e;
            const char* why = nullptr;
            uint64_t at = x;
            if (!s->committed[c]) {
                why = "is not committed";
            } else if (s->phys[c] < 0 && !back_chunk(*s, c)) {
                why = "could not be backed";
            } else {
                for (uint64_t y = down(x); y < hi && !why; y += g_page)
                    if (v->prot[(y - v->addr) / g_page] == kNone) {
                        why = "has no access in its view";
                        at = y;
                    }
            }
            if (why && !bad.chunks++) {
                bad.at = at;
                bad.why = why;
                bad.view = *v;
                bad.section = static_cast<int>(s - g_sections);
                bad.views = s->nviews;
            }
            x = hi;
        }
    }
    static std::atomic<int> told{0};
    if (bad.chunks && told.fetch_add(1) < 8)
        say("pkp: prefault of [%llx, %llx): %llu chunks left alone, the first at %llx %s (view %llx of %llu MiB at offset "
            "%llx of section %d, %d views)\n",
            (unsigned long long)a, (unsigned long long)e, (unsigned long long)bad.chunks, (unsigned long long)bad.at, bad.why,
            (unsigned long long)bad.view.addr, (unsigned long long)(bad.view.size >> 20), (unsigned long long)bad.view.off,
            bad.section, bad.views);
}

bool handle_fault(void* siginfo, void* ucontext) {
    if (!g_active.load() || !siginfo) return false;
    const uint64_t a = reinterpret_cast<uint64_t>(static_cast<siginfo_t*>(siginfo)->si_addr);
    const bool w = g_b.fault_is_write ? g_b.fault_is_write(ucontext) : true;
    Guard g;
    const bool ok = handle_fault_locked(a, w);
    if (ok) g_faults++;
    return ok;
}

VmStats stats() {
    Guard g;
    return VmStats{g_region_backed, g_section_backed, g_section_committed, g_faults, g_active.load()};
}

}  // namespace pkp::plat::pv
