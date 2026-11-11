// SPDX-License-Identifier: GPL-3.0-or-later
// Replacements for the Windows-only parts of nxrt's diagnostics and kernel helpers on POSIX hosts and the PS5:
//   crash.cpp          -> a POSIX signal handler that logs the guest context of the faulting thread, then exits
//   sampler.cpp        -> off (needs SuspendThread / GetThreadContext)
//   watchdog.cpp       -> heartbeat counters only (no freeze dump)
//   stackwalk.cpp      -> no host stack walk
//   symbols.cpp        -> no linker-map symbols
//   starvation_guard   -> off (a Windows scheduler work-around)
//   write_watch.cpp    -> inert: no write tracking; pages are always reported "not valid", so a GPU back end re-uploads
// The upstream files stay untouched; runtime/nxrt_port/CMakeLists.txt builds these instead.
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/ucontext.h>
#include <pthread.h>
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <typeinfo>
#include <exception>
#include <cstdarg>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/diag.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/symbols.h"
#include "nxrt/write_watch.h"
#include "pkp/platform.h"
#include "psapi.h"
#include "windows.h"

struct _CONTEXT;

namespace nxrt {

// ---------------------------------------------------------------- crash
namespace {
std::atomic<uint32_t> g_reports{0};
std::string g_crash_path;
// The report is built in a static buffer (no allocation in the handler), written to stderr, then logged once.
char g_report[8192];
size_t g_rlen = 0;
void rprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void rprintf(const char* fmt, ...) {
    if (g_rlen >= sizeof g_report - 1) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(g_report + g_rlen, sizeof g_report - g_rlen, fmt, ap);
    va_end(ap);
    if (n > 0) g_rlen = std::min(sizeof g_report - 1, g_rlen + (size_t)n);
}

uint64_t pc0_of(void* uc) {
#if defined(__APPLE__) && defined(__x86_64__)
    return uc ? static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__rip : 0;
#elif defined(__x86_64__) && defined(__FreeBSD__)
    return uc ? (uint64_t)static_cast<ucontext_t*>(uc)->uc_mcontext.mc_rip : 0;
#else
    (void)uc;
    return 0;
#endif
}

#if defined(__PROSPERO__)
// libkernel's module lookup for unwinders (the layout OpenOrbis' libunwind uses on the PS4).
struct ModuleInfoForUnwind {
    uint64_t st_size;
    char name[256];
    uint64_t eh_frame_hdr_addr, eh_frame_addr, eh_frame_size, seg0_addr, seg0_size;
};
extern "C" int sceKernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info);
#endif

void describe_system_pc(uint64_t pc) {   // which module the faulting instruction is in (PS5 system libraries have no dladdr)
#if defined(__PROSPERO__)
    if (!pc) return;
    ModuleInfoForUnwind mi{};
    mi.st_size = sizeof mi;
    const int r = sceKernelGetModuleInfoForUnwind(pc, 1, &mi);
    mi.name[sizeof mi.name - 1] = 0;
    if (r == 0) NXLOG_E("crash", "fault pc %llx is in module '%s' at +%llx (segment 0 at %llx, %llx bytes)", (unsigned long long)pc, mi.name,
                        (unsigned long long)(pc - mi.seg0_addr), (unsigned long long)mi.seg0_addr, (unsigned long long)mi.seg0_size);
    else NXLOG_E("crash", "fault pc %llx: module lookup failed (%#x)", (unsigned long long)pc, (unsigned)r);
    log_flush();
#else
    (void)pc;
#endif
}

// The calling thread's stack, [lo, hi). The handler reads host memory only inside it: lifted code uses rbp as a general
// register, so a frame pointer can point anywhere, and on the PS5 a read of the eboot's execute-only code is no signal this
// handler sees (the kernel ends the process with SYSTEM_XO_VIOLATION before the report is written).
bool thread_stack(uint64_t& lo, uint64_t& hi) {
#if defined(__APPLE__)
    const uint64_t top = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(pthread_self());
    const uint64_t size = pthread_get_stacksize_np(pthread_self());
    if (!top || !size || size > top) return false;
    lo = top - size;
    hi = top;
    return true;
#elif defined(__FreeBSD__)
    pthread_attr_t at;
    if (pthread_attr_init(&at) != 0) return false;
    void* sa = nullptr;
    size_t ss = 0;
    const bool ok = pthread_attr_get_np(pthread_self(), &at) == 0 && pthread_attr_getstack(&at, &sa, &ss) == 0 && sa && ss;
    pthread_attr_destroy(&at);
    if (!ok) return false;
    lo = (uint64_t)(uintptr_t)sa;
    hi = lo + ss;
    return true;
#else
    (void)lo;
    (void)hi;
    return false;
#endif
}

void on_fatal(int sig, siginfo_t* si, void* uc) {
    if ((sig == SIGSEGV || sig == SIGBUS) && pkp::plat::vm_handle_fault(si, uc)) return;   // lazily backed page: retry
    g_reports.fetch_add(1);
    static std::atomic<int> s_depth{0};
    if (s_depth.fetch_add(1) > 0) _exit(128 + sig);   // a fault inside this handler: stop here
    ::Ctx* c = diag_thread_ctx();
    g_rlen = 0;
    rprintf("*** HOST SIGNAL %d at address %p", sig, si ? si->si_addr : nullptr);
    if (c) rprintf(" in guest f_%" PRIx64 " (lr=0x%" PRIx64 " sp=0x%" PRIx64 " x0=0x%" PRIx64 ")", c->fn, c->x[30], c->x[31], c->x[0]);
    rprintf("\n  host image: nxrt::crash_install at %p\n", (void*)&crash_install);
    if (c) {
        for (int r = 0; r < 31; r += 4) {
            rprintf(" ");
            for (int k = r; k < r + 4 && k < 31; ++k) rprintf(" x%-2d %016" PRIx64, k, c->x[k]);
            rprintf("\n");
        }
    }
    // Host frames from the signal context: RIP, then the frame-pointer chain (host code keeps frame pointers; lifted
    // code omits them, so the walk ends at the first lifted frame, which the guest context above names). Each record is
    // read only inside this thread's stack and above the previous one (thread_stack).
    uint64_t stk_lo = 0, stk_hi = 0;
    const bool have_stack = thread_stack(stk_lo, stk_hi);
    uint64_t pc = 0, fp = 0;
#if defined(__APPLE__) && defined(__x86_64__)
    if (uc) {
        const auto* m = static_cast<ucontext_t*>(uc)->uc_mcontext;
        pc = m->__ss.__rip;
        fp = m->__ss.__rbp;
    }
#elif defined(__x86_64__) && defined(__FreeBSD__)
    if (uc) {
        pc = (uint64_t)static_cast<ucontext_t*>(uc)->uc_mcontext.mc_rip;
        fp = (uint64_t)static_cast<ucontext_t*>(uc)->uc_mcontext.mc_rbp;
    }
#endif
    for (int i = 0; i < 24 && pc; i++) {
        Dl_info di{};
        if (dladdr(reinterpret_cast<void*>(pc), &di) && di.dli_sname)
            rprintf("  #%-2d %016" PRIx64 " %s+%" PRIu64 "\n", i, pc, di.dli_sname, pc - (uint64_t)(uintptr_t)di.dli_saddr);
        else
            rprintf("  #%-2d %016" PRIx64 "\n", i, pc);
        if (!have_stack || !fp || (fp & 7) || fp < stk_lo || fp + 16 > stk_hi) break;
        pc = reinterpret_cast<const uint64_t*>(fp)[1];
        const uint64_t next = reinterpret_cast<const uint64_t*>(fp)[0];
        fp = next > fp ? next : 0;
    }
    // Guest frames: the lifted code keeps AArch64 frame records ([x29] = caller's x29, [x29 + 8] = return address) in guest
    // memory, so the guest call chain is walked from the context (a bad record faults into the guard above and ends the walk).
    if (c) {
        rprintf("  guest lr  %s\n", module_describe(c->x[30]).c_str());
        uint64_t gfp = c->x[29];
        for (int i = 0; i < 32 && gfp && !(gfp & 7) && gfp >= 0x6000000000ull && gfp < 0x8000000000ull; i++) {
            const uint64_t next = reinterpret_cast<const uint64_t*>(gfp)[0], ret = reinterpret_cast<const uint64_t*>(gfp)[1];
            if (!ret) break;
            rprintf("  guest #%-2d %s\n", i, module_describe(ret).c_str());
            if (next <= gfp) break;   // the stack grows down: each caller's record is above
            gfp = next;
        }
    }
    (void)!write(2, "\n", 1);
    (void)!write(2, g_report, g_rlen);
    // The log last, and only once: the fault may have come from inside the logger (its lock held), so a second signal
    // while logging ends the process instead of looping.
    static std::atomic<int> s_in{0};
    if (s_in.fetch_add(1) == 0) {
        if (g_rlen && g_report[g_rlen - 1] == '\n') g_report[--g_rlen] = 0;
        NXLOG_E("crash", "%s", g_report);
        log_flush();
        // Details that read memory or ask the system, each its own line once the report is safe in the log: a fault in
        // here ends the process through the guard above without losing it.
        uint64_t sp = 0, r[5] = {};
#if defined(__APPLE__) && defined(__x86_64__)
        if (uc) {
            const auto* m = static_cast<ucontext_t*>(uc)->uc_mcontext;
            sp = m->__ss.__rsp;
            r[0] = m->__ss.__rdi, r[1] = m->__ss.__rsi, r[2] = m->__ss.__rdx, r[3] = m->__ss.__rcx, r[4] = m->__ss.__rax;
        }
#elif defined(__x86_64__) && defined(__FreeBSD__)
        if (uc) {
            const auto& m = static_cast<ucontext_t*>(uc)->uc_mcontext;
            sp = (uint64_t)m.mc_rsp;
            r[0] = (uint64_t)m.mc_rdi, r[1] = (uint64_t)m.mc_rsi, r[2] = (uint64_t)m.mc_rdx, r[3] = (uint64_t)m.mc_rcx, r[4] = (uint64_t)m.mc_rax;
        }
#endif
        NXLOG_E("crash", "registers: rip %llx rsp %llx rdi %llx rsi %llx rdx %llx rcx %llx rax %llx", (unsigned long long)pc0_of(uc),
                (unsigned long long)sp, (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2],
                (unsigned long long)r[3], (unsigned long long)r[4]);
        log_flush();
        if (sp) {   // return addresses into this executable (system libraries keep no frame pointers): offsets from crash_install
            uint64_t hi = sp + 0x2000;
            if (have_stack && stk_hi > sp && hi > stk_hi) hi = stk_hi;
            const uint64_t anchor = (uint64_t)(uintptr_t)&crash_install;
            char line[1600];
            int n = std::snprintf(line, sizeof line, "stack scan (rsp..%llx), code addresses as crash_install+offset:", (unsigned long long)hi);
            int found = 0;
            for (uint64_t a = sp & ~7ull; a + 8 <= hi && found < 40 && n < (int)sizeof line - 40; a += 8) {
                const uint64_t v = *reinterpret_cast<const uint64_t*>(a);
                if (v >= anchor - 0x200000 && v < anchor + 0x14000000)
                    n += std::snprintf(line + n, sizeof line - (size_t)n, " %+lld", (long long)(v - anchor)), found++;
            }
            NXLOG_E("crash", "%s", line);
            log_flush();
        }
        describe_system_pc(pc0_of(uc));
        const std::vector<std::string> files = diag_recent_files();   // fs.cpp notes every open (registry.cpp keeps the last 32)
        NXLOG_E("crash", "last files opened (oldest first): %zu", files.size());
        for (const std::string& f : files) NXLOG_E("crash", "  %s", f.c_str());
        log_flush();
    }
    _exit(128 + sig);
}

// std::terminate (an uncaught C++ exception, e.g. std::bad_alloc): the type and what() into the log before the abort.
[[noreturn]] void on_terminate() {
    const char* what = "(no active exception)";
    std::string w;
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            w = std::string(typeid(x).name()) + ": " + x.what();
            what = w.c_str();
        } catch (...) {
            what = "(an exception that is not a std::exception)";
        }
    }
    NXLOG_E("crash", "std::terminate: uncaught exception %s", what);
    log_flush();
    std::abort();
}
}  // namespace

