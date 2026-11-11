// SPDX-License-Identifier: GPL-3.0-or-later
// Included first in every nxrt source of the port (-include): the Win32 shim is visible even where upstream relies on
// <windows.h> arriving through another header, and the C++20 pieces the PS5 SDK's libc++ lacks are filled in.
#pragma once
#include "windows.h"

#ifdef __cplusplus
#include <atomic>
#include <version>
#if !defined(__cpp_lib_atomic_ref)
// std::atomic_ref (C++20, libc++ 19+): the subset nxrt uses (hid.cpp's lock-free rings), on the __atomic builtins.
namespace std {
template <class T>
class atomic_ref {
    T* p_;
    static constexpr int mo(memory_order o) {
        return o == memory_order_relaxed ? __ATOMIC_RELAXED : o == memory_order_acquire ? __ATOMIC_ACQUIRE
             : o == memory_order_release ? __ATOMIC_RELEASE : o == memory_order_acq_rel ? __ATOMIC_ACQ_REL : __ATOMIC_SEQ_CST;
    }

public:
    explicit atomic_ref(T& v) noexcept : p_(&v) {}
    void store(T v, memory_order o = memory_order_seq_cst) const noexcept { __atomic_store_n(p_, v, mo(o)); }
    T load(memory_order o = memory_order_seq_cst) const noexcept { return __atomic_load_n(p_, mo(o)); }
    T exchange(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_exchange_n(p_, v, mo(o)); }
    T fetch_add(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_add(p_, v, mo(o)); }
    T fetch_sub(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_sub(p_, v, mo(o)); }
    bool compare_exchange_strong(T& e, T d, memory_order o = memory_order_seq_cst) const noexcept {
        return __atomic_compare_exchange_n(p_, &e, d, false, mo(o), __ATOMIC_RELAXED);
    }
    operator T() const noexcept { return load(); }
};
}  // namespace std
#endif
#endif

// FreeBSD / PS5: <sys/endian.h> (SDL's headers bring it) makes bswap16/32/64 macros, which then rewrite the declarations of
// nxrecomp's ctx.h functions of the same names. Included first here and the macros dropped, its include guard keeps them out.
#if defined(__FreeBSD__)
#include <sys/endian.h>
#undef bswap16
#undef bswap32
#undef bswap64
#endif
