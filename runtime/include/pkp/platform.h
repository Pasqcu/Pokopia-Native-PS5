// SPDX-License-Identifier: GPL-3.0-or-later
// Pokopia Native on PS5: the platform layer the runtime is written against. Two implementations:
//   runtime/platform/posix     macOS / Linux / FreeBSD hosts (development, oracle tests)
//   runtime/platform/prospero  the PS5 title (public ps5-payload-sdk: libkernel, libSceLibcInternal, VideoOut)
// Guest memory is identity mapped: a guest address is the host address it lives at, so every reservation is made at a
// fixed address. The module bases (0x71_0000_0000 ...) are baked into the lifted code.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pkp::plat {

enum Prot : int { kNone = 0, kRead = 1, kReadWrite = 3 };

// Address space. reserve: claim [addr, addr+size) without backing; commit: back it (zero filled) with `prot`.
// `how` receives a short description of the primitive that succeeded or of the failure (for the bring-up log).
bool vm_reserve(uint64_t addr, uint64_t size, char* how, size_t how_len);
bool vm_commit(uint64_t addr, uint64_t size, Prot prot, char* how, size_t how_len);
bool vm_protect(uint64_t addr, uint64_t size, Prot prot);
void vm_decommit(uint64_t addr, uint64_t size);   // back to reserved: contents dropped, accesses fault
void vm_release(uint64_t addr, uint64_t size);
uint64_t vm_granule();   // reservation / commit granularity (16 KiB on the PS5, the host page size elsewhere)

// Shared memory: one zero-filled object whose pages several views map at once (the heap section of the HLE runtime and
// its alias views). POSIX: shm_open + mmap MAP_SHARED. PS5: direct memory mapped at several addresses.
int shared_memory_create(uint64_t size);                                      // handle, -1 on failure
void* shared_memory_map(int handle, uint64_t off, uint64_t size, uint64_t at);   // at == 0: anywhere; nullptr on failure
void shared_memory_unmap(uint64_t addr, uint64_t size);
// Back [off, off+size) of the object with memory in every view of it (a commit through any view). POSIX: the pages exist
// already (true). PS5: direct memory is allocated per 64 KiB chunk the first time and mapped into all the views.
bool shared_memory_commit(int handle, uint64_t off, uint64_t size);
void shared_memory_destroy(int handle);   // after its views are unmapped

// Memory statistics for the log (0 when the platform cannot tell).
struct MemInfo {
    uint64_t direct_total, flexible_available, page_size;
};
MemInfo mem_info();

// Lazily backed guest memory (runtime/platform/common/paged_vm.h: the PS5, or a POSIX host with PKP_PS5_VM=1).
// prefault: back the pages of [p, p+n) that are not backed yet, before a system call or the GPU uses them (the kernel
// returns EFAULT instead of raising a signal). It never faults: pages the range cannot have (not committed, no access)
// stay as they are, told on stderr. vm_handle_fault: for a SIGSEGV / SIGBUS handler, true when the fault was a
// lazily backed page that is now there (return and the access repeats).
void prefault(const void* p, size_t n);
bool vm_handle_fault(void* siginfo, void* ucontext);
struct VmStats {
    uint64_t region_backed, section_backed, section_committed, faults;   // bytes, bytes, bytes, count
    bool lazy;                                                           // the paged VM is in use
};
VmStats vm_stats();

// Large blocks outside the host heap. PS5: direct memory in 64 KiB granules, because the host heap (malloc) lives in the
// small flexible-memory budget (~448 MiB) together with the ~300 MB executable; the title's malloc / free / new send
// blocks of 1 MiB and more here (linker --wrap, runtime/platform/prospero/big_heap.cpp), and thread stacks come from here.
// Elsewhere big_alloc returns nullptr and callers use malloc.
void* big_alloc(size_t size);
void big_free(void* p);
bool big_owns(const void* p);
size_t big_size(const void* p);
struct BigStats {
    uint64_t bytes, blocks, peak_bytes;
};
BigStats big_stats();

// Time.
uint64_t now_ns();
void sleep_us(uint32_t us);
void yield_cpu();

// Futex on a 32-bit word: wait while *addr == expected (timeout_ns < 0: forever; returns false on timeout), wake one or all.
// macOS: os_sync_wait_on_address; FreeBSD / PS5: _umtx_op(UMTX_OP_WAIT_UINT_PRIVATE).
bool futex_wait(uint32_t* addr, uint32_t expected, int64_t timeout_ns);
void futex_wake(uint32_t* addr, bool all);

// Host thread identity (stable for the thread's lifetime, small integer where the OS has one).
uint64_t thread_id();

// Files (whole-file read; the runtime keeps module images itself).
bool read_file(const char* path, uint8_t** data, size_t* size);   // malloc'd buffer, caller frees
bool append_file(const char* path, const char* text, size_t len);

// Logging sinks: the platform's console (stderr, kernel log) plus any file opened with log_open_file.
void log_console(const char* line, size_t len);

// Runs fn(arg) and catches a fatal signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE) raised inside it: returns false and puts
// the signal and the faulting address into `why`. Guests that crash fail one test instead of the whole title.
bool run_guarded(void (*fn)(void*), void* arg, char* why, size_t why_len);

// Threads, with the guest's core mask translated by the CCX-aware map below.
using ThreadFn = void* (*)(void*);
bool thread_start(ThreadFn fn, void* arg, int guest_core, size_t stack_size);

// Zen 2 topology of the PS5: 8 cores in 2 CCX of 4 (cores 0-3, 4-7), each CCX with its own 4 MiB L3. The Switch 2 runs
// a game on 6 application cores. Guest cores 0-2 (the main, render and audio threads of most engines, which share the most
// data) go to CCX0 so they hit the same L3; guest cores 3-5 to CCX1. Returns a host CPU number (one hardware thread).
int host_cpu_for_guest_core(int guest_core);

// Host CPU description for the log.
void cpu_description(char* out, size_t len);

}  // namespace pkp::plat
