# The graphics shell of nxrt (pokopia-recomp runtime/src/gfx: SDL3 platform, Vulkan 1.3 device, NVN front end, ImGui
# overlay) built for macOS (MoltenVK) and later the PS5 (PS5_Vulkan), from the unchanged upstream sources:
#   - the pinned third-party sources of upstream runtime/cmake/gfx_deps.cmake (same URLs and SHA-256), cached in build/deps;
#   - tools/tiling (Tegra block-linear untile) as source;
#   - tools/textures (astc2bc7): astc-encoder 5.7.0 linked statically instead of the per-ISA DLLs, and bc7e.ispc compiled with
#     the official ISPC 1.32.0 release for the target (Mach-O here, ELF for the PS5), the pins of tools/textures/THIRD_PARTY.md;
#   - the Windows-only files (DLSS, FidelityFX API, the Win32 display probe) replaced by src/gfx_port_stubs.cpp.
include(FetchContent)
set(PKP_DEPS_DIR "${CMAKE_SOURCE_DIR}/build/deps" CACHE PATH "Downloaded pinned third-party sources")
set(FETCHCONTENT_QUIET ON)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
set(NXRT_TOOLS "${CMAKE_SOURCE_DIR}/third_party/pokopia-recomp/tools")

FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    DOWNLOAD_DIR "${PKP_DEPS_DIR}" URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
FetchContent_Declare(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    DOWNLOAD_DIR "${PKP_DEPS_DIR}" URL_HASH SHA256=f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81 SOURCE_SUBDIR none)
FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    DOWNLOAD_DIR "${PKP_DEPS_DIR}" URL_HASH SHA256=889b90fdcbe9cee7b118339d640c6895b7be5c920d309c44b744692063275b3d SOURCE_SUBDIR none)
FetchContent_Declare(vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.zip
    DOWNLOAD_DIR "${PKP_DEPS_DIR}" URL_HASH SHA256=ce9a61ea5e72284ac9ccc152c214eef381ab57bfd7383f20232e5a652e3afda6 SOURCE_SUBDIR none)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
    DOWNLOAD_DIR "${PKP_DEPS_DIR}" URL_HASH SHA256=e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8 SOURCE_SUBDIR none)
FetchContent_Declare(astcenc
    URL https://github.com/ARM-software/astc-encoder/archive/baff485b0ff36d2f95d28961605106502c653966.tar.gz
    DOWNLOAD_NAME astc-encoder-5.7.0.tar.gz DOWNLOAD_DIR "${PKP_DEPS_DIR}"
    URL_HASH SHA256=eee2fbb77ee1f087dcc3883dceb15a0203d49909e6cbc1d678192857d910b186 SOURCE_SUBDIR none)
FetchContent_Declare(bc7enc_rdo
    URL https://github.com/richgel999/bc7enc_rdo/archive/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0.tar.gz
    DOWNLOAD_NAME bc7enc_rdo-b9438627.tar.gz DOWNLOAD_DIR "${PKP_DEPS_DIR}"
    URL_HASH SHA256=bbb33d1dbb6178a3a2c044956c3cc0b8ea9fc2f2903824379dd86f87d53b64c6 SOURCE_SUBDIR none)
FetchContent_Declare(ispc_bin
    URL https://github.com/ispc/ispc/releases/download/v1.32.0/ispc-v1.32.0-macOS.universal.tar.gz
    DOWNLOAD_DIR "${PKP_DEPS_DIR}" URL_HASH SHA256=9b7bf6cc9fbd8085be267a1d2179fc926e899abf3ded56cd685b4a52f9ee7e98 SOURCE_SUBDIR none)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL_DOCS ON CACHE BOOL "" FORCE)
