// SPDX-License-Identifier: GPL-3.0-or-later
// Guest memory on a host whose pages are larger than the guest's 4 KiB and whose backing is allocated explicitly: the
// PS5 (16 KiB pages, direct memory). Written once over a small backend, so a POSIX host can run the whole game on the
// same algorithm (PKP_PS5_VM=1) before a console run. The rules:
//
//  1. Reservations claim address space only. A commit backs whole host pages, page by page: a guest commit of 4 KiB
//     inside a page that a neighbour already backed zero-fills just that range (newly committed memory reads zero),
//     instead of mapping new memory over the neighbour's data.
//  2. Protections cannot be finer than a host page. A grant (read-write) rounds outwards, a restriction (read-only,
//     no access) rounds inwards: a well-behaved guest never notices, and data next to a read-only boundary stays
//     writable (module .rodata / .data boundaries, MapMemory sources of 4 KiB).
//  3. Shared-memory sections (the HLE heap and its alias views) are backed lazily, per 64 KiB chunk, on the first
//     access through any view (fault handler) or before a system call or the GPU uses them (prefault, which backs the
//     chunks without touching them): a game that asks for a 10 GiB heap pays for what it touches. Every backed chunk is
//     mapped in every view of its section. A new chunk is zeroed in a private window before any view shows it: zeroed
//     through a view, it would wipe what another thread writes there meanwhile (that access does not fault).
//  4. A decommit gives back whole pages only; partial pages stay backed (and read zero again when recommitted).
#pragma once

#include <cstddef>
#include <cstdint>

#include "pkp/platform.h"

namespace pkp::plat::pv {

struct Backend {
    uint64_t page;                                                          // host page size (power of two)
    bool (*reserve)(uint64_t va, uint64_t len, char* how, size_t how_len);  // claim [va, va+len), no backing
    uint64_t (*reserve_any)(uint64_t len, uint64_t align);                  // anywhere (0 on failure)
    void (*unreserve)(uint64_t va, uint64_t len);                           // unmap everything there
    int64_t (*alloc)(uint64_t len, uint64_t align);                         // physical backing, -1 when none is left
    void (*release)(int64_t phys, uint64_t len);
    bool (*map)(uint64_t va, uint64_t len, int64_t phys);                   // read-write, MAP_FIXED over reserved space
                                                                            // (or over a mapping of its own, replaced)
    void (*unmap_to_reserved)(uint64_t va, uint64_t len);                   // drop the backing, keep the reservation
    bool (*protect)(uint64_t va, uint64_t len, Prot prot);
    bool (*fault_is_write)(void* ucontext);                                 // the faulting access was a write
};

void init(const Backend& b);   // once, before any other call
bool active();

bool vm_reserve(uint64_t addr, uint64_t size, char* how, size_t how_len);
bool vm_commit(uint64_t addr, uint64_t size, Prot prot, char* how, size_t how_len);
bool vm_protect(uint64_t addr, uint64_t size, Prot prot);
void vm_decommit(uint64_t addr, uint64_t size);
void vm_release(uint64_t addr, uint64_t size);

int shared_memory_create(uint64_t size);
void* shared_memory_map(int handle, uint64_t off, uint64_t size, uint64_t at);
void shared_memory_unmap(uint64_t addr, uint64_t size);
bool shared_memory_commit(int handle, uint64_t off, uint64_t size);
void shared_memory_destroy(int handle);

void prefault(const void* p, size_t n);   // never faults: what cannot be backed is left alone (and told on stderr)
bool handle_fault(void* siginfo, void* ucontext);   // true: a lazily backed chunk is now there, retry the access
VmStats stats();

}  // namespace pkp::plat::pv
