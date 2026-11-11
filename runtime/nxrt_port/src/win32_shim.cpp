// SPDX-License-Identifier: GPL-3.0-or-later
// Implementation of compat/windows.h for POSIX hosts and the PS5 (see the header for the contract).
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include "pkp/platform.h"
#include "windows.h"

namespace {

using namespace pkp;

thread_local DWORD t_last_error;

inline uint32_t* word(void* p) { return reinterpret_cast<uint32_t*>(p); }
inline uint32_t load(uint32_t* w) { return __atomic_load_n(w, __ATOMIC_ACQUIRE); }

// ---------------------------------------------------------------- SRW lock: one 32-bit word (bit 31 writer, low bits readers)
constexpr uint32_t kWriter = 0x80000000u;

// ---------------------------------------------------------------- handles
enum class Kind { Thread, Event, Timer, Mutex, Section, File };
struct Obj {
    Kind kind;
    std::atomic<int> refs{1};
    explicit Obj(Kind k) : kind(k) {}
    virtual ~Obj() = default;
};
struct ThreadObj : Obj {
    ThreadObj() : Obj(Kind::Thread) {}
    pthread_t th{};
    uint32_t done = 0, resumed = 0;
    LPTHREAD_START_ROUTINE fn = nullptr;
    void* arg = nullptr;
    DWORD code = 0;
    uint64_t tid = 0;
    sigjmp_buf exit_jmp;
    bool has_jmp = false;
    void* stack = nullptr;   // from plat::big_alloc (PS5), else the system's
    size_t stack_size = 0;
};
struct EventObj : Obj {
    EventObj() : Obj(Kind::Event) {}
    uint32_t state = 0;
    bool manual = false;
};
struct TimerObj : Obj {
    TimerObj() : Obj(Kind::Timer) {}
    uint64_t due_ns = 0;
};
struct SectionObj : Obj {
    SectionObj() : Obj(Kind::Section) {}
    int fd = -1;
    uint64_t size = 0;
};
struct FileObj : Obj {
    FileObj() : Obj(Kind::File) {}
    int fd = -1;
};

thread_local ThreadObj* t_self;
std::atomic<uint32_t> g_next_tid{1};

void unref(Obj* o) {
    if (o && o->refs.fetch_sub(1) == 1) {
        if (o->kind == Kind::Section) plat::shared_memory_destroy(static_cast<SectionObj*>(o)->fd);
        if (o->kind == Kind::File) close(static_cast<FileObj*>(o)->fd);
        delete o;
    }
}

ThreadObj* adopt_current() {   // a host thread the shim did not create (the process's main thread) gets an object on demand
    if (!t_self) {
        t_self = new ThreadObj;
        t_self->th = pthread_self();
        t_self->tid = plat::thread_id();
        t_self->resumed = 1;
        t_self->refs = 1 << 20;   // never freed
    }
    return t_self;
}

// Threads whose stack came from plat::big_alloc are joinable: at the end each hands its pthread and stack to this list, and
// the next CreateThread joins it (gone or about to be) and frees the stack.
struct DeadStack {
    pthread_t th;
    void* stack;
};
std::mutex g_dead_mu;
std::vector<DeadStack> g_dead;
void reap_dead_stacks() {
    std::vector<DeadStack> d;
    {
        std::lock_guard<std::mutex> lk(g_dead_mu);
        d.swap(g_dead);
    }
    for (DeadStack& x : d) {
        pthread_join(x.th, nullptr);
        plat::big_free(x.stack);
    }
}

void* thread_trampoline(void* p) {
    ThreadObj* t = static_cast<ThreadObj*>(p);
    t_self = t;
    t->tid = plat::thread_id();
    while (!load(&t->resumed)) plat::futex_wait(&t->resumed, 0, -1);
    if (sigsetjmp(t->exit_jmp, 0) == 0) {
        t->has_jmp = true;
        t->code = t->fn(t->arg);
    }
    t->has_jmp = false;
    if (t->stack) {
        std::lock_guard<std::mutex> lk(g_dead_mu);
        g_dead.push_back(DeadStack{pthread_self(), t->stack});
    }
    __atomic_store_n(&t->done, 1u, __ATOMIC_RELEASE);
    plat::futex_wake(&t->done, true);
    unref(t);   // the thread's own reference
    return nullptr;
}

bool wait_word(uint32_t* w, uint32_t until_nonzero_of, DWORD ms) {   // wait until *w != 0 (until_nonzero_of unused: clarity)
    (void)until_nonzero_of;
    const uint64_t deadline = ms == INFINITE ? 0 : plat::now_ns() + (uint64_t)ms * 1000000ull;
    for (;;) {
        if (load(w)) return true;
        if (ms == INFINITE) { plat::futex_wait(w, 0, -1); continue; }
        const uint64_t now = plat::now_ns();
        if (now >= deadline) return false;
        plat::futex_wait(w, 0, (int64_t)(deadline - now));
    }
}

// ---------------------------------------------------------------- virtual memory regions (for VirtualQuery)
struct Region {
    uint64_t size;
    std::vector<uint8_t> page;   // per 4 KiB page: 0 reserved, else the PAGE_* protection of a committed page
    bool section_view = false;
    int section = -1;            // a view: the platform's shared-memory handle and the view's offset in it
    uint64_t section_off = 0;
};
std::mutex g_vm_lock;
std::map<uint64_t, Region> g_regions;
constexpr uint64_t kPg = 0x1000;
// Commit state of each section (by platform handle), per 4 KiB page: as on Windows a page committed through one view is
// committed in the section, and every view of it shows the page (with that view's access) until the view protects it.
std::map<int, std::vector<uint8_t>> g_section_pages;

// Section pages [soff, soff+len) were committed: open them in the other views that still show them as reserved.
void section_committed(int section, uint64_t soff, uint64_t len, uint64_t except_view) {
    std::vector<uint8_t>& sp = g_section_pages[section];
    for (uint64_t o = soff; o < soff + len; o += kPg)
        if (o / kPg < sp.size()) sp[o / kPg] = 1;
    for (auto& [base, r] : g_regions) {
        if (!r.section_view || r.section != section || base == except_view) continue;
        const uint64_t lo = std::max<uint64_t>(soff, r.section_off), hi = std::min<uint64_t>(soff + len, r.section_off + r.size);
        for (uint64_t o = lo; o < hi;) {
            uint64_t q = o;
            while (q < hi && r.page[(q - r.section_off) / kPg] == 0) q += kPg;
            if (q > o) {
                plat::vm_protect(base + (o - r.section_off), q - o, plat::kReadWrite);
                for (uint64_t x = o; x < q; x += kPg) r.page[(x - r.section_off) / kPg] = PAGE_READWRITE;
                o = q;
            } else {
                o += kPg;
            }
        }
    }
}

plat::Prot to_prot(DWORD p) {
    switch (p & 0xFF) {
    case PAGE_NOACCESS: return plat::kNone;
    case PAGE_READONLY: case PAGE_EXECUTE_READ: case PAGE_EXECUTE: return plat::kRead;
    default: return plat::kReadWrite;
    }
}

std::map<uint64_t, Region>::iterator region_of(uint64_t a) {
    auto it = g_regions.upper_bound(a);
    if (it == g_regions.begin()) return g_regions.end();
    --it;
    return a < it->first + it->second.size ? it : g_regions.end();
}

}  // namespace

