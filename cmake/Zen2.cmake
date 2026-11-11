# Optimisation policy for the AMD Zen 2 cores of the PS5 (8 cores / 16 threads, 2 CCX x 4 cores, 4 MiB L3 per CCX,
# 64-byte lines, 32 KiB L1I + 4K-op op cache per core, 256-bit FP/AVX2 datapaths, FMA3, BMI1/2 (PDEP/PEXT microcoded: avoid)).
#
# Two classes of code, two flag sets:
#   lifted   the C++ nxrecomp generates from the game (12 M AArch64 instructions). Big and cold on average: code size and
#            I-cache pressure matter more than unrolling. FP stays bit-exact: no contraction, no fast-math.
#   runtime  the HLE / OS layer and the PS5 platform code (hot loops: memcpy-class helpers, audio DSP, texture
#            deswizzle, dispatch). Full -O3 with unrolling.
#
# PKP_LTO: off | thin | full. ThinLTO lets the dispatcher and the HLE stubs inline into lifted code across modules. It
# needs an ld.lld of the SAME LLVM version as the compiler (bitcode objects). Full LTO over the lifted code is not
# practical (tens of GB of RAM, one thread); with PKP_LTO=full only the runtime is LTO'd and the lifted code is ThinLTO.
include_guard(GLOBAL)

set(PKP_LIFTED_OPT "-O2" CACHE STRING "optimisation level of the lifted game code")
set(PKP_LTO "off" CACHE STRING "link-time optimisation: off, thin, full")
set_property(CACHE PKP_LTO PROPERTY STRINGS off thin full)

if(PS5)
    # the console: Zen 2 ISA and scheduling (x86_64-sie-ps5 defaults to znver2; spelled out)
    set(PKP_ISA_FLAGS -march=znver2 -mtune=znver2)
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    # macOS host: the AVX2 / FMA / BMI2 subset of Zen 2 (x86-64-v3), WITHOUT Zen tuning. Apple clang 21 miscompiles Mach-O
    # thread-local accesses under -mtune=znver1..3: the _tlv_get_addr call gets its descriptor in %r8 instead of %rdi and
    # writes into a random thread-specific slot (found here: nxrt's log_set_thread_name corrupted the main pthread).
    # -march=znver2 would also allow SSE4a, which Intel hosts lack.
    set(PKP_ISA_FLAGS -march=x86-64-v3)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(PKP_ISA_FLAGS -march=x86-64-v3 -mtune=znver2)   # Linux / FreeBSD hosts: the Zen 2 subset, Zen 2 scheduling
else()
    set(PKP_ISA_FLAGS)
endif()

set(PKP_LIFTED_FLAGS
    ${PKP_LIFTED_OPT} ${PKP_ISA_FLAGS}
    -fno-exceptions -fno-rtti
    -fno-strict-aliasing      # guest memory is accessed through memcpy'd integers of any type
    -ffp-contract=off         # AArch64 rounds FMUL and FADD separately; FMADD is lifted as an explicit fma()
    -fno-math-errno
    -mcx16                    # 128-bit exclusives (LDXP/STXP) are cmpxchg16b
    -fomit-frame-pointer
    -ffunction-sections -fdata-sections
    -w)

set(PKP_RUNTIME_FLAGS
    -O3 ${PKP_ISA_FLAGS}
    -fno-exceptions -fno-rtti
    -fno-strict-aliasing -ffp-contract=off -mcx16
    -fomit-frame-pointer -funroll-loops
    -ffunction-sections -fdata-sections)

if(PKP_LTO STREQUAL "thin" OR PKP_LTO STREQUAL "full")
    list(APPEND PKP_LIFTED_FLAGS -flto=thin)
    if(PKP_LTO STREQUAL "full")
        list(APPEND PKP_RUNTIME_FLAGS -flto)
    else()
        list(APPEND PKP_RUNTIME_FLAGS -flto=thin)
    endif()
endif()

# lifted module from build/lift/<name>: c_000.cpp ... and module_<name>.cpp, as an OBJECT library (no archiver needed)
function(pkp_add_lifted_module name)
    set(dir "${PKP_LIFT_DIR}/${name}")
    file(GLOB shards CONFIGURE_DEPENDS "${dir}/c_*.cpp")
    if(NOT shards)
        message(FATAL_ERROR "no lifted sources in ${dir}: run scripts/lift.sh first")
    endif()
    add_library(pkp_${name} OBJECT ${shards} "${dir}/module_${name}.cpp")
    target_include_directories(pkp_${name} PUBLIC "${PKP_NXRECOMP_INCLUDE}" PRIVATE "${dir}")
    target_compile_options(pkp_${name} PRIVATE ${PKP_LIFTED_FLAGS})
    target_compile_features(pkp_${name} PUBLIC cxx_std_20)
endfunction()
