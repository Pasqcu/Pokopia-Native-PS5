# CMake toolchain: PS5 (Prospero) native homebrew, public ps5-payload-sdk sysroot.
#
#   cmake -S . -B build/ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ps5.cmake [-DPS5_PAYLOAD_SDK=<dir>]
#
# Any clang >= 18 works as the compiler: the x86_64-sie-ps5 triple is upstream LLVM (it defaults to -target-cpu znver2,
# omits frame pointers and uses the FreeBSD-style ABI of the console). Apple clang (Xcode) included. Objects are ELF; no
# archiver is needed because the lifted modules are OBJECT libraries. Linking the title needs ld.lld (LLVM, or `zig ld.lld`).
#
# Not used: the SDK's own bin/prospero-clang wrappers, which need a host LLVM with llvm-config.
cmake_minimum_required(VERSION 3.25)

set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_VERSION 9)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CROSSCOMPILING ON)
set(PS5 TRUE)
set(PROSPERO TRUE)

get_filename_component(_pkp_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT PS5_PAYLOAD_SDK)
    if(DEFINED ENV{PS5_PAYLOAD_SDK})
        set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}")
    else()
        set(PS5_PAYLOAD_SDK "${_pkp_root}/third_party/ps5-payload-sdk-v0.45/ps5-payload-sdk")
    endif()
endif()
set(PS5_PAYLOAD_SDK "${PS5_PAYLOAD_SDK}" CACHE PATH "ps5-payload-sdk root (bin/, target/)")
if(NOT EXISTS "${PS5_PAYLOAD_SDK}/target/include/c++/v1/__config")
    message(FATAL_ERROR "ps5-payload-sdk not found at '${PS5_PAYLOAD_SDK}' (scripts/setup_host.sh fetches it)")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS5_PAYLOAD_SDK)

find_program(PKP_PS5_CLANG NAMES clang-21 clang-20 clang-19 clang-18 clang REQUIRED)
find_program(PKP_PS5_CLANGXX NAMES clang++-21 clang++-20 clang++-19 clang++-18 clang++ REQUIRED)
set(CMAKE_C_COMPILER "${PKP_PS5_CLANG}")
set(CMAKE_CXX_COMPILER "${PKP_PS5_CLANGXX}")
set(CMAKE_C_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_CXX_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_ASM_COMPILER_TARGET x86_64-sie-ps5)

set(CMAKE_SYSROOT "${PS5_PAYLOAD_SDK}/target")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Compile checks must not try to link a PS5 executable on the build machine.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The SDK's flags (bin/prospero-clang): PIC, no stack protector, no PLT, emulated TLS (the console's dynamic TLS model is
# not what clang emits by default for this triple in a homebrew title).
set(_pkp_ps5_common "-fPIC -fno-stack-protector -fno-plt -femulated-tls -fvisibility-nodllstorageclass=default")
set(CMAKE_C_FLAGS_INIT "${_pkp_ps5_common} -isystem \"${PS5_PAYLOAD_SDK}/target/include\"")
set(CMAKE_CXX_FLAGS_INIT "${_pkp_ps5_common} -nostdinc++ -isystem \"${PS5_PAYLOAD_SDK}/target/include/c++/v1\" -isystem \"${PS5_PAYLOAD_SDK}/target/include\"")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF)

# Static libraries of ELF objects need an ELF archiver (the build machine's may be Mach-O only, and the SDK's llvm-ar
# wrapper needs an llvm-config): Zig's llvm-ar (scripts/setup_host.sh fetches Zig), through two small scripts written next
# to the build. Libraries built here (SDL3, ...) are then ordinary archives.
if(EXISTS "${_pkp_root}/third_party/zig/zig" AND NOT CMAKE_AR)
    foreach(_pkp_tool ar ranlib)
        file(WRITE "${CMAKE_BINARY_DIR}/pkp-elf-${_pkp_tool}" "#!/bin/sh\nexec \"${_pkp_root}/third_party/zig/zig\" ${_pkp_tool} \"$@\"\n")
        file(CHMOD "${CMAKE_BINARY_DIR}/pkp-elf-${_pkp_tool}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
    endforeach()
    set(CMAKE_AR "${CMAKE_BINARY_DIR}/pkp-elf-ar" CACHE FILEPATH "ELF archiver" FORCE)
    set(CMAKE_RANLIB "${CMAKE_BINARY_DIR}/pkp-elf-ranlib" CACHE FILEPATH "ELF ranlib" FORCE)
endif()
