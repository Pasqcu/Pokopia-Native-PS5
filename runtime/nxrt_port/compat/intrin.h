#pragma once
// MSVC intrin.h on clang/x86-64: the x86 intrinsics, plus MSVC's __cpuid / __cpuidex (cpuid.h's __cpuid is a 5-argument macro)
#include <x86intrin.h>
#include <cpuid.h>
#undef __cpuid
static inline void __cpuid(int info[4], int leaf) {
    unsigned a, b, c, d;
    __cpuid_count((unsigned)leaf, 0u, a, b, c, d);
    info[0] = (int)a, info[1] = (int)b, info[2] = (int)c, info[3] = (int)d;
}
// __cpuidex: clang's cpuid.h has it