void crash_install(const CrashOptions& opt) {
    (void)opt;
    struct sigaction sa {};
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;   // a fault inside the handler re-enters it and ends the process (guard)
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, nullptr);
    std::set_terminate(on_terminate);
}
void crash_uninstall() {
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) signal(s, SIG_DFL);
}
std::string crash_build_report(void*) { return std::string(); }
std::string crash_report_path() { return g_crash_path; }
uint32_t crash_exceptions_reported() { return g_reports.load(); }

// ---------------------------------------------------------------- sampler
void prof_sampler_start() {}
void prof_sampler_stop() {}
void prof_sampler_label(const char*) {}

// ---------------------------------------------------------------- watchdog
namespace {
std::atomic<uint64_t> g_frames{0}, g_progress{0};
std::atomic<void (*)()> g_frame_listener{nullptr};
}  // namespace
void watchdog_start(const WatchdogOptions&) {}
void watchdog_stop() {}
bool watchdog_running() { return false; }
void diag_progress() { g_progress.fetch_add(1, std::memory_order_relaxed); }
void diag_frame_tick_nolisten() { g_frames.fetch_add(1, std::memory_order_relaxed); }
void diag_frame_tick() {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (void (*fn)() = g_frame_listener.load()) fn();
}
void diag_set_frame_listener(void (*fn)()) { g_frame_listener.store(fn); }
uint64_t diag_frame_count() { return g_frames.load(std::memory_order_relaxed); }
uint32_t watchdog_fire_count() { return 0; }
void diag_add_report(std::string (*)()) {}
std::string diag_thread_dump() { return std::string(); }

