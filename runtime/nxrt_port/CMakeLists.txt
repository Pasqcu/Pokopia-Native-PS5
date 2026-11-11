# The HLE runtime of pokopia-recomp (nxrt, Windows) built for POSIX hosts and the PS5. The upstream sources are used as they
# are: compat/ provides the Win32 subset they call (src/win32_shim.cpp, src/win32_files.cpp), and the few files that are
# Windows-only in substance are replaced (src/diag_port.cpp, src/port_stubs.cpp):
#   diag/crash, sampler, stackwalk, watchdog, symbols   SEH, thread suspension, dbghelp
#   kernel/starvation_guard                             a Windows scheduler work-around
#   mem/write_watch                                     GetWriteWatch-style GPU dirty tracking (inert until the GPU stage)
#   services/audio_xaudio2, audio_devices               XAudio2 / MMDevice (the paced sink plays; sceAudioOut on the PS5 later)
#   services/album, swkbd_prompt                        WIC screenshots, a Win32 dialog
#   media/movie_mf, testtools/tt_game                   Media Foundation (Switch 1 movies), SEH memory probes
set(NXRT_DIR "${CMAKE_SOURCE_DIR}/third_party/pokopia-recomp/runtime")
if(NOT EXISTS "${NXRT_DIR}/src/core/runtime.cpp")
    message(FATAL_ERROR "nxrt not found at ${NXRT_DIR}: run scripts/fetch_upstream.sh")
endif()