// file handles for win32_files.cpp
HANDLE shim_file_new(int fd) {
    auto* f = new FileObj;
    f->fd = fd;
    return f;
}
int shim_file_fd(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || h == GetCurrentThread() || h == GetCurrentProcess()) return -1;
    auto* o = static_cast<Obj*>(h);
    return o->kind == Kind::File ? static_cast<FileObj*>(o)->fd : -1;
}

// ================================================================= synchronisation
void InitializeSRWLock(PSRWLOCK l) { l->Ptr = nullptr; }

void AcquireSRWLockExclusive(PSRWLOCK l) {
    uint32_t* w = word(&l->Ptr);
    for (int spin = 0;; spin++) {
        uint32_t v = load(w);
        if (v == 0 && __atomic_compare_exchange_n(w, &v, kWriter, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if (spin < 64) { __builtin_ia32_pause(); continue; }
        if (v) plat::futex_wait(w, v, -1);
    }
}

void AcquireSRWLockShared(PSRWLOCK l) {
    uint32_t* w = word(&l->Ptr);
    for (int spin = 0;; spin++) {
        uint32_t v = load(w);
        if (!(v & kWriter) && __atomic_compare_exchange_n(w, &v, v + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if (spin < 64) { __builtin_ia32_pause(); continue; }
        if (v & kWriter) plat::futex_wait(w, v, -1);
    }
}

void ReleaseSRWLockExclusive(PSRWLOCK l) {
    __atomic_store_n(word(&l->Ptr), 0u, __ATOMIC_RELEASE);
    plat::futex_wake(word(&l->Ptr), true);
}

void ReleaseSRWLockShared(PSRWLOCK l) {
    if (__atomic_sub_fetch(word(&l->Ptr), 1u, __ATOMIC_RELEASE) == 0) plat::futex_wake(word(&l->Ptr), true);
}

BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK l) {
    uint32_t v = 0;
    return __atomic_compare_exchange_n(word(&l->Ptr), &v, kWriter, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

BOOLEAN TryAcquireSRWLockShared(PSRWLOCK l) {
    uint32_t v = load(word(&l->Ptr));
    return !(v & kWriter) && __atomic_compare_exchange_n(word(&l->Ptr), &v, v + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void InitializeConditionVariable(PCONDITION_VARIABLE cv) { cv->Ptr = nullptr; }

BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags) {
    uint32_t* seq = word(&cv->Ptr);
    const uint32_t s = load(seq);
    const bool shared = flags & CONDITION_VARIABLE_LOCKMODE_SHARED;
    if (shared) ReleaseSRWLockShared(l); else ReleaseSRWLockExclusive(l);
    const bool woke = plat::futex_wait(seq, s, ms == INFINITE ? -1 : (int64_t)ms * 1000000);
    if (shared) AcquireSRWLockShared(l); else AcquireSRWLockExclusive(l);
    if (!woke && load(seq) == s) { t_last_error = ERROR_TIMEOUT; return FALSE; }
    return TRUE;
}

void WakeConditionVariable(PCONDITION_VARIABLE cv) {
    __atomic_add_fetch(word(&cv->Ptr), 1u, __ATOMIC_RELEASE);
    plat::futex_wake(word(&cv->Ptr), false);
}

void WakeAllConditionVariable(PCONDITION_VARIABLE cv) {
    __atomic_add_fetch(word(&cv->Ptr), 1u, __ATOMIC_RELEASE);
    plat::futex_wake(word(&cv->Ptr), true);
}

// ================================================================= time
void Sleep(DWORD ms) {
    if (ms == 0) { plat::yield_cpu(); return; }
    timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}
BOOL SwitchToThread() { plat::yield_cpu(); return TRUE; }
BOOL QueryPerformanceCounter(LARGE_INTEGER* v) { v->QuadPart = (LONGLONG)plat::now_ns(); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER* v) { v->QuadPart = 1000000000; return TRUE; }
ULONGLONG GetTickCount64() { return plat::now_ns() / 1000000ull; }
DWORD GetTickCount() { return (DWORD)GetTickCount64(); }

HANDLE CreateWaitableTimerExW(void*, LPCWSTR, DWORD, DWORD) { return new TimerObj; }
BOOL SetWaitableTimer(HANDLE t, const LARGE_INTEGER* due, LONG, void*, void*, BOOL) {
    // negative: relative, in 100 ns units (what nxrt uses); positive absolute FILETIME is not needed
    const int64_t d = due->QuadPart;
    static_cast<TimerObj*>(t)->due_ns = plat::now_ns() + (uint64_t)(d < 0 ? -d : 0) * 100ull;
    return TRUE;
}
BOOL SetWaitableTimerEx(HANDLE t, const LARGE_INTEGER* due, LONG period, void* r, void* a, void*, ULONG) {
    return SetWaitableTimer(t, due, period, r, a, FALSE);
}

// ================================================================= threads, events, mutexes
HANDLE CreateThread(void*, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid) {
    auto* t = new ThreadObj;
    t->fn = fn;
    t->arg = arg;
    t->resumed = (flags & CREATE_SUSPENDED) ? 0 : 1;
    t->refs = 2;   // the handle and the running thread
    reap_dead_stacks();
    pthread_attr_t a;
    pthread_attr_init(&a);
    // Windows reserves 1 MiB by default and nxrt's helpers keep large buffers on the stack; macOS gives 512 KiB. 16 MiB floor.
    constexpr SIZE_T kMinStack = 16u << 20;
    const size_t size = ((stack < kMinStack ? kMinStack : stack) + 0xFFFF) & ~(size_t)0xFFFF;
    t->stack = plat::big_alloc(size);   // PS5: direct memory (flexible memory is too small for 16 MiB per thread)
    t->stack_size = size;
    if (t->stack) pthread_attr_setstack(&a, t->stack, size);
    else pthread_attr_setstacksize(&a, size);
    const int r = pthread_create(&t->th, &a, thread_trampoline, t);
    pthread_attr_destroy(&a);
    if (r != 0) {
        if (t->stack) plat::big_free(t->stack);
        delete t;
        t_last_error = ERROR_NOT_ENOUGH_MEMORY;
        return nullptr;
    }
    if (!t->stack) pthread_detach(t->th);
    if (tid) *tid = (DWORD)(uintptr_t)t;   // the real tid is known once the thread runs; nxrt uses it only as a label
    return t;
}

DWORD ResumeThread(HANDLE h) {
    auto* t = static_cast<ThreadObj*>(h);
    if (!t || t->kind != Kind::Thread) return (DWORD)-1;
    const uint32_t was = __atomic_exchange_n(&t->resumed, 1u, __ATOMIC_ACQ_REL);
    plat::futex_wake(&t->resumed, true);
    return was ? 0 : 1;
}

DWORD SuspendThread(HANDLE) { t_last_error = ERROR_NOT_SUPPORTED; return (DWORD)-1; }   // samplers / watchdogs only

void ExitThread(DWORD code) {
    ThreadObj* t = t_self;
    if (t && t->has_jmp) { t->code = code; siglongjmp(t->exit_jmp, 1); }
    pthread_exit(nullptr);
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    // the pseudo-handles of the current process / thread (and OpenProcess of a live process, see win32_files.cpp) are
    // signalled only when that process or thread ends: from here, never
    if (h == GetCurrentProcess() || h == GetCurrentThread()) {
        if (ms == INFINITE) for (;;) plat::sleep_us(1000000);
        if (ms) plat::sleep_us(ms * 1000u);
        return WAIT_TIMEOUT;
    }
    Obj* o = static_cast<Obj*>(h);
    if (!o || h == INVALID_HANDLE_VALUE) return WAIT_FAILED;
    switch (o->kind) {
    case Kind::Thread: return wait_word(&static_cast<ThreadObj*>(o)->done, 0, ms) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    case Kind::Event: {
        auto* e = static_cast<EventObj*>(o);
        for (;;) {
            if (!wait_word(&e->state, 0, ms)) return WAIT_TIMEOUT;
            if (e->manual) return WAIT_OBJECT_0;
            uint32_t one = 1;
            if (__atomic_compare_exchange_n(&e->state, &one, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return WAIT_OBJECT_0;
        }
    }
    case Kind::Timer: {
        const uint64_t due = static_cast<TimerObj*>(o)->due_ns, now = plat::now_ns();
        if (due > now) {
            const uint64_t wait = ms == INFINITE ? due - now : std::min<uint64_t>(due - now, (uint64_t)ms * 1000000ull);
            timespec ts{(time_t)(wait / 1000000000ull), (long)(wait % 1000000000ull)};
            while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
        }
        return plat::now_ns() >= due ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }
    case Kind::Mutex: return WAIT_OBJECT_0;
    default: return WAIT_FAILED;
    }
}

BOOL CloseHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || h == GetCurrentThread() || h == GetCurrentProcess()) return TRUE;
    unref(static_cast<Obj*>(h));
    return TRUE;
}

BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, HANDLE* out, DWORD, BOOL, DWORD) {
    Obj* o = src == GetCurrentThread() ? adopt_current() : static_cast<Obj*>(src);
    if (!o) return FALSE;
    o->refs.fetch_add(1);
    *out = o;
    return TRUE;
}

HANDLE GetCurrentThread() { return (HANDLE)(intptr_t)-2; }
HANDLE GetCurrentProcess() { return (HANDLE)(intptr_t)-1; }
DWORD GetCurrentThreadId() { return (DWORD)plat::thread_id(); }
DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
DWORD GetThreadId(HANDLE h) {
    if (h == GetCurrentThread()) return GetCurrentThreadId();
    auto* t = static_cast<ThreadObj*>(h);
    return t && t->kind == Kind::Thread ? (DWORD)t->tid : 0;
}
DWORD GetCurrentProcessorNumber() { return 0; }
void GetCurrentProcessorNumberEx(PROCESSOR_NUMBER* p) { p->Group = 0; p->Number = 0; p->Reserved = 0; }
// Priorities and affinity are applied by the console core model of the port (thread_port); the Win32 calls report success.
BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
int GetThreadPriority(HANDLE) { return THREAD_PRIORITY_NORMAL; }
BOOL SetThreadPriorityBoost(HANDLE, BOOL) { return TRUE; }
DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR mask) { return mask ? mask : 1; }
BOOL SetThreadIdealProcessorEx(HANDLE, PROCESSOR_NUMBER*, PROCESSOR_NUMBER* prev) {
    if (prev) { prev->Group = 0; prev->Number = 0; prev->Reserved = 0; }
    return TRUE;
}
BOOL GetThreadIdealProcessorEx(HANDLE, PROCESSOR_NUMBER* ideal) { ideal->Group = 0; ideal->Number = 0; ideal->Reserved = 0; return TRUE; }
BOOL GetThreadTimes(HANDLE h, FILETIME* c, FILETIME* e, FILETIME* k, FILETIME* u) {
    *c = *e = *k = *u = FILETIME{0, 0};
    if (h != GetCurrentThread()) return TRUE;
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    const uint64_t t100 = ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) / 100ull;
    u->dwLowDateTime = (DWORD)t100;
    u->dwHighDateTime = (DWORD)(t100 >> 32);
    return TRUE;
}
BOOL SetThreadStackGuarantee(ULONG* size) { if (size) *size = 0; return TRUE; }

HANDLE CreateEventA(void*, BOOL manual, BOOL initial, LPCSTR) {
    auto* e = new EventObj;
    e->manual = manual;
    e->state = initial ? 1 : 0;
    return e;
}
HANDLE CreateEventW(void* sa, BOOL manual, BOOL initial, LPCWSTR) { return CreateEventA(sa, manual, initial, nullptr); }
BOOL SetEvent(HANDLE h) {
    auto* e = static_cast<EventObj*>(h);
    __atomic_store_n(&e->state, 1u, __ATOMIC_RELEASE);
    plat::futex_wake(&e->state, true);
    return TRUE;
}
BOOL ResetEvent(HANDLE h) { __atomic_store_n(&static_cast<EventObj*>(h)->state, 0u, __ATOMIC_RELEASE); return TRUE; }
HANDLE CreateMutexA(void*, BOOL, LPCSTR) { t_last_error = 0; return new Obj(Kind::Mutex); }   // never ALREADY_EXISTS: one process
void ExitProcess(UINT code) { std::fflush(nullptr); std::exit((int)code); }
BOOL IsBadReadPtr(const void* p, UINT_PTR_SHIM n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return TRUE;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS) {
        // not a region of the shim (host heap, stack, the executable): readable by construction
        return mbi.State == MEM_RESERVE ? TRUE : FALSE;
    }
    (void)n;
    return FALSE;
}
HANDLE OpenThread(DWORD, BOOL, DWORD tid) {
    if (tid != GetCurrentThreadId()) return nullptr;
    HANDLE h = nullptr;
    DuplicateHandle(nullptr, GetCurrentThread(), nullptr, &h, 0, FALSE, 0);
    return h;
}
// No processor topology: nxrt's core model falls back to "every host processor" (its documented behaviour when the call fails).
BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, DWORD* len) {
    if (len) *len = 0;
    t_last_error = ERROR_NOT_SUPPORTED;
    return FALSE;
}
BOOL GetProcessAffinityMask(HANDLE, DWORD_PTR* proc, DWORD_PTR* sys) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (proc) *proc = si.dwActiveProcessorMask;
    if (sys) *sys = si.dwActiveProcessorMask;
    return TRUE;
}
BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }
DWORD GetPriorityClass(HANDLE) { return NORMAL_PRIORITY_CLASS; }
BOOL ReleaseMutex(HANDLE) { return TRUE; }

