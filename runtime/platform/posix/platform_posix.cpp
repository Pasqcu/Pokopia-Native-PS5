// SPDX-License-Identifier: GPL-3.0-or-later
// Platform layer for POSIX hosts (macOS, Linux, FreeBSD): development builds and the host CPU self-test.
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <os/os_sync_wait_on_address.h>
#include <sys/sysctl.h>
#elif defined(__FreeBSD__)
#include <pthread_np.h>
#include <sys/umtx.h>
#else
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

#include "../common/paged_vm.h"
#include "pkp/platform.h"

namespace pkp::plat {

namespace {
int prot_of(Prot p) { return p == kNone ? PROT_NONE : p == kRead ? PROT_READ : PROT_READ | PROT_WRITE; }
#if defined(__APPLE__)
// mach absolute time units per nanosecond (the timeout clock of os_sync_wait_on_address_with_timeout)
uint64_t g_tb_num = 1, g_tb_den = 1;
struct TbInit {
    TbInit() { mach_timebase_info_data_t tb; mach_timebase_info(&tb); g_tb_num = tb.numer; g_tb_den = tb.denom; }
} g_tb_init;
#endif
}

// ---- native host memory: mmap / mprotect, shm_open for sections (pages exist on first touch)
namespace {

bool native_vm_reserve(uint64_t addr, uint64_t size, char* how, size_t how_len) {
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_FIXED_NOREPLACE)
    flags |= MAP_FIXED_NOREPLACE;
#elif defined(MAP_EXCL)
    flags |= MAP_FIXED | MAP_EXCL;   // FreeBSD: fail instead of replacing an existing mapping
#endif
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) { std::snprintf(how, how_len, "mmap reserve: %s", std::strerror(errno)); return false; }
    if (reinterpret_cast<uint64_t>(p) != addr) {   // a hint the kernel did not honour (macOS has no NOREPLACE flag)
        munmap(p, size);
        std::snprintf(how, how_len, "mmap reserve: address %#llx not available", (unsigned long long)addr);
        return false;
    }
    std::snprintf(how, how_len, "mmap");
    return true;
}

bool native_vm_commit(uint64_t addr, uint64_t size, Prot prot, char* how, size_t how_len) {
    // anonymous pages are zero on first touch; mprotect makes the range accessible
    if (mprotect(reinterpret_cast<void*>(addr), size, prot_of(prot)) != 0) {
        std::snprintf(how, how_len, "mprotect commit: %s", std::strerror(errno));
        return false;
    }
    std::snprintf(how, how_len, "mmap");
    return true;
}

bool native_vm_protect(uint64_t addr, uint64_t size, Prot prot) {
    return mprotect(reinterpret_cast<void*>(addr), size, prot_of(prot)) == 0;
}

void native_vm_release(uint64_t addr, uint64_t size) { munmap(reinterpret_cast<void*>(addr), size); }