set(SDL_VENDOR_INFO "pokopia-native" CACHE STRING "" FORCE)
if(PS5)
    # The PS5: SDL's dummy video driver (events, surfaces for screenshots); the picture goes through RADV's VK_KHR_display on
    # VideoOut (gfx/platform_ps5_vulkan.inc), audio through the runtime's own sink. SDL has no driver for the DualSense here
    # (no HIDAPI): gfx/ps5_pad.cpp reads it through libScePad.
    foreach(_off SDL_OFFSCREEN SDL_X11 SDL_WAYLAND SDL_KMSDRM SDL_OPENGL SDL_OPENGLES SDL_VULKAN SDL_RENDER SDL_GPU SDL_AUDIO
                 SDL_CAMERA SDL_DIALOG SDL_HIDAPI SDL_HIDAPI_JOYSTICK SDL_HAPTIC SDL_POWER SDL_LIBUDEV SDL_DBUS SDL_IBUS
                 SDL_LIBICONV SDL_SYSTEM_ICONV SDL_TRAY SDL_PROCESS SDL_LOADSO SDL_RPATH)
        set(${_off} OFF CACHE BOOL "" FORCE)
    endforeach()
    foreach(_on SDL_UNIX_CONSOLE_BUILD SDL_VIDEO SDL_DUMMYVIDEO SDL_JOYSTICK SDL_VIRTUAL_JOYSTICK SDL_SENSOR SDL_PTHREADS SDL_CLOCK_GETTIME)
        set(${_on} ON CACHE BOOL "" FORCE)
    endforeach()
    set(SDL_ASSERTIONS "disabled" CACHE STRING "" FORCE)
    # The toolchain's checks only compile (no executable links for the console), so SDL takes every function the SDK's
    # FreeBSD headers declare as present. These are declared but no system module exports them to a title: SDL's own
    # versions instead, and no USB HID joystick back end (libusbhid).
    foreach(_absent LIBC_HAS_WCSLCPY LIBC_HAS_WCSLCAT LIBC_HAS_WCSNLEN LIBUSBHID HAVE_USBHID)
        set(${_absent} "" CACHE INTERNAL "absent on the PS5 (gfx.cmake)" FORCE)
    endforeach()
endif()
FetchContent_MakeAvailable(sdl3 vulkan_headers volk vma imgui astcenc bc7enc_rdo ispc_bin)