DWORD GetLastError() { return t_last_error; }
void SetLastError(DWORD e) { t_last_error = e; }

void GetSystemInfo(SYSTEM_INFO* si) {
    std::memset(si, 0, sizeof *si);
    si->dwPageSize = 0x1000;
    si->dwAllocationGranularity = 0x10000;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1;
    si->dwActiveProcessorMask = (DWORD_PTR)((1ull << (si->dwNumberOfProcessors > 63 ? 63 : si->dwNumberOfProcessors)) - 1);
    si->lpMinimumApplicationAddress = (LPVOID)0x10000;
    si->lpMaximumApplicationAddress = (LPVOID)0x7FFFFFFEFFFFull;
}

void RaiseException(DWORD code, DWORD, DWORD, const ULONG_PTR*) {
    std::fprintf(stderr, "[nxrt-port] fatal exception 0x%08x: aborting\n", code);
    std::fflush(stderr);
    std::abort();
}
void TerminateProcess(HANDLE, UINT code) { std::fflush(nullptr); _exit((int)code); }

BOOL IsDebuggerPresent() { return FALSE; }
void OutputDebugStringA(LPCSTR s) { if (s) plat::log_console(s, std::strlen(s)); }
HMODULE GetModuleHandleA(LPCSTR) { return nullptr; }
DWORD GetModuleFileNameA(HMODULE, LPSTR out, DWORD n) { if (n) out[0] = 0; return 0; }
DWORD GetModuleFileNameW(HMODULE, wchar_t* out, DWORD n) { if (n) out[0] = 0; return 0; }
void* GetProcAddress(HMODULE, LPCSTR) { return nullptr; }
DWORD GetFileAttributesA(LPCSTR path) { return access(path, F_OK) == 0 ? 0x80u : INVALID_FILE_ATTRIBUTES; }
BOOL DeleteFileA(LPCSTR path) { return unlink(path) == 0; }

