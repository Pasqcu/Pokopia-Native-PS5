// SPDX-License-Identifier: GPL-3.0-or-later
// Platform layer of the PS5 title (public ps5-payload-sdk: libkernel and libSceLibcInternal stubs).
//
// Memory facts this follows (measured by the R-comp and PS5 Dolphin/PPSSPP ports, see docs/ARCHITETTURA.md):
//   - anonymous mmap, PROT_NONE included, is charged to the title's small flexible-memory budget (~448 MiB);
//   - sceKernelReserveVirtualRange claims address space without a charge (4 GiB granted, 64 GiB refused);
//   - backing comes from direct memory (a ~12 GiB pool): sceKernelAllocateDirectMemory + sceKernelMapDirectMemory MAP_FIXED
//     over the reservation; recycled direct memory is not guaranteed to be zero; pages are 16 KiB;
//   - [0x2_0000_0000, 0x3_0000_0000) is the GPU-visible window of PS5_Vulkan: guest memory never goes there.
// Every primitive that fails falls back to the next one and says so in `how`, so the bring-up log tells which ones a
// given firmware grants.
#include <fcntl.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sched.h>
#include <setjmp.h>
#include <sys/umtx.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../common/paged_vm.h"
#include "pkp/platform.h"

extern "C" {
int sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type,
                                  int64_t* physical_address);
int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t physical_address, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelMunmap(void* addr, size_t len);
int sceKernelMprotect(const void* addr, size_t len, int prot);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableFlexibleMemorySize(size_t* size);
int sceKernelUsleep(unsigned int microseconds);
}

namespace pkp::plat {

namespace {

constexpr int kMapFixed = 0x10;
constexpr int kProtCpuRead = 0x01, kProtCpuWrite = 0x02;
// Guest memory is GPU memory too: nxrt's NVN back end imports the game's pools (VK_EXT_external_memory_host) and RADV reaches
// them at the CPU's address. Every mapping and protection carries the GPU bits along with the CPU's, so a later protection
// change of the guest never takes the GPU's access away.
constexpr int kProtGpuRead = 0x10, kProtGpuWrite = 0x20;
constexpr uint64_t kGranule = 0x4000;
constexpr int kMemoryTypes[] = {12, 0, 10};   // CPU write-back first; logged when another one is used

int sce_prot(Prot p) {
    return p == kNone ? 0 : p == kRead ? kProtCpuRead | kProtGpuRead : kProtCpuRead | kProtCpuWrite | kProtGpuRead | kProtGpuWrite;
}

// ---- backend of the paged VM (runtime/platform/common/paged_vm.h): direct memory over reserved address space
bool b_reserve(uint64_t va, uint64_t len, char* how, size_t how_len) {
    void* a = reinterpret_cast<void*>(va);
    const int r = sceKernelReserveVirtualRange(&a, len, kMapFixed, kGranule);
    if (r == 0 && reinterpret_cast<uint64_t>(a) == va) {
        std::snprintf(how, how_len, "sceKernelReserveVirtualRange");
        return true;
    }
    if (r == 0) sceKernelMunmap(a, len);   // granted elsewhere: not usable, guest addresses are fixed
    std::snprintf(how, how_len, "ReserveVirtualRange %#x", (unsigned)r);
    return false;
}
uint64_t b_reserve_any(uint64_t len, uint64_t align) {
    void* a = nullptr;
    return sceKernelReserveVirtualRange(&a, len, 0, align) == 0 ? reinterpret_cast<uint64_t>(a) : 0;
}
void b_unreserve(uint64_t va, uint64_t len) { sceKernelMunmap(reinterpret_cast<void*>(va), len); }
int64_t b_alloc(uint64_t len, uint64_t align) {
    int64_t phys = 0;
    for (int t : kMemoryTypes)
        if (sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), len, align, t, &phys) == 0) return phys;
    return -1;
}
void b_release(int64_t phys, uint64_t len) { sceKernelReleaseDirectMemory(phys, len); }
bool b_map(uint64_t va, uint64_t len, int64_t phys) {
    void* a = reinterpret_cast<void*>(va);
    return sceKernelMapDirectMemory(&a, len, sce_prot(kReadWrite), kMapFixed, phys, kGranule) == 0 &&
           reinterpret_cast<uint64_t>(a) == va;
}
void b_unmap_to_reserved(uint64_t va, uint64_t len) {
    // R-comp's measured recipe: unmap the backed range, re-reserve the hole at the same address (MAP_FIXED)
    sceKernelMunmap(reinterpret_cast<void*>(va), len);
    void* a = reinterpret_cast<void*>(va);
    sceKernelReserveVirtualRange(&a, len, kMapFixed, kGranule);
}
bool b_protect(uint64_t va, uint64_t len, Prot p) { return sceKernelMprotect(reinterpret_cast<void*>(va), len, sce_prot(p)) == 0; }
bool b_fault_is_write(void* uc) {   // x86 page-fault error code, bit 1: the access was a write
    return uc && (static_cast<ucontext_t*>(uc)->uc_mcontext.mc_err & 2) != 0;
}