namespace detail {
size_t capture_stack(const CONTEXT*, uint64_t*, size_t) { return 0; }
}  // namespace detail

// ---------------------------------------------------------------- symbols
size_t symbols_load_map(const char*) { return 0; }
std::string symbols_map_path() { return std::string(); }
size_t symbols_load_map_text(const std::string&) { return 0; }

namespace kernel {
void starvation_guard_start() {}
void starvation_guard_stop() {}
}  // namespace kernel

// ---------------------------------------------------------------- write watch (inert)
namespace mem::ww {
struct Region {
    uint64_t base, npages;
};
Region* region_create(uint64_t addr, uint64_t size) { return new Region{addr, (size + 0xFFF) / 0x1000}; }
void region_destroy(Region* r) { delete r; }
uint64_t region_base(const Region* r) { return r ? r->base : 0; }
uint64_t region_pages(const Region* r) { return r ? r->npages : 0; }
bool arm(Region*, uint64_t, uint64_t) { return true; }
bool all_valid(const Region*, uint64_t, uint64_t) { return false; }
bool page_valid(const Region*, uint64_t) { return false; }
bool page_banned(const Region*, uint64_t) { return false; }
void invalidate(Region*, uint64_t, uint64_t) {}
void take_written(Region*, std::vector<uint64_t>&) {}
void before_os_write(const void*, uint64_t) {}
OsWriteScope::OsWriteScope(const void*, uint64_t) {}
OsWriteScope::~OsWriteScope() = default;
void before_os_read(const void*, uint64_t) {}
OsReadScope::OsReadScope(const void*, uint64_t) {}
OsReadScope::~OsReadScope() = default;
void forget(uint64_t, uint64_t, bool) {}
void ban_section(uint64_t, uint64_t) {}
void set_guard_handler(GuardHandler) {}
bool guard(Region*, uint64_t, uint64_t) { return false; }
void unguard(Region*, uint64_t, uint64_t) {}
bool all_armed(const Region*, uint64_t, uint64_t) { return false; }
uint64_t guard_faults() { return 0; }
uint64_t faults() { return 0; }
uint64_t fault_waits() { return 0; }
}  // namespace mem::ww

}  // namespace nxrt

// psapi.h
BOOL GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS pmc, DWORD cb) {
    std::memset(pmc, 0, cb);
    pmc->cb = cb;
    return TRUE;
}