// ================================================================= virtual memory
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot) {
    std::lock_guard<std::mutex> lk(g_vm_lock);
    uint64_t a = (uint64_t)(uintptr_t)addr;
    size = (size + kPg - 1) & ~(kPg - 1);
    char how[160];
    if (type & MEM_RESERVE) {
        a &= ~(0x10000ull - 1);
        if (!a) {   // the system picks: anonymous mapping (small runtime buffers)
            void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (p == MAP_FAILED) { t_last_error = ERROR_NOT_ENOUGH_MEMORY; return nullptr; }
            a = (uint64_t)(uintptr_t)p;
        } else if (!plat::vm_reserve(a, size, how, sizeof how)) {
            t_last_error = ERROR_INVALID_ADDRESS;
            return nullptr;
        }
        g_regions[a] = Region{size, std::vector<uint8_t>(size / kPg, 0)};
    }
    if (type & MEM_COMMIT) {
        auto it = region_of(a);
        if (it == g_regions.end()) { t_last_error = ERROR_INVALID_ADDRESS; return nullptr; }
        const uint64_t lo = a & ~(kPg - 1), hi = (a + size + kPg - 1) & ~(kPg - 1);
        // commit only the pages that are not committed yet (contents of committed pages are kept); pages committed
        // already take the new protection, as on Windows (nxrt's heap_commit re-opens pages heap_decommit made no-access)
        Region& r = it->second;
        for (uint64_t p = lo; p < hi;) {
            uint64_t q = p;
            while (q < hi && r.page[(q - it->first) / kPg] != 0) q += kPg;
            if (q > p) {
                plat::vm_protect(p, q - p, to_prot(prot));
                for (uint64_t x = p; x < q; x += kPg) r.page[(x - it->first) / kPg] = (uint8_t)(prot & 0xFF);
                p = q;
                continue;
            }
            while (q < hi && r.page[(q - it->first) / kPg] == 0) q += kPg;
            if (q > p) {
                if (!r.section_view && !plat::vm_commit(p, q - p, to_prot(prot), how, sizeof how)) {
                    t_last_error = ERROR_NOT_ENOUGH_MEMORY;
                    return nullptr;
                }
                if (r.section_view) {   // back the section's pages (every view sees them), then open this view
                    if (!plat::shared_memory_commit(r.section, r.section_off + (p - it->first), q - p)) {
                        t_last_error = ERROR_NOT_ENOUGH_MEMORY;
                        return nullptr;
                    }
                    plat::vm_protect(p, q - p, to_prot(prot));
                    section_committed(r.section, r.section_off + (p - it->first), q - p, it->first);
                }
                for (uint64_t x = p; x < q; x += kPg) r.page[(x - it->first) / kPg] = (uint8_t)(prot & 0xFF);
                p = q;
            } else {
                p += kPg;
            }
        }
        return (LPVOID)(uintptr_t)a;
    }
    return (LPVOID)(uintptr_t)a;
}

BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    std::lock_guard<std::mutex> lk(g_vm_lock);
    const uint64_t a = (uint64_t)(uintptr_t)addr;
    auto it = region_of(a);
    if (it == g_regions.end()) { t_last_error = ERROR_INVALID_ADDRESS; return FALSE; }
    if (type & MEM_RELEASE) {
        if (it->first != a) { t_last_error = ERROR_INVALID_PARAMETER; return FALSE; }
        plat::vm_release(it->first, it->second.size);
        g_regions.erase(it);
        return TRUE;
    }
    if (type & MEM_DECOMMIT) {
        const uint64_t lo = a & ~(kPg - 1), hi = (a + size + kPg - 1) & ~(kPg - 1);
        plat::vm_decommit(lo, hi - lo);
        for (uint64_t x = lo; x < hi; x += kPg) it->second.page[(x - it->first) / kPg] = 0;
        return TRUE;
    }
    return FALSE;
}

BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD* old) {
    std::lock_guard<std::mutex> lk(g_vm_lock);
    const uint64_t a = (uint64_t)(uintptr_t)addr;
    const uint64_t lo = a & ~(kPg - 1), hi = (a + size + kPg - 1) & ~(kPg - 1);
    auto it = region_of(lo);
    if (old) *old = it != g_regions.end() && it->second.page[(lo - it->first) / kPg] ? it->second.page[(lo - it->first) / kPg] : PAGE_READWRITE;
    if (!plat::vm_protect(lo, hi - lo, to_prot(prot))) return FALSE;
    if (it != g_regions.end())
        for (uint64_t x = lo; x < hi && x < it->first + it->second.size; x += kPg)
            if (it->second.page[(x - it->first) / kPg]) it->second.page[(x - it->first) / kPg] = (uint8_t)(prot & 0xFF);
    return TRUE;
}

SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len) {
    if (len < sizeof *mbi) return 0;
    std::lock_guard<std::mutex> lk(g_vm_lock);
    const uint64_t a = (uint64_t)(uintptr_t)addr & ~(kPg - 1);
    std::memset(mbi, 0, sizeof *mbi);
    auto it = region_of(a);
    if (it == g_regions.end()) {   // free: up to the next region
        auto nx = g_regions.upper_bound(a);
        mbi->BaseAddress = (PVOID)(uintptr_t)a;
        mbi->RegionSize = nx == g_regions.end() ? 0x10000 : (SIZE_T)(nx->first - a);
        mbi->State = MEM_FREE;
        mbi->Protect = PAGE_NOACCESS;
        return sizeof *mbi;
    }
    const Region& r = it->second;
    const size_t i0 = (a - it->first) / kPg;
    const uint8_t s = r.page[i0];
    size_t i = i0;
    while (i < r.page.size() && r.page[i] == s) i++;
    mbi->BaseAddress = (PVOID)(uintptr_t)a;
    mbi->AllocationBase = (PVOID)(uintptr_t)it->first;
    mbi->AllocationProtect = PAGE_READWRITE;
    mbi->RegionSize = (i - i0) * kPg;
    mbi->State = s ? MEM_COMMIT : MEM_RESERVE;
    mbi->Protect = s ? s : 0;
    mbi->Type = r.section_view ? MEM_MAPPED : MEM_PRIVATE;
    return sizeof *mbi;
}