void vm_ready() {
    static const bool once = [] {
        pv::Backend b{};
        b.page = kGranule;
        b.reserve = b_reserve;
        b.reserve_any = b_reserve_any;
        b.unreserve = b_unreserve;
        b.alloc = b_alloc;
        b.release = b_release;
        b.map = b_map;
        b.unmap_to_reserved = b_unmap_to_reserved;
        b.protect = b_protect;
        b.fault_is_write = b_fault_is_write;
        pv::init(b);
        return true;
    }();
    (void)once;
}

}  // namespace

// Guest memory: the paged VM (16 KiB pages, direct memory, the heap section backed on first access).
uint64_t vm_granule() { return kGranule; }
bool vm_reserve(uint64_t addr, uint64_t size, char* how, size_t how_len) { vm_ready(); return pv::vm_reserve(addr, size, how, how_len); }
bool vm_commit(uint64_t addr, uint64_t size, Prot prot, char* how, size_t how_len) { vm_ready(); return pv::vm_commit(addr, size, prot, how, how_len); }
bool vm_protect(uint64_t addr, uint64_t size, Prot prot) { vm_ready(); return pv::vm_protect(addr, size, prot); }
void vm_decommit(uint64_t addr, uint64_t size) { vm_ready(); pv::vm_decommit(addr, size); }
void vm_release(uint64_t addr, uint64_t size) { vm_ready(); pv::vm_release(addr, size); }
int shared_memory_create(uint64_t size) { vm_ready(); return pv::shared_memory_create(size); }
void* shared_memory_map(int handle, uint64_t off, uint64_t size, uint64_t at) { vm_ready(); return pv::shared_memory_map(handle, off, size, at); }
void shared_memory_unmap(uint64_t addr, uint64_t size) { vm_ready(); pv::shared_memory_unmap(addr, size); }
bool shared_memory_commit(int handle, uint64_t off, uint64_t size) { vm_ready(); return pv::shared_memory_commit(handle, off, size); }
void shared_memory_destroy(int handle) { vm_ready(); pv::shared_memory_destroy(handle); }
void prefault(const void* p, size_t n) { pv::prefault(p, n); }
bool vm_handle_fault(void* siginfo, void* ucontext) { return pv::handle_fault(siginfo, ucontext); }
VmStats vm_stats() { return pv::stats(); }

MemInfo mem_info() {
    size_t flex = 0;
    sceKernelAvailableFlexibleMemorySize(&flex);
    return MemInfo{(uint64_t)sceKernelGetDirectMemorySize(), (uint64_t)flex, kGranule};
}

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void sleep_us(uint32_t us) { sceKernelUsleep(us); }
void yield_cpu() { sched_yield(); }