void native_vm_decommit(uint64_t addr, uint64_t size) {
    // a fresh inaccessible anonymous mapping over the range: the old pages are freed, the address space stays claimed
    mmap(reinterpret_cast<void*>(addr), size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
}

int native_shared_memory_create(uint64_t size) {
    char name[64];
    static std::atomic<unsigned> seq{0};
    std::snprintf(name, sizeof name, "/pkp-%d-%u", (int)getpid(), seq.fetch_add(1));
    const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
    return fd;
}

void* native_shared_memory_map(int handle, uint64_t off, uint64_t size, uint64_t at) {
    void* p = mmap(reinterpret_cast<void*>(at), size, PROT_READ | PROT_WRITE, MAP_SHARED | (at ? MAP_FIXED : 0), handle, (off_t)off);
    return p == MAP_FAILED ? nullptr : p;
}

void native_shared_memory_unmap(uint64_t addr, uint64_t size) { munmap(reinterpret_cast<void*>(addr), size); }

bool native_shared_memory_commit(int, uint64_t, uint64_t) { return true; }   // shm pages are allocated on first touch
void native_shared_memory_destroy(int handle) { close(handle); }


// ---- PS5 simulation (PKP_PS5_VM=1): the PS5's paged VM (runtime/platform/common/paged_vm.h) with 16 KiB pages over a
// shared-memory pool that stands in for direct memory, so the host runs the game on the console's memory algorithm.
constexpr uint64_t kSimPage = 0x4000, kSimPool = 24ull << 30;
int g_pool = -1;
uint64_t g_pool_next = 0;   // bump allocator (the simulation does not reuse backing)

bool sim() {
    static const bool on = std::getenv("PKP_PS5_VM") != nullptr;
    return on;
}
bool s_reserve(uint64_t va, uint64_t len, char* how, size_t how_len) { return native_vm_reserve(va, len, how, how_len); }
uint64_t s_reserve_any(uint64_t len, uint64_t align) {
    void* p = mmap(nullptr, len + align, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;
    const uint64_t b = reinterpret_cast<uint64_t>(p), a = (b + align - 1) & ~(align - 1), e = b + len + align;
    if (a > b) munmap(p, a - b);
    if (e > a + len) munmap(reinterpret_cast<void*>(a + len), e - (a + len));
    return a;
}
void s_unreserve(uint64_t va, uint64_t len) { munmap(reinterpret_cast<void*>(va), len); }
int64_t s_alloc(uint64_t len, uint64_t align) {
    const uint64_t a = (g_pool_next + align - 1) & ~(align - 1);
    if (g_pool < 0 || a + len > kSimPool) return -1;
    g_pool_next = a + len;
    return static_cast<int64_t>(a);
}
void s_release(int64_t, uint64_t) {}
bool s_map(uint64_t va, uint64_t len, int64_t phys) {
    void* p = mmap(reinterpret_cast<void*>(va), len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_pool, (off_t)phys);
    return p != MAP_FAILED && reinterpret_cast<uint64_t>(p) == va;
}
void s_unmap_to_reserved(uint64_t va, uint64_t len) {
    mmap(reinterpret_cast<void*>(va), len, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
}
bool s_protect(uint64_t va, uint64_t len, Prot p) { return mprotect(reinterpret_cast<void*>(va), len, prot_of(p)) == 0; }
bool s_fault_is_write(void* uc) {   // x86 page-fault error code, bit 1
#if defined(__APPLE__) && defined(__x86_64__)
    return uc && (static_cast<ucontext_t*>(uc)->uc_mcontext->__es.__err & 2) != 0;
#elif defined(__linux__) && defined(__x86_64__)
    return uc && (static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(__FreeBSD__) && defined(__x86_64__)
    return uc && (static_cast<ucontext_t*>(uc)->uc_mcontext.mc_err & 2) != 0;
#else
    (void)uc;
    return true;
#endif
}
void sim_ready() {
    static const bool once = [] {
        char name[64];
        std::snprintf(name, sizeof name, "/pkp-pool-%d", (int)getpid());
        g_pool = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (g_pool >= 0) {
            shm_unlink(name);
            if (ftruncate(g_pool, (off_t)kSimPool) != 0) { close(g_pool); g_pool = -1; }
        }
        pv::Backend b{};
        b.page = kSimPage;
        b.reserve = s_reserve;
        b.reserve_any = s_reserve_any;
        b.unreserve = s_unreserve;
        b.alloc = s_alloc;
        b.release = s_release;
        b.map = s_map;
        b.unmap_to_reserved = s_unmap_to_reserved;
        b.protect = s_protect;
        b.fault_is_write = s_fault_is_write;
        pv::init(b);
        std::fprintf(stderr, "PKP_PS5_VM: PS5 memory simulation (16 KiB pages, lazily backed sections, pool %s)\n",
                     g_pool >= 0 ? "ready" : "FAILED");
        return true;
    }();
    (void)once;
}
}  // namespace

uint64_t vm_granule() { return sim() ? kSimPage : (uint64_t)sysconf(_SC_PAGESIZE); }
bool vm_reserve(uint64_t addr, uint64_t size, char* how, size_t how_len) {
    if (sim()) { sim_ready(); return pv::vm_reserve(addr, size, how, how_len); }
    return native_vm_reserve(addr, size, how, how_len);
}
bool vm_commit(uint64_t addr, uint64_t size, Prot prot, char* how, size_t how_len) {
    if (sim()) { sim_ready(); return pv::vm_commit(addr, size, prot, how, how_len); }
    return native_vm_commit(addr, size, prot, how, how_len);
}
bool vm_protect(uint64_t addr, uint64_t size, Prot prot) {
    if (sim()) { sim_ready(); return pv::vm_protect(addr, size, prot); }
    return native_vm_protect(addr, size, prot);
}
void vm_release(uint64_t addr, uint64_t size) {
    if (sim()) { sim_ready(); pv::vm_release(addr, size); return; }
    native_vm_release(addr, size);
}
void vm_decommit(uint64_t addr, uint64_t size) {
    if (sim()) { sim_ready(); pv::vm_decommit(addr, size); return; }
    native_vm_decommit(addr, size);
}
int shared_memory_create(uint64_t size) {
    if (sim()) { sim_ready(); return pv::shared_memory_create(size); }
    return native_shared_memory_create(size);
}
void* shared_memory_map(int handle, uint64_t off, uint64_t size, uint64_t at) {
    if (sim()) { sim_ready(); return pv::shared_memory_map(handle, off, size, at); }
    return native_shared_memory_map(handle, off, size, at);
}
void shared_memory_unmap(uint64_t addr, uint64_t size) {
    if (sim()) { sim_ready(); pv::shared_memory_unmap(addr, size); return; }
    native_shared_memory_unmap(addr, size);
}
bool shared_memory_commit(int handle, uint64_t off, uint64_t size) {
    if (sim()) { sim_ready(); return pv::shared_memory_commit(handle, off, size); }
    return native_shared_memory_commit(handle, off, size);
}
void shared_memory_destroy(int handle) {
    if (sim()) { sim_ready(); pv::shared_memory_destroy(handle); return; }
    native_shared_memory_destroy(handle);
}
void prefault(const void* p, size_t n) {
    if (sim()) pv::prefault(p, n);
}
bool vm_handle_fault(void* siginfo, void* ucontext) { return sim() && pv::handle_fault(siginfo, ucontext); }
VmStats vm_stats() { return sim() ? pv::stats() : VmStats{0, 0, 0, 0, false}; }

// hosts: malloc has no small budget; callers fall back to it
void* big_alloc(size_t) { return nullptr; }
void big_free(void*) {}
bool big_owns(const void*) { return false; }
size_t big_size(const void*) { return 0; }
BigStats big_stats() { return BigStats{0, 0, 0}; }

MemInfo mem_info() { return MemInfo{0, 0, vm_granule()}; }

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void sleep_us(uint32_t us) { usleep(us); }
void yield_cpu() { sched_yield(); }

bool futex_wait(uint32_t* addr, uint32_t expected, int64_t timeout_ns) {
#if defined(__APPLE__)
    int r = timeout_ns < 0
                ? os_sync_wait_on_address(addr, expected, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE)
                : os_sync_wait_on_address_with_timeout(addr, expected, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                       OS_CLOCK_MACH_ABSOLUTE_TIME, (uint64_t)timeout_ns * g_tb_den / g_tb_num);
    return !(r < 0 && errno == ETIMEDOUT);
#elif defined(__FreeBSD__)
    struct _umtx_time ut {};
    ut._clockid = CLOCK_MONOTONIC;
    ut._timeout.tv_sec = timeout_ns / 1000000000;
    ut._timeout.tv_nsec = timeout_ns % 1000000000;
    int r = _umtx_op(addr, UMTX_OP_WAIT_UINT_PRIVATE, expected, timeout_ns < 0 ? nullptr : (void*)sizeof ut,
                     timeout_ns < 0 ? nullptr : &ut);
    return !(r != 0 && errno == ETIMEDOUT);
#else
    timespec ts{(time_t)(timeout_ns / 1000000000), (long)(timeout_ns % 1000000000)};
    long r = syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, timeout_ns < 0 ? nullptr : &ts, nullptr, 0);
    return !(r != 0 && errno == ETIMEDOUT);
#endif
}

void futex_wake(uint32_t* addr, bool all) {
#if defined(__APPLE__)
    if (all) os_sync_wake_by_address_all(addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    else os_sync_wake_by_address_any(addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
#elif defined(__FreeBSD__)
    _umtx_op(addr, UMTX_OP_WAKE_PRIVATE, all ? INT32_MAX : 1, nullptr, nullptr);
#else
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, all ? INT32_MAX : 1, nullptr, nullptr, 0);
#endif
}

uint64_t thread_id() {
#if defined(__APPLE__)
    uint64_t t = 0;
    pthread_threadid_np(nullptr, &t);
    return t;
#elif defined(__FreeBSD__)
    return (uint64_t)pthread_getthreadid_np();
#else
    return (uint64_t)syscall(SYS_gettid);
#endif
}

bool read_file(const char* path, uint8_t** data, size_t* size) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    uint8_t* p = n > 0 ? static_cast<uint8_t*>(std::malloc((size_t)n)) : nullptr;
    const bool ok = p && std::fread(p, 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    if (!ok) { std::free(p); return false; }
    *data = p;
    *size = (size_t)n;
    return true;
}

bool append_file(const char* path, const char* text, size_t len) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
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
    // CCX0 = host CPUs 0-3 (and their SMT siblings), CCX1 = 4-7: guest cores 0-2 share CCX0's L3, 3-5 go to CCX1
    static const int map[6] = {0, 1, 2, 4, 5, 6};
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
    (void)guest_core;   // macOS has no hard affinity; Linux/FreeBSD would pin to host_cpu_for_guest_core()
    return ok;
}

void cpu_description(char* out, size_t len) {
    char brand[128] = "x86-64";
#if defined(__APPLE__)
    size_t n = sizeof brand;
    sysctlbyname("machdep.cpu.brand_string", brand, &n, nullptr, 0);
#endif
    std::snprintf(out, len, "%s, %ld threads", brand, sysconf(_SC_NPROCESSORS_ONLN));
}

}  // namespace pkp::plat