file(GLOB_RECURSE NXRT_SOURCES CONFIGURE_DEPENDS "${NXRT_DIR}/src/*.cpp")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/gfx/")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/media/movie_mf\\.cpp$")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/services/(album|audio_xaudio2|audio_devices|swkbd_prompt)\\.cpp$")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/kernel/starvation_guard\\.cpp$")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/mem/write_watch\\.cpp$")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/diag/(crash|sampler|stackwalk|watchdog|symbols)\\.cpp$")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/testtools/tt_game\\.cpp$")
file(GLOB PORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# Source transforms (the upstream tree stays untouched: CMake compiles an edited copy from the build folder, and stops if the
# text it edits is not found, e.g. after an upstream update). Each one: file, exact text, replacement, reason.
#   fs_romfs.cpp: long_path() rewrites host paths into Windows form (backslashes, \\?\ prefix) before std::filesystem and
#   CreateFileW see them; on POSIX a path is used as it is (the Windows version stays in the copy, unused).
function(port_transform rel)   # rel, then pairs of (exact text, replacement)
    file(READ "${NXRT_DIR}/src/${rel}" body)
    # ARGV<n> keep each argument whole (a list would split the C++ text at its semicolons)
    math(EXPR last "${ARGC} - 1")
    foreach(i RANGE 1 ${last} 2)
        math(EXPR j "${i} + 1")
        set(from "${ARGV${i}}")
        set(to "${ARGV${j}}")
        string(FIND "${body}" "${from}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "nxrt_port: transform of ${rel} no longer applies (text not found: ${from})")
        endif()
        string(REPLACE "${from}" "${to}" body "${body}")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/transformed/${rel}.tmp" "${body}")
    configure_file("${CMAKE_CURRENT_BINARY_DIR}/transformed/${rel}.tmp" "${CMAKE_CURRENT_BINARY_DIR}/transformed/${rel}" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NXRT_DIR}/src/${rel}")
    set(_src ${NXRT_SOURCES})
    list(REMOVE_ITEM _src "${NXRT_DIR}/src/${rel}")
    set(NXRT_SOURCES ${_src} PARENT_SCOPE)
    set(PORT_SOURCES ${PORT_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/transformed/${rel}" PARENT_SCOPE)
endfunction()
#   fs_romfs.cpp, 2: scan() walks folders with POSIX calls (src/fs_romfs_scan.inc, src/port_fs.cpp): std::filesystem's
#   iterator cannot list the PS5 title's /app0 folders; upstream's scan() stays in the copy as scan_std(), unused.
port_transform(services/fs_romfs.cpp
    "std::wstring long_path(std::wstring p) {"
    "std::wstring long_path(std::wstring p) { return p; }\nstd::wstring long_path_win32(std::wstring p) {"
    "bool scan(Node& dir, std::string& msg) {"
    "#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/fs_romfs_scan.inc\"\nbool scan_std(Node& dir, std::string& msg) {")
#   fs_volume.cpp: a commit lists the entries of the work folder relative to it. std::filesystem::relative() resolves both
#   paths first, and on the PS5 it answered "." for every entry: the marker on the console held "M ." and "F ." lines only,
#   the commit stopped at "moving . failed" and no save was ever committed. The iterator's paths begin with the work
#   folder: their lexical difference is the relative path.
port_transform(services/fs_volume.cpp
    "        const std::string rel = to_u8(stdfs::relative(it->path(), s.work, ec));"
    "        const std::string rel = to_u8(it->path().lexically_relative(s.work));   // port: nothing to resolve")
#   am.cpp: the software keyboard's answer is UTF-16 unless the config's use_utf8 byte (0x3BD) says UTF-8. Pokopia's config
#   (library version 0x140010, 1328 bytes) has that byte clear, and the game showed "P" for the answer "Player": the first
#   byte of UTF-16 read as a C string. [applet] swkbd_utf8 (NXRT_APPLET_SWKBD_UTF8=1) answers in UTF-8 whatever the byte
#   says, and the config's bytes from 0x3A8 go to the log, to find where this version keeps the flag.
port_transform(services/am.cpp
    "        a.utf8 = read_at<uint8_t>(*cfg, kCfgUseUtf8, 0) != 0;"
    "        a.utf8 = read_at<uint8_t>(*cfg, kCfgUseUtf8, 0) != 0 || nxrt::config_get_bool(\"applet\", \"swkbd_utf8\", false);   // port\n        for (size_t i = 0x3A8; i < cfg->size(); i += 128) {   // port: this version's bytes around and after the flags\n            std::string hx;\n            for (size_t j = i; j < i + 128 && j < cfg->size(); ++j) { char b[4]; std::snprintf(b, sizeof b, \"%02x\", (*cfg)[j]); hx += b; }\n            NXLOG_I(\"am\", \"swkbd: config bytes from 0x%zx: %s\", i, hx.c_str());\n        }")
# the game's runtime module (title facts, probes); the PC play-mode hooks (2.0.1 addresses) stay out
set(GAME_SOURCES "${NXRT_DIR}/games/pkp/pkp_core.cpp" "${NXRT_DIR}/games/pkp/pkp_data.cpp" "${NXRT_DIR}/games/pkp/pkp_tt_probe.cpp")

# PS5: objects handed to ld.lld directly (the build machine's archiver may not handle ELF); host: a static library
if(PS5)
    add_library(nxrt_port OBJECT ${NXRT_SOURCES} ${PORT_SOURCES} ${GAME_SOURCES})
else()
    add_library(nxrt_port STATIC ${NXRT_SOURCES} ${PORT_SOURCES} ${GAME_SOURCES})
endif()
target_include_directories(nxrt_port PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat" "${NXRT_DIR}/include" "${PKP_NXRECOMP_INCLUDE}"
                           PRIVATE "${NXRT_DIR}/src" "${NXRT_DIR}/src/testtools" "${NXRT_DIR}/src/services" "${NXRT_DIR}/games/pkp")
target_compile_definitions(nxrt_port PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
# nxrt uses dynamic_cast (handle_as) and C++ exceptions (IPC, the unwinder): the x86_64-sie-ps5 defaults turn both off
target_compile_options(nxrt_port PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/port_prelude.h" -w
                       ${PKP_ISA_FLAGS} -O2 -fno-strict-aliasing -ffp-contract=off -mcx16 -frtti -fexceptions)
target_compile_features(nxrt_port PUBLIC cxx_std_20)
target_link_libraries(nxrt_port PUBLIC pkp_platform)

# The boot (src/pkp_boot.cpp, pkp_boot, the stage-2 title) needs the lifted tables of all four modules.
set(_all_modules TRUE)
foreach(m rtld main sdk subsdk0)
    if(NOT TARGET pkp_${m})
        set(_all_modules FALSE)
    endif()
endforeach()
if(NOT _all_modules OR (PS5 AND NOT PKP_TITLE_STAGE STREQUAL "boot"))
    set_target_properties(nxrt_port PROPERTIES EXCLUDE_FROM_ALL ON)   # nothing links it (stage-1 title, self-test only)
endif()

# The graphics shell's stand-ins for builds without it (linked by whoever has no nxrt_gfx)
add_library(nxrt_nogfx OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/nogfx/nogfx_stubs.cpp")
target_link_libraries(nxrt_nogfx PRIVATE nxrt_port)
target_compile_options(nxrt_nogfx PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/port_prelude.h" -w ${PKP_ISA_FLAGS})

if(NOT PS5 AND _all_modules)
    # Host boot of the whole game on the HLE runtime: pkp_boot <exefs dir> [romfs dir] [--seconds N]
    add_executable(pkp_boot "${CMAKE_CURRENT_SOURCE_DIR}/boot_main.cc")
    target_link_libraries(pkp_boot PRIVATE nxrt_port pkp_rtld pkp_main pkp_sdk pkp_subsdk0)
    target_include_directories(pkp_boot PRIVATE "${NXRT_DIR}/games/pkp")
    target_compile_options(pkp_boot PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/port_prelude.h")
    if(APPLE)
        target_link_options(pkp_boot PRIVATE -Wl,-stack_size,0x4000000)   # guest window clear of the main stack
    endif()
elseif(NOT PS5)
    message(STATUS "pkp_boot (the game on the HLE runtime) needs PKP_MODULES=rtld;sdk;subsdk0;main: not built")
endif()
if(TARGET pkp_boot AND NOT PKP_NXRT_GFX)
    target_link_libraries(pkp_boot PRIVATE nxrt_nogfx)
endif()

# The graphics shell (gfx.cmake): host builds for now (MoltenVK on macOS); the PS5 comes with PS5_Vulkan.
option(PKP_NXRT_GFX "build nxrt's graphics shell (SDL3, Vulkan, NVN front end)" OFF)
if(PKP_NXRT_GFX AND _all_modules)
    include("${CMAKE_CURRENT_SOURCE_DIR}/gfx.cmake")
endif()