# volk (loader) next to the pinned Vulkan headers
add_library(pkp_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(pkp_volk SYSTEM PUBLIC "${volk_SOURCE_DIR}" "${vulkan_headers_SOURCE_DIR}/include")
target_compile_definitions(pkp_volk PUBLIC VK_NO_PROTOTYPES $<$<PLATFORM_ID:Darwin>:VK_USE_PLATFORM_METAL_EXT>)
set_target_properties(pkp_volk PROPERTIES C_STANDARD 11)

add_library(pkp_vma INTERFACE)
target_include_directories(pkp_vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")

add_library(pkp_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp" "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(pkp_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(pkp_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(pkp_imgui PUBLIC pkp_volk)

# tools/tiling
add_library(pkp_tiling STATIC "${NXRT_TOOLS}/tiling/src/tegra_tiling.cpp")
target_include_directories(pkp_tiling PUBLIC "${NXRT_TOOLS}/tiling/include")

# tools/textures: astcenc decoder (static), bc7e (ISPC), a2b.cpp (transformed: astcenc linked in, no DLLs)
file(GLOB PKP_ASTCENC_SOURCES "${astcenc_SOURCE_DIR}/Source/astcenc_*.cpp")
add_library(pkp_astcenc STATIC ${PKP_ASTCENC_SOURCES})
target_include_directories(pkp_astcenc PUBLIC "${astcenc_SOURCE_DIR}/Source")
# decompressor-only, AVX2 (Zen 2 and the host): the options of upstream's astcdec-avx2 build
target_compile_definitions(pkp_astcenc PRIVATE ASTCENC_DECOMPRESS_ONLY ASTCENC_SSE=41 ASTCENC_AVX=2 ASTCENC_POPCNT=1 ASTCENC_F16C=1 ASTCENC_NEON=0 ASTCENC_SVE=0)
target_compile_options(pkp_astcenc PRIVATE -O3 -mavx2 -mfma -mf16c -mpopcnt -w)

set(PKP_ISPC "${ispc_bin_SOURCE_DIR}/bin/ispc")
set(PKP_BC7E_OUT "${CMAKE_BINARY_DIR}/bc7e")
file(MAKE_DIRECTORY "${PKP_BC7E_OUT}")
if(PS5)
    set(_ispc_os linux)   # ELF x86-64, SysV ABI: what the PS5 links (ISPC has no FreeBSD target; bc7e calls no OS service)
else()
    set(_ispc_os macos)
endif()
add_custom_command(
    OUTPUT "${PKP_BC7E_OUT}/bc7e.o" "${PKP_BC7E_OUT}/bc7e_ispc.h"
    COMMAND "${PKP_ISPC}" -O2 "${bc7enc_rdo_SOURCE_DIR}/bc7e.ispc" -o "${PKP_BC7E_OUT}/bc7e.o" -h "${PKP_BC7E_OUT}/bc7e_ispc.h"
            --target=avx2-i32x8 --arch=x86-64 --target-os=${_ispc_os} --pic --opt=fast-math --opt=disable-assertions --woff
    DEPENDS "${bc7enc_rdo_SOURCE_DIR}/bc7e.ispc"
    COMMENT "ispc bc7e.ispc (${_ispc_os})")
add_custom_target(pkp_bc7e_gen DEPENDS "${PKP_BC7E_OUT}/bc7e.o" "${PKP_BC7E_OUT}/bc7e_ispc.h")

set(PKP_A2B_SRC "${CMAKE_CURRENT_BINARY_DIR}/transformed/a2b.cpp")
file(READ "${NXRT_TOOLS}/textures/src/a2b.cpp" _a2b)
# (plain variables, not a list: the C++ text has semicolons)
set(_from "bool load_isa(const std::wstring& dir, const char* isa, AstcApi& api) {")
set(_to "bool load_isa(const std::wstring&, const char* isa, AstcApi& api) {\n    // port: astcenc is linked in (one AVX2 build), no per-ISA DLLs\n    api.module = reinterpret_cast<HMODULE>(1);\n    api.isa = isa;\n    api.config_init = &astcenc_config_init;\n    api.context_alloc = &astcenc_context_alloc;\n    api.decompress = &astcenc_decompress_image;\n    api.context_free = &astcenc_context_free;\n    api.error_string = &astcenc_get_error_string;\n    return true;\n}\nbool load_isa_win32(const std::wstring& dir, const char* isa, AstcApi& api) {")
string(FIND "${_a2b}" "${_from}" _at)
if(_at EQUAL -1)
    message(FATAL_ERROR "gfx: a2b.cpp transform no longer applies: ${_from}")
endif()
string(REPLACE "${_from}" "${_to}" _a2b "${_a2b}")
file(WRITE "${PKP_A2B_SRC}.tmp" "${_a2b}")
configure_file("${PKP_A2B_SRC}.tmp" "${PKP_A2B_SRC}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NXRT_TOOLS}/textures/src/a2b.cpp")
set_source_files_properties("${PKP_BC7E_OUT}/bc7e.o" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(pkp_astc2bc7 STATIC "${PKP_A2B_SRC}" "${PKP_BC7E_OUT}/bc7e.o")
add_dependencies(pkp_astc2bc7 pkp_bc7e_gen)
target_include_directories(pkp_astc2bc7 PUBLIC "${NXRT_TOOLS}/textures/include" PRIVATE "${PKP_BC7E_OUT}")
target_link_libraries(pkp_astc2bc7 PUBLIC pkp_astcenc PRIVATE nxrt_port)
target_compile_options(pkp_astc2bc7 PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/port_prelude.h" -w ${PKP_ISA_FLAGS} -mxsave)

# nxrt gfx
set(NXRT_GFX_DIR "${NXRT_DIR}/src/gfx")
file(GLOB_RECURSE NXRT_GFX_SOURCES CONFIGURE_DEPENDS "${NXRT_GFX_DIR}/*.cpp")
list(FILTER NXRT_GFX_SOURCES EXCLUDE REGEX "nvn_gpu_stub[.]cpp$")                       # upstream's unit-test stand-in
list(FILTER NXRT_GFX_SOURCES EXCLUDE REGEX "/extras/(temporal_dlss|temporal_ffx|win_display)[.]cpp$")   # Windows-only
# Source transforms of the gfx files (as port_transform in CMakeLists.txt): the copy in the build folder is compiled instead.
#   SEH (__try / __except) guards a few reads of guest texture memory that the game may have unmapped; the port has no SEH:
#   the reads run unguarded for now (a fault is a crash report instead of a skipped texture).
set(PKP_GFX_TRANSFORMED "")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(gfx_transform rel)
    # a copy without raw NUL bytes first (CMake's strings end at one)
    get_filename_component(_tdir "${CMAKE_CURRENT_BINARY_DIR}/transformed/gfx/${rel}" DIRECTORY)
    file(MAKE_DIRECTORY "${_tdir}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/nul_to_escape.py"
                            "${NXRT_GFX_DIR}/${rel}" "${CMAKE_CURRENT_BINARY_DIR}/transformed/gfx/${rel}.src" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "gfx: cannot read ${rel}")
    endif()
    file(READ "${CMAKE_CURRENT_BINARY_DIR}/transformed/gfx/${rel}.src" body)
    math(EXPR last "${ARGC} - 1")
    foreach(i RANGE 1 ${last} 2)
        math(EXPR j "${i} + 1")
        set(from "${ARGV${i}}")
        set(to "${ARGV${j}}")
        string(FIND "${body}" "${from}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "gfx: transform of ${rel} no longer applies (text not found: ${from})")
        endif()
        string(REPLACE "${from}" "${to}" body "${body}")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/transformed/gfx/${rel}.tmp" "${body}")
    configure_file("${CMAKE_CURRENT_BINARY_DIR}/transformed/gfx/${rel}.tmp" "${CMAKE_CURRENT_BINARY_DIR}/transformed/gfx/${rel}" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NXRT_GFX_DIR}/${rel}")
    set(_src ${NXRT_GFX_SOURCES})
    list(REMOVE_ITEM _src "${NXRT_GFX_DIR}/${rel}")
    set(NXRT_GFX_SOURCES ${_src} PARENT_SCOPE)
    set(PKP_GFX_TRANSFORMED ${PKP_GFX_TRANSFORMED} "${CMAKE_CURRENT_BINARY_DIR}/transformed/gfx/${rel}" PARENT_SCOPE)
endfunction()
foreach(_seh nvn/nvn_texprep.cpp nvn/nvn_tex_edit.cpp)
    gfx_transform(${_seh} "__try {" "{   // port: no SEH (gfx.cmake)" "} __except (EXCEPTION_EXECUTE_HANDLER) {" "} if (false) {")
endforeach()

#   vk_device.cpp (Apple): MoltenVK is a portability driver, listed only when the instance asks (portability enumeration),
#   and its device extension VK_KHR_portability_subset must be enabled when offered.
if(APPLE)
    gfx_transform(vk_device.cpp
        "    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};"
        "    // port (MoltenVK): a portability driver is enumerated only when the instance asks for it\n    const bool x_portability = has_iext(\"VK_KHR_portability_enumeration\");\n    if (x_portability) inst_exts.push_back(\"VK_KHR_portability_enumeration\");\n    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};"
        "    ici.pApplicationInfo = &app;"
        "    ici.pApplicationInfo = &app;\n    if (x_portability) ici.flags |= 0x00000001;   // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR"
        "    if (!headless) dev_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);"
        "    if (has_dext(\"VK_KHR_portability_subset\")) dev_exts.push_back(\"VK_KHR_portability_subset\");   // port (MoltenVK)\n    if (!headless) dev_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);")
endif()

#   nvn/nvn_gpu_texture.cpp: storage on an sRGB texture through EXTENDED_USAGE is kept only when the driver takes the sRGB image with it
#   too, not just its UNORM partner (MoltenVK refuses R8G8B8A8_SRGB so: the game's window textures then had no image at all).
#   A view that names the texture's own NVN format reads an image stored in that format's fallback through the image's format
#   (the PS5's GPU has no D24_UNORM_S8_UINT: the depth buffer is a D32_SFLOAT_S8_UINT image, and the game's DEPTH24_STENCIL8
#   views of it were refused, "view format 129 is not compatible with the image's 130": the shaders sampled the dummy texture).
gfx_transform(nvn/nvn_gpu_texture.cpp
    "            device().image_supported(make_desc(s, unorm, usage | VK_IMAGE_USAGE_STORAGE_BIT, flags))) {"
    "            device().image_supported(make_desc(s, unorm, usage | VK_IMAGE_USAGE_STORAGE_BIT, flags)) &&\n            device().image_supported(make_desc(s, chosen, usage | VK_IMAGE_USAGE_STORAGE_BIT, flags | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT))) {   // port"
    "VkFormat promoted_view_format(const TexGpu& g, VkFormat f) {"
    "VkFormat promoted_view_format(const TexGpu& g, VkFormat f) {\n    if (g.fmt && g.fmt->vk_fallback != VK_FORMAT_UNDEFINED && g.format == g.fmt->vk_fallback && f == g.fmt->vk) return g.format;   // port: fallback storage")

#   nvn/nvn_gpu_pool.cpp: the guest pages of a pool chunk are backed before the chunk is imported into the GPU (the PS5's
#   paged VM backs the heap lazily, on a CPU fault: a GPU access to a page no CPU touched yet would find nothing there).
gfx_transform(nvn/nvn_gpu_pool.cpp
    "    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};\n    VkResult r;"
    "    pkp::plat::prefault(import_ptr, size);   // port: back lazily backed guest pages before the GPU sees them\n    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};\n    VkResult r;"
    "#include \"nxrt/config.h\""
    "#include \"nxrt/config.h\"\n#include \"pkp/platform.h\"")
#   nvn/nvn_shaders.cpp: every cache file is read through read_file(), which asks the shader pack first (gfx/shader_pack.cpp:
#   one spirv.pack instead of ~99,000 files, scripts/pack_shader_cache.py); without a pack the files are read as before.
gfx_transform(nvn/nvn_shaders.cpp
    "bool read_file(const std::string& path, std::string& out) {\n    std::ifstream f(path, std::ios::binary);"
    "extern \"C\" bool pkp_shader_pack_read(const char* path, void (*sink)(void* ctx, const char* data, size_t n), void* ctx);\nbool read_file(const std::string& path, std::string& out) {\n    if (pkp_shader_pack_read(path.c_str(), [](void* c, const char* d, size_t n) { static_cast<std::string*>(c)->assign(d, n); }, &out)) return true;   // port\n    std::ifstream f(path, std::ios::binary);")
#   nvn/nvn_replay.cpp: the pictures of a traced frame (trace_t<id>_<format>.png, snap_d<draw>_t<id>_...png) take
#   [nvn] trace_dir as a prefix (NXRT_NVN_TRACE_DIR=/app0/trace/): upstream names them relative to the working directory,
#   and on the PS5 nothing of a traced frame appeared in the title's folder.
gfx_transform(nvn/nvn_replay.cpp
    "        const std::string png = std::string(\"trace_t\") + std::to_string(t->id) + \"_\" + (t->fmt ? t->fmt->name : \"x\") + \".png\";"
    "        const std::string png = nxrt::config_get_str(\"nvn\", \"trace_dir\", \"\") + \"trace_t\" + std::to_string(t->id) + \"_\" + (t->fmt ? t->fmt->name : \"x\") + \".png\";   // port"
    "std::string(\"snap_d\") + std::to_string(g_trace_n) + \"_t\" + std::to_string(t->id)"
    "nxrt::config_get_str(\"nvn\", \"trace_dir\", \"\") + \"snap_d\" + std::to_string(g_trace_n) + \"_t\" + std::to_string(t->id)")
#   nvn/nvn_pipeline.cpp: every vertex-stage Location output starts at (0,0,0,1) before the shader runs (gfx/spirv_init_outputs.cpp,
#   from featureJosh/pokopia-recomp pull request 14): RADV leaves an output component the shader never writes NaN, which blacked out
#   the clouds and the sky and gave everything a white halo through the bloom. [nvn] init_outputs = false turns it off.
gfx_transform(nvn/nvn_pipeline.cpp
    "#include \"nvn_spirv_patch.h\""
    "#include \"nvn_spirv_patch.h\"\nnamespace nxrt::nvn::be::gpu { bool spirv_init_outputs(const std::vector<uint32_t>& in, std::vector<uint32_t>& out); }   // port: gfx/spirv_init_outputs.cpp"
    "    // [nvn] spirv_salt (measurement only)"
    "    static const bool init_outputs = config_get_bool(\"nvn\", \"init_outputs\", true);   // port: gfx/spirv_init_outputs.cpp\n    std::vector<uint32_t> initialised;\n    if (init_outputs && spirv_init_outputs(*code, initialised)) code = &initialised;\n    // [nvn] spirv_salt (measurement only)")
if(PS5)
    #   platform.cpp (PS5): the window is SDL's dummy one; the Vulkan surface is a VideoOut display plane, and the pixel size the
    #   display mode's (gfx/platform_ps5_vulkan.inc); SDL's versions stay, renamed *_sdl.
    gfx_transform(platform.cpp
        "    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;"
        "    SDL_WindowFlags flags = 0;   // port (PS5): SDL's dummy video driver"
        "void pixel_size(int* w, int* h) { current_pixel_size(w, h); }"
        "void pixel_size_sdl(int* w, int* h) { current_pixel_size(w, h); }"
        "const char* const* vulkan_instance_extensions(uint32_t* count) {"
        "#include \"${CMAKE_CURRENT_SOURCE_DIR}/gfx/platform_ps5_vulkan.inc\"\nconst char* const* vulkan_instance_extensions_sdl(uint32_t* count) {"
        "bool create_vulkan_surface(void* vk_instance, uint64_t* surface_out) {"
        "bool create_vulkan_surface_sdl(void* vk_instance, uint64_t* surface_out) {"
        "void destroy_vulkan_surface(void* vk_instance, uint64_t surface) {"
        "void destroy_vulkan_surface_sdl(void* vk_instance, uint64_t surface) {")
    #   vk_device.cpp (PS5): no Vulkan loader on the console; RADV is linked in, its vk_icdGetInstanceProcAddr feeds volk.
    gfx_transform(vk_device.cpp
        "    else if (volkInitialize() != VK_SUCCESS) return fail(\"no Vulkan loader found (vulkan-1.dll)\");"
        "    else volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk_icdGetInstanceProcAddr));   // port (PS5): RADV linked in"
        "namespace nxrt::gfx {"
        "extern \"C\" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);   // RADV (PS5_Mesa)\nnamespace nxrt::gfx {")
endif()

add_library(nxrt_gfx STATIC ${NXRT_GFX_SOURCES} ${PKP_GFX_TRANSFORMED} "${CMAKE_CURRENT_SOURCE_DIR}/gfx/gfx_port_stubs.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/gfx/gfx_boot.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/gfx/shader_pack.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/gfx/boot_status.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/gfx/spirv_init_outputs.cpp")
if(PS5)
    target_sources(nxrt_gfx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gfx/ps5_pad.cpp")   # the DualSense (libScePad)
endif()
target_include_directories(nxrt_gfx PUBLIC "${NXRT_DIR}/include" "${NXRT_GFX_DIR}/nvn/third_party"
                           PRIVATE "${NXRT_GFX_DIR}" "${NXRT_GFX_DIR}/nvn" "${NXRT_DIR}/src")
target_link_libraries(nxrt_gfx PUBLIC nxrt_port pkp_volk pkp_vma pkp_imgui pkp_tiling pkp_astc2bc7 SDL3::SDL3-static)
target_compile_definitions(nxrt_gfx PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
target_compile_options(nxrt_gfx PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/port_prelude.h" -w
                       ${PKP_ISA_FLAGS} -O2 -fno-strict-aliasing -ffp-contract=off -frtti -fexceptions)
target_compile_features(nxrt_gfx PUBLIC cxx_std_20)

if(TARGET pkp_boot)
    target_link_libraries(pkp_boot PRIVATE nxrt_gfx)
    target_compile_definitions(pkp_boot PRIVATE PKP_HAVE_GFX=1)
endif()