BOOL FlushInstructionCache(HANDLE, LPCVOID, SIZE_T) { return TRUE; }

// ================================================================= sections and views
HANDLE CreateFileMappingW(HANDLE, void*, DWORD, DWORD size_high, DWORD size_low, LPCWSTR) {
    const uint64_t size = ((uint64_t)size_high << 32) | size_low;
    int fd = plat::shared_memory_create(size);
    if (fd < 0) { t_last_error = ERROR_NOT_ENOUGH_MEMORY; return nullptr; }
    auto* s = new SectionObj;
    s->fd = fd;
    s->size = size;
    {
        std::lock_guard<std::mutex> lk(g_vm_lock);
        g_section_pages[fd].assign((size + kPg - 1) / kPg, 0);
    }
    return s;
}

LPVOID MapViewOfFileEx(HANDLE section, DWORD, DWORD off_high, DWORD off_low, SIZE_T size, LPVOID at) {
    auto* s = static_cast<SectionObj*>(section);
    const uint64_t off = ((uint64_t)off_high << 32) | off_low;
    if (!size) size = (SIZE_T)(s->size - off);
    void* p = plat::shared_memory_map(s->fd, off, size, (uint64_t)(uintptr_t)at);
    if (!p) { t_last_error = ERROR_INVALID_ADDRESS; return nullptr; }
    std::lock_guard<std::mutex> lk(g_vm_lock);
    // a SEC_RESERVE section: pages the section has not committed are reserved in the view (no access); the ones committed
    // through another view are there already, read-write (alias views of the heap, the GPU back end's private views)
    Region r{(uint64_t)size, std::vector<uint8_t>(size / kPg, 0), true, s->fd, off};
    const uint64_t va = (uint64_t)(uintptr_t)p;
    plat::vm_protect(va, size, plat::kNone);
    const std::vector<uint8_t>& sp = g_section_pages[s->fd];
    for (uint64_t o = 0; o < size;) {
        const auto committed = [&](uint64_t x) { return (off + x) / kPg < sp.size() && sp[(off + x) / kPg]; };
        if (!committed(o)) { o += kPg; continue; }
        uint64_t q = o;
        while (q < size && committed(q)) q += kPg;
        plat::vm_protect(va + o, q - o, plat::kReadWrite);
        for (uint64_t x = o; x < q; x += kPg) r.page[x / kPg] = PAGE_READWRITE;
        o = q;
    }
    g_regions[va] = std::move(r);
    return p;
}

LPVOID MapViewOfFile(HANDLE section, DWORD access, DWORD off_high, DWORD off_low, SIZE_T size) {
    return MapViewOfFileEx(section, access, off_high, off_low, size, nullptr);
}

BOOL UnmapViewOfFile(LPCVOID view) {
    std::lock_guard<std::mutex> lk(g_vm_lock);
    auto it = g_regions.find((uint64_t)(uintptr_t)view);
    if (it == g_regions.end()) return FALSE;
    plat::shared_memory_unmap(it->first, it->second.size);
    g_regions.erase(it);
    return TRUE;
}