// The PS5 kernel is FreeBSD's: libkernel exports the _umtx_op system call that FreeBSD's libthr builds on.
bool futex_wait(uint32_t* addr, uint32_t expected, int64_t timeout_ns) {
    struct _umtx_time ut {};
    ut._clockid = CLOCK_MONOTONIC;
    ut._timeout.tv_sec = timeout_ns / 1000000000;
    ut._timeout.tv_nsec = timeout_ns % 1000000000;
    const int r = _umtx_op(addr, UMTX_OP_WAIT_UINT_PRIVATE, expected, timeout_ns < 0 ? nullptr : (void*)sizeof ut,
                           timeout_ns < 0 ? nullptr : &ut);
    return !(r != 0 && errno == ETIMEDOUT);
}

void futex_wake(uint32_t* addr, bool all) { _umtx_op(addr, UMTX_OP_WAKE_PRIVATE, all ? INT32_MAX : 1, nullptr, nullptr); }

uint64_t thread_id() { return (uint64_t)pthread_getthreadid_np(); }

bool read_file(const char* path, uint8_t** data, size_t* size) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    const off_t n = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    uint8_t* p = n > 0 ? static_cast<uint8_t*>(std::malloc((size_t)n)) : nullptr;
    size_t got = 0;
    while (p && got < (size_t)n) {
        const ssize_t k = read(fd, p + got, (size_t)n - got);
        if (k <= 0) break;
        got += (size_t)k;
    }
    close(fd);
    if (!p || got != (size_t)n) { std::free(p); return false; }
    *data = p;
    *size = got;
    return true;
}

bool append_file(const char* path, const char* text, size_t len) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return false;
    const bool ok = write(fd, text, len) == (ssize_t)len;
    close(fd);
    return ok;
}

void log_console(const char* line, size_t len) { (void)!write(2, line, len); }

namespace {
sigjmp_buf g_guard;
volatile int g_guard_sig;
void* volatile g_guard_addr;
void on_fault(int sig, siginfo_t* si, void*) {
    g_guard_sig = sig;
    g_guard_addr = si ? si->si_addr : nullptr;
    siglongjmp(g_guard, 1);
}
}  // namespace

bool run_guarded(void (*fn)(void*), void* arg, char* why, size_t why_len) {
    struct sigaction sa {}, old[4];
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    const int sigs[4] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (int i = 0; i < 4; i++) sigaction(sigs[i], &sa, &old[i]);
    bool ok = true;
    if (sigsetjmp(g_guard, 1) == 0) {
        fn(arg);
    } else {
        ok = false;
        std::snprintf(why, why_len, "signal %d at %p", g_guard_sig, g_guard_addr);
    }
    for (int i = 0; i < 4; i++) sigaction(sigs[i], &old[i], nullptr);
    return ok;
}

int host_cpu_for_guest_core(int guest_core) {
    // PS5: 8 Zen 2 cores, CCX0 = cores 0-3, CCX1 = cores 4-7 (hardware threads 2n, 2n+1). Guest cores 0-2 share CCX0's
    // 4 MiB L3; 3-5 go to CCX1. One hardware thread per guest core keeps the SMT sibling free for host work.
    static const int map[6] = {0, 2, 4, 8, 10, 12};
    return guest_core >= 0 && guest_core < 6 ? map[guest_core] : -1;
}

bool thread_start(ThreadFn fn, void* arg, int guest_core, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (stack_size) pthread_attr_setstacksize(&a, stack_size);
    pthread_t t;
    const bool ok = pthread_create(&t, &a, fn, arg) == 0;
    pthread_attr_destroy(&a);
    if (ok) pthread_detach(t);
    (void)guest_core;   // affinity (scePthreadSetaffinity) comes with the kernel's thread model in the HLE stage
    return ok;
}

void cpu_description(char* out, size_t len) {
    unsigned regs[12] = {0};
    for (unsigned i = 0; i < 3; i++) {
        unsigned a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000002u + i), "c"(0u));
        regs[4 * i] = a; regs[4 * i + 1] = b; regs[4 * i + 2] = c; regs[4 * i + 3] = d;
    }
    char brand[49];
    std::memcpy(brand, regs, 48);
    brand[48] = 0;
    const char* b = brand;
    while (*b == ' ') b++;
    std::snprintf(out, len, "%s", *b ? b : "AMD Zen 2");
}

}  // namespace pkp::plat
