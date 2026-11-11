# Pokopia Native on PS5: static recompilation of Pokémon Pokopia (Switch 2) into native x86-64 code for the PS5 (Zen 2).
#
# Host (macOS / Linux x86-64), for the CPU self-test of the recompiled code:
#   cmake -S . -B build/host -G Ninja && ninja -C build/host && build/host/pkp_selftest
# PS5 objects (the title is linked and packaged by scripts/make_ppsa.sh):
#   cmake -S . -B build/ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ps5.cmake && ninja -C build/ps5
cmake_minimum_required(VERSION 3.25)
project(pokopia_native C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Zen2.cmake)

set(PKP_LIFT_DIR "${CMAKE_SOURCE_DIR}/build/lift" CACHE PATH "output of scripts/lift.sh")
set(PKP_NXRECOMP_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/pokopia-recomp/recomp/include" CACHE PATH "nxrecomp/ctx.h")
# Modules compiled. The title links the ones in PKP_TITLE_MODULES; main (9.95 M instructions) is compiled to prove the
# whole game builds for Zen 2 and is linked once the HLE runtime boots it (docs/ROADMAP.md).
set(PKP_MODULES "rtld;subsdk0;sdk;main" CACHE STRING "lifted modules to compile")
set(PKP_TITLE_MODULES "rtld;sdk" CACHE STRING "lifted modules linked into the bring-up title / self-test")

foreach(m IN LISTS PKP_MODULES)
    pkp_add_lifted_module(${m})
endforeach()

# Platform layer (posix or prospero), shared by the bring-up runtime and the HLE runtime port.
if(PS5)
    file(GLOB PKP_PLAT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/platform/prospero/*.cpp")
else()
    file(GLOB PKP_PLAT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/platform/posix/*.cpp")
endif()
# the paged VM of the PS5 (16 KiB pages, lazily backed sections), also run on POSIX hosts with PKP_PS5_VM=1
list(APPEND PKP_PLAT_SOURCES "${CMAKE_SOURCE_DIR}/runtime/platform/common/paged_vm.cpp")
# The PS5 SDK fork of PS5_Vulkan (v0.42 + its platform layer, libps5platform.a) is what RADV is built and validated with: a title
# on it takes the platform's heap (malloc in direct memory) and its stand-ins for the libc functions the console lacks
# (scripts/make_ppsa.sh links them), so this tree's own versions stay out: libc_compat.cpp, the allocator wrappers of
# big_heap.cpp and emutls.cpp's __cxa_thread_atexit_impl.
set(PKP_PS5_PLATFORM_LAYER OFF)
if(PS5 AND EXISTS "${PS5_PAYLOAD_SDK}/target/lib/libps5platform.a")
    set(PKP_PS5_PLATFORM_LAYER ON)
    list(FILTER PKP_PLAT_SOURCES EXCLUDE REGEX "/prospero/libc_compat[.]cpp$")
    message(STATUS "PS5 SDK with the platform layer (libps5platform): its heap and libc stand-ins are used")
endif()
add_library(pkp_platform OBJECT ${PKP_PLAT_SOURCES})
if(PKP_PS5_PLATFORM_LAYER)
    target_compile_definitions(pkp_platform PRIVATE PKP_PS5_PLATFORM_LAYER=1)
endif()
target_include_directories(pkp_platform PUBLIC "${CMAKE_SOURCE_DIR}/runtime/include")
target_compile_options(pkp_platform PRIVATE ${PKP_RUNTIME_FLAGS} -Wall -Wextra -Wno-unused-parameter)

# Bring-up runtime: loader, generated-code ABI, dispatch, CPU self-test.
file(GLOB PKP_RT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/src/*.cpp")
add_library(pkprt OBJECT ${PKP_RT_SOURCES})
target_include_directories(pkprt PUBLIC "${CMAKE_SOURCE_DIR}/runtime/include" "${PKP_NXRECOMP_INCLUDE}")
target_compile_options(pkprt PRIVATE ${PKP_RUNTIME_FLAGS} -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(pkprt PUBLIC pkp_platform)

# The Horizon HLE runtime (nxrt of pokopia-recomp) on this platform layer: kernel, memory, IPC, services, C++ exceptions.
option(PKP_NXRT_PORT "build the HLE runtime port and the game boot executable" ON)
if(PKP_NXRT_PORT)
    add_subdirectory(runtime/nxrt_port)
endif()
foreach(m IN LISTS PKP_TITLE_MODULES)
    string(TOUPPER "${m}" M)
    target_compile_definitions(pkprt PRIVATE PKP_HAVE_${M}=1)
endforeach()

if(PS5)
    # Everything the title links, as one list for scripts/make_ppsa.sh: $<TARGET_OBJECTS> of these targets.
    # The title's startup (CRT), allocation bridge and VideoOut renderer come from ps5-native-app-boilerplate (GPL-3.0,
    # hardware-validated on firmware 6.02 and 12.70), fetched into third_party/ by scripts/fetch_upstream.sh.
    # PKP_TITLE_STAGE: selftest = stage 1 (recompiled sdk CPU self-test, title_main.cc);
    #                  boot     = stage 2 (the whole game on the nxrt HLE runtime, title_boot.cc; needs all four modules)
    set(PKP_TITLE_STAGE "selftest" CACHE STRING "which title to build: selftest or boot")
    set_property(CACHE PKP_TITLE_STAGE PROPERTY STRINGS selftest boot)
    set(PKP_BOILERPLATE "${CMAKE_SOURCE_DIR}/third_party/ps5-native-app-boilerplate" CACHE PATH "ps5-native-app-boilerplate")
    set(_bp "${PKP_BOILERPLATE}/src/demo_renderer.cpp" "${PKP_BOILERPLATE}/tooling/native/app_crt.cpp")
    if(PKP_TITLE_STAGE STREQUAL "boot")
        foreach(m rtld main sdk subsdk0)
            if(NOT TARGET pkp_${m} OR NOT PKP_NXRT_PORT)
                message(FATAL_ERROR "PKP_TITLE_STAGE=boot needs PKP_NXRT_PORT=ON and PKP_MODULES=rtld;sdk;subsdk0;main")
            endif()
        endforeach()
        # the full libc++ / libc++abi / libunwind provide operator new and the exceptions: no boilerplate allocation bridge
        set(_whole "")
        if(PKP_NXRT_GFX)
            # stage 3: nxrt's NVN front end on RADV (PS5_Vulkan), the picture on VideoOut; the boilerplate's renderer stays out
            # (VideoOut is RADV's). RADV's archive is linked whole (Mesa's dispatch tables reach entry points by weak reference).
            if(NOT PKP_PS5_PLATFORM_LAYER)
                message(FATAL_ERROR "the graphics title needs the SDK fork with libps5platform (scripts/build_radv_ps5.sh)")
            endif()
            set(PKP_RADV_ARCHIVE "$ENV{HOME}/.cache/pokopia-native/ps5gfx/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
                CACHE FILEPATH "RADV for the PS5 (scripts/build_radv_ps5.sh)")
            add_library(pkp_title OBJECT "${CMAKE_SOURCE_DIR}/runtime/platform/prospero/title_gfx.cc"
                        "${CMAKE_SOURCE_DIR}/runtime/platform/prospero/title_crt.cc"
                        "${CMAKE_SOURCE_DIR}/runtime/platform/prospero/boot_screen.cc")
            target_link_libraries(pkp_title PRIVATE nxrt_port)
            set(_objs "$<TARGET_OBJECTS:pkp_title>\n$<TARGET_OBJECTS:nxrt_port>\n$<TARGET_OBJECTS:pkp_platform>")
            set(_libs "$<TARGET_FILE:nxrt_gfx>\n$<TARGET_FILE:pkp_imgui>\n$<TARGET_FILE:pkp_volk>\n$<TARGET_FILE:pkp_tiling>\n$<TARGET_FILE:pkp_astc2bc7>\n$<TARGET_FILE:pkp_astcenc>\n$<TARGET_FILE:SDL3::SDL3-static>\n")
            string(APPEND _libs "${PS5_PAYLOAD_SDK}/target/lib/libps5platform.a\n")
            set(_whole "${PKP_RADV_ARCHIVE}\n")
            set(_stage_name "boot-gfx")
        else()
            add_library(pkp_title OBJECT "${CMAKE_SOURCE_DIR}/runtime/platform/prospero/title_boot.cc" ${_bp})
            target_link_libraries(pkp_title PRIVATE nxrt_port)
            set(_objs "$<TARGET_OBJECTS:pkp_title>\n$<TARGET_OBJECTS:nxrt_port>\n$<TARGET_OBJECTS:nxrt_nogfx>\n$<TARGET_OBJECTS:pkp_platform>")
            set(_libs "")
            set(_stage_name "boot")
        endif()
        target_include_directories(pkp_title PRIVATE "${CMAKE_SOURCE_DIR}/third_party/pokopia-recomp/runtime/games/pkp")
        target_compile_options(pkp_title PRIVATE -frtti -fexceptions -include
                               "${CMAKE_SOURCE_DIR}/runtime/nxrt_port/compat/port_prelude.h")
        set(_title_mods rtld main sdk subsdk0)
        string(APPEND _libs "${PS5_PAYLOAD_SDK}/target/lib/libc++.a\n${PS5_PAYLOAD_SDK}/target/lib/libc++abi.a\n${PS5_PAYLOAD_SDK}/target/lib/libunwind.a")
    else()
        add_library(pkp_title OBJECT "${CMAKE_SOURCE_DIR}/runtime/platform/prospero/title_main.cc" ${_bp}
                    "${PKP_BOILERPLATE}/tooling/native/app_cpp_runtime.cpp")
        target_compile_options(pkp_title PRIVATE -fno-exceptions -fno-rtti)
        set(_objs "$<TARGET_OBJECTS:pkp_title>\n$<TARGET_OBJECTS:pkprt>\n$<TARGET_OBJECTS:pkp_platform>")
        set(_title_mods ${PKP_TITLE_MODULES})
        set(_libs "")
        set(_whole "")
        set(_stage_name "${PKP_TITLE_STAGE}")
    endif()
    target_include_directories(pkp_title PRIVATE "${CMAKE_SOURCE_DIR}/runtime/include" "${PKP_NXRECOMP_INCLUDE}"
                               "${PKP_BOILERPLATE}/src")
    target_compile_options(pkp_title PRIVATE -O2 -ffunction-sections -fdata-sections)
    foreach(m IN LISTS _title_mods)
        string(APPEND _objs "\n$<TARGET_OBJECTS:pkp_${m}>")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/title_objects.txt" CONTENT "${_objs}\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/title_libs.txt" CONTENT "${_libs}\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/title_stage.txt" CONTENT "${_stage_name}\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/title_whole.txt" CONTENT "${_whole}")
else()
    add_executable(pkp_selftest "${CMAKE_SOURCE_DIR}/runtime/platform/posix/selftest_main.cc")
    target_link_libraries(pkp_selftest PRIVATE pkprt pkp_platform)   # object libraries: each one linked directly
    foreach(m IN LISTS PKP_TITLE_MODULES)
        target_link_libraries(pkp_selftest PRIVATE pkp_${m})
    endforeach()
    target_compile_options(pkp_selftest PRIVATE ${PKP_RUNTIME_FLAGS})
    if(APPLE)
        # keep the guest window (0x71_0000_0000 .. 0x80_0000_0000) clear of the executable and its heap
        target_link_options(pkp_selftest PRIVATE -Wl,-stack_size,0x4000000)
    endif()
endif()
