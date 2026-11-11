#!/usr/bin/env bash
# Stage 3 of the pipeline: link the PS5 objects into eboot.bin and assemble the title folder dist/<TITLE_ID>/.
#
#   scripts/make_ppsa.sh [--no-game-files] [--title-id PPSA97656] [--name "Pokopia Native"]
#
# Needs: build/ps5 built (cmake --toolchain cmake/toolchains/ps5.cmake, ninja), the boilerplate and SDK in third_party/
# (scripts/fetch_upstream.sh), and an ld.lld (third_party/zig, or LLD on PATH). The folder is deployed as it is to
# /data/homebrew/<TITLE_ID>/ on the console (FTP) and registered by ShadowMountPlus. A folder rather than an .ffpkg /
# .ffpfsc image: every file (eboot.bin, game files, settings) can be replaced over FTP and the logs read back without
# rebuilding an image.
#
# Game files: the user's own exefs NSOs are copied into <TITLE_ID>/game/exefs/ for the title to load. They are never
# part of the repository or of any release; --no-game-files leaves them out (copy them by hand).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TITLE_ID=PPSA97656
TITLE_NAME="Pokopia Native"
GAME_FILES=1
WITH_ROMFS=0
BUILD="$ROOT/build/ps5"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --title-id) TITLE_ID="$2"; shift 2 ;;
        --name) TITLE_NAME="$2"; shift 2 ;;
        --no-game-files) GAME_FILES=0; shift ;;
        --with-romfs) WITH_ROMFS=1; shift ;;          # stage 2: also copy the 6 GB RomFS into game/romfs (else upload it once)
        --build) BUILD="$(cd "$2" && pwd)"; shift 2 ;;  # the CMake build folder of the title (title_objects.txt)
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[[ $TITLE_ID =~ ^PPSA[0-9]{5}$ ]] || { echo "title id must be PPSA + 5 digits" >&2; exit 2; }

# the SDK the build was configured with (CMakeCache), else PS5_PAYLOAD_SDK, else the pinned v0.45 of scripts/setup_host.sh
cached_sdk="$(sed -n 's/^PS5_PAYLOAD_SDK:PATH=//p' "$BUILD/CMakeCache.txt" 2>/dev/null | head -1)"
SDK="${cached_sdk:-${PS5_PAYLOAD_SDK:-$ROOT/third_party/ps5-payload-sdk-v0.45/ps5-payload-sdk}}"
BP="$ROOT/third_party/ps5-native-app-boilerplate"
NATIVE="$BP/tooling/native"
OBJS="$BUILD/title_objects.txt"
LIBS="$BUILD/title_libs.txt"
STAGE="$(cat "$BUILD/title_stage.txt" 2>/dev/null || echo selftest)"
WORK="$ROOT/build/ppsa"
DIST="$ROOT/dist"
APP="$DIST/$TITLE_ID"
MODULE_SDK=0x02000009        # loader constants of the boilerplate's hardware-validated build (firmware 6.02 and 12.70)
COMPANION_SDK=0x08050001
FSELF_MAGIC=0x1D3D154F
LIBC_PRX_SHA256=e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036

if [[ -n "${LLD:-}" ]]; then :;
elif [[ -x "$ROOT/third_party/zig/zig" ]]; then LLD=("$ROOT/third_party/zig/zig" ld.lld);
elif command -v ld.lld >/dev/null; then LLD=(ld.lld);
else echo "ld.lld not found: run scripts/setup_host.sh (fetches Zig) or install LLD" >&2; exit 2; fi
[[ -f "$OBJS" ]] || { echo "missing $OBJS: build the PS5 objects first (README: Build)" >&2; exit 2; }
[[ -f "$NATIVE/native_app_builder.cpp" ]] || { echo "missing $BP: run scripts/fetch_upstream.sh" >&2; exit 2; }
sha256() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1 || sha256sum "$1" | cut -d' ' -f1; }

mkdir -p "$WORK"
echo "== host tools (ps5-native-tool, libc-builder)"
CXX_HOST="${CXX_HOST:-clang++}"
"$CXX_HOST" -std=c++20 -O2 "$NATIVE/native_app_builder.cpp" "$NATIVE/self_container.cpp" "$NATIVE/elf_object.cpp" \
    "$NATIVE/sce_module_writer.cpp" -lz -o "$WORK/ps5-native-tool"
"$CXX_HOST" -std=c++20 -O2 "$NATIVE/libc_builder.cpp" -o "$WORK/libc-builder"

echo "== libc.prx (clean-room runtime shim, reproducible)"
"$WORK/libc-builder" "$NATIVE/runtime/api-surface.txt" "$NATIVE/runtime/imports.txt" "$WORK/libc.raw.elf"
"$WORK/ps5-native-tool" self --sign --in "$WORK/libc.raw.elf" --out "$WORK/libc.prx" >/dev/null
got="$(sha256 "$WORK/libc.prx")"
[[ "$got" == "$LIBC_PRX_SHA256" ]] || { echo "libc.prx digest $got differs from the validated $LIBC_PRX_SHA256" >&2; exit 2; }

objs=()
while IFS= read -r o; do [[ -n "$o" ]] && objs+=("$o"); done < <(tr ';' '\n' < "$OBJS")
libs=()
[[ -f "$LIBS" ]] && while IFS= read -r l; do [[ -n "$l" ]] && libs+=("$l"); done < "$LIBS"
echo "== link stage '$STAGE' (${#objs[@]} objects, ${#libs[@]} static libraries)"
# The boilerplate's validated layout, plus the .eh_frame bounds the SDK's libunwind looks for (a title has no
# dl_iterate_phdr): same sections, same segments.
sed -e 's|KEEP(\*(.eh_frame_hdr))|PROVIDE_HIDDEN(__eh_frame_hdr_start = .); KEEP(*(.eh_frame_hdr)) PROVIDE_HIDDEN(__eh_frame_hdr_end = .);|' \
    -e 's|{ KEEP(\*(.eh_frame)) }|{ PROVIDE_HIDDEN(__eh_frame_start = .); KEEP(*(.eh_frame)) PROVIDE_HIDDEN(__eh_frame_end = .); }|' \
    "$NATIVE/ps5-pie.ld" > "$WORK/title.ld"
# A title exports nothing (the converter refuses exports): every symbol local, the static libc++ / libc++abi included.
printf '{\n    local: *;\n};\n' > "$WORK/title-symbols.map"
grep -q __eh_frame_start "$WORK/title.ld" || { echo "linker script: .eh_frame bounds not inserted" >&2; exit 2; }
# The title's allocations of 1 MiB and more go to direct memory (runtime/platform/prospero/big_heap.cpp): malloc's flexible
# memory is ~448 MiB and the executable itself is charged to it. --wrap redirects only the title's own references.
WRAP=()
WHOLE=()
STUBS=()
[[ -f "$BUILD/title_whole.txt" ]] && while IFS= read -r l; do [[ -n "$l" ]] && WHOLE+=("$l"); done < "$BUILD/title_whole.txt"
if [[ "$STAGE" == boot-gfx ]]; then
    # RADV (PS5_Vulkan) and the SDK fork's platform layer, linked as PS5_Vulkan's tools/radv-link.sh links its titles:
    # every allocation of the title in the platform's heap (direct memory), thread stacks in direct memory, the platform's
    # stand-ins for the libc functions no system module exports (bound to libc's names, kept local: a title exports nothing).
    PLATFORM="$SDK/target/lib/libps5platform.a"
    for f in "${WHOLE[@]}" "$PLATFORM"; do [[ -f "$f" ]] || { echo "missing $f (scripts/build_radv_ps5.sh)" >&2; exit 2; }; done
    WRAP+=(--wrap=pthread_create --wrap=pthread_join --wrap=pthread_detach --wrap=fclose --wrap=fflush)
    for f in malloc calloc realloc free posix_memalign aligned_alloc memalign malloc_usable_size reallocf reallocarray getline getdelim; do
        WRAP+=("--wrap=$f")
    done
    defined="$(nm --defined-only "$PLATFORM" 2>/dev/null | awk '{print $3}' | sort -u)"
    for f in qsort_r mkstemps openlog popen pclose open_memstream __xuname __assert __memset_chk regcomp regexec regfree regerror \
             localtime_r newlocale freelocale strtod_l strtof_l dladdr utimensat localeconv_l strtoll_l strtoull_l strtold_l snprintf_l \
             sscanf_l asprintf_l strcoll_l strxfrm_l strftime_l wcscoll_l wcsxfrm_l btowc_l wctob_l iswctype_l mbrlen_l mbrtowc_l \
             mbsrtowcs_l mbsnrtowcs_l wcrtomb_l wcsnrtombs_l mbtowc_l ___mb_cur_max_l ___runetype_l ___tolower_l ___toupper_l \
             __runes_for_locale catopen catgets catclose backtrace backtrace_symbols_fd __cxa_thread_atexit_impl localeconv \
             arc4random arc4random_buf arc4random_uniform gmtime_r statvfs fstatvfs futimens clock_nanosleep getaddrinfo freeaddrinfo \
             if_nameindex if_freenameindex opendir fdopendir readdir rewinddir dirfd closedir nl_langinfo nl_langinfo_l getpwuid_r \
             posix_fallocate access openat unlinkat fchmodat fstatat mkdirat renameat memfd_create strcasestr mkstemp readlink link symlink \
             mbsnrtowcs wcsnrtombs; do
        grep -qx "ps5_$f" <<<"$defined" && WRAP+=("--defsym=$f=ps5_$f")
    done
    # Imported, where the platform layer of this SDK pin has no stand-in, from a module that gives a title nothing (null at
    # run time): this tree's (runtime/platform/prospero/libc_gaps.cpp).
    for f in readlink pathconf mkstemp fork vfork setsid getcwd; do
        grep -qx "ps5_$f" <<<"$defined" || WRAP+=("--defsym=$f=pkp_libc_$f")
    done
    # libSceAgc and libSceAgcDriver, which RADV's PS5 winsys imports, have no stub in the SDK: PS5_Vulkan's link stubs
    # (declarations only, never packaged) name their functions for the linker and the converter, as its build-radv-title.sh.
    VK="${PKP_PS5_VULKAN:-$(cd "$SDK/../../.." && pwd)}"
    CC_PS5="$(sed -n 's/^CMAKE_C_COMPILER:[A-Z]*=//p' "$BUILD/CMakeCache.txt" | head -1)"
    mkdir -p "$WORK/stubs"
    for lib in libSceAgc:agc_canary_link_stub.c libSceAgcDriver:agc_driver_canary_link_stub.c; do
        name="${lib%%:*}"
        src="$VK/vendor/ps5/sdk/stubs/${lib#*:}"
        [[ -f "$src" ]] || { echo "missing $src (scripts/build_radv_ps5.sh, or set PKP_PS5_VULKAN)" >&2; exit 2; }
        # default visibility, as the SDK's prospero-clang: the PS5 target makes symbols without dllexport hidden
        "${CC_PS5:-clang}" -target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default -std=c11 -O2 -fPIC \
            -c "$src" -o "$WORK/stubs/$name.o"
        "${LLD[@]}" -m elf_x86_64 --shared -soname "$name.prx" -o "$WORK/stubs/$name.so" "$WORK/stubs/$name.o"
        STUBS+=("$WORK/stubs/$name.so")
    done
    # __builtin_cpu_supports (AMD's addrlib in RADV picks its AVX2 swizzle with it) reads compiler-rt's __cpu_model, which a
    # Linux toolchain links from libclang_rt.builtins: here compiler-rt's own x86 CPU model, from the LLVM sources that
    # scripts/build_host_llvm.sh unpacked.
    CPU_MODEL_SRC="${PKP_LLVM_SRC:-$ROOT/build/host-llvm/llvm-project-21.1.8.src}/compiler-rt/lib/builtins/cpu_model/x86.c"
    [[ -f "$CPU_MODEL_SRC" ]] || { echo "missing $CPU_MODEL_SRC (scripts/build_host_llvm.sh, or set PKP_LLVM_SRC)" >&2; exit 2; }
    "${CC_PS5:-clang}" -target x86_64-sie-ps5 -O2 -fPIC -fno-stack-protector -isystem "$SDK/target/include" \
        -c "$CPU_MODEL_SRC" -o "$WORK/stubs/cpu_model.o"
    objs+=("$WORK/stubs/cpu_model.o")
else
    for f in malloc free calloc realloc posix_memalign memalign aligned_alloc malloc_usable_size; do WRAP+=("--wrap=$f"); done
fi
# The SDK's stub libraries in C order, whatever the shell's locale. lld binds a symbol to the first stub that defines it,
# and the converter each import to the first NEEDED module that exports it: in C order libkernel.so (libkernel.sprx) comes
# before libkernel_stub_weak.so, whose SONAME is libkernel_web.sprx. An Italian locale's collation put the latter first:
# every kernel function was imported from libkernel_web, null for a title, and the first console run of the graphics
# title jumped to address 0 at its first open().
SDK_STUBS=()
while IFS= read -r so; do SDK_STUBS+=("$SDK/target/lib/$so"); done < <(cd "$SDK/target/lib" && LC_ALL=C ls -1 | LC_ALL=C sort | grep '[.]so$')
# the flags of the SDK's bin/prospero-lld wrapper (-pie, 16 KiB pages, GNU hash); DWARF dropped (RADV's release build has
# it: debugoptimized), the symbol table kept for crash addresses
"${LLD[@]}" -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu --strip-debug "${WRAP[@]}" \
    -T "$WORK/title.ld" --eh-frame-hdr --version-script "$WORK/title-symbols.map" -e _start \
    --gc-sections --no-dependent-libraries -o "$WORK/llvm-pie.elf" "${objs[@]}" \
    ${STUBS[@]+"${STUBS[@]}"} \
    ${WHOLE[@]+--whole-archive "${WHOLE[@]}" --no-whole-archive} ${libs[@]+--start-group "${libs[@]}" --end-group} \
    --as-needed "${SDK_STUBS[@]}"
STUB_ARGS=()
for st in ${STUBS[@]+"${STUBS[@]}"}; do STUB_ARGS+=(--stub "$st"); done
"$WORK/ps5-native-tool" link --in "$WORK/llvm-pie.elf" --out "$WORK/eboot.elf" --stub-dir "$SDK/target/lib" \
    ${STUB_ARGS[@]+"${STUB_ARGS[@]}"} --module-sdk "$MODULE_SDK" --companion-sdk "$COMPANION_SDK" --file-name eboot.elf

echo "== title folder $APP"
rm -rf -- "$APP"
mkdir -p "$APP/sce_sys" "$APP/sce_module" "$APP/logs" "$APP/game/exefs"
"$WORK/ps5-native-tool" self --sign --in "$WORK/eboot.elf" --out "$APP/eboot.bin" --magic "$FSELF_MAGIC" >/dev/null
cp "$WORK/libc.prx" "$APP/sce_module/libc.prx"
python3 - "$ROOT/ppsa/sce_sys/param.json" "$APP/sce_sys/param.json" "$TITLE_ID" "$TITLE_NAME" <<'PY'
import json, sys
src, dst, tid, name = sys.argv[1:5]
p = json.load(open(src, encoding="utf-8"))
p["titleId"] = tid
p["conceptId"] = tid[4:]
p["contentId"] = f"UP9000-{tid}_00-POKOPIANATIVE001"
p["localizedParameters"][p["localizedParameters"]["defaultLanguage"]]["titleName"] = name
json.dump(p, open(dst, "w", encoding="utf-8"), indent=2, ensure_ascii=False)
PY
# home-screen art: your own in build/art/local/ (tools/art/official_art.sh, never committed) over the repository's;
# the launch background is its own file if there is one, else the selection background
first() { for p in "$@"; do [[ -f "$p" ]] && { echo "$p"; return 0; }; done; return 0; }
LART="$ROOT/build/art/local" RART="$ROOT/ppsa/sce_sys"
icon0="$(first "$LART/icon0.png" "$RART/icon0.png")"
pic0="$(first "$LART/pic0.dds" "$RART/pic0.dds")"
pic1="$(first "$LART/pic1.dds" "$LART/pic0.dds" "$RART/pic1.dds" "$RART/pic0.dds")"
[[ -n "$icon0" && -n "$pic0" ]] || { echo "missing ppsa/sce_sys/icon0.png or pic0.dds: run tools/art/make_art.sh" >&2; exit 2; }
cp "$icon0" "$APP/sce_sys/icon0.png"
cp "$pic0" "$APP/sce_sys/pic0.dds"
cp "$pic1" "$APP/sce_sys/pic1.dds"
for p in "$icon0" "$pic0" "$pic1"; do [[ "$p" == "$LART"/* ]] && echo "   art: build/art/local/${p##*/}"; done
cp "$ROOT/ppsa/README_PS5.txt" "$APP/README_PS5.txt"
echo "logs of the title are written here" > "$APP/logs/.keep"
if [[ $GAME_FILES == 1 ]]; then
    dump="$ROOT/Pokemon Pokopia [0400A3A01EE4C000]"
    latest="$(ls -1 "$dump/updates" 2>/dev/null | sort -n | tail -1 || true)"
    exefs="$dump/${latest:+updates/$latest/}exefs"
    [[ -n "$latest" ]] || exefs="$dump/base/exefs"
    mods=(rtld sdk)
    [[ "$STAGE" == boot* ]] && mods=(rtld main sdk subsdk0)
    for m in "${mods[@]}"; do
        [[ -f "$exefs/$m" ]] && cp "$exefs/$m" "$APP/game/exefs/$m" || echo "warning: $exefs/$m not found" >&2
    done
    if [[ "$STAGE" == boot* ]]; then
        romfs="$dump/${latest:+updates/$latest/}romfs"
        if [[ $WITH_ROMFS == 1 && -d "$romfs" ]]; then
            mkdir -p "$APP/game/romfs" && cp -R "$romfs/." "$APP/game/romfs/"
        else
            echo "note: the RomFS (game/romfs, about 6 GB) is not copied; upload it once to /data/homebrew/$TITLE_ID/game/romfs/"
        fi
    fi
fi
"$WORK/ps5-native-tool" self --inspect --file "$APP/eboot.bin" | head -5
echo
echo "Title folder ready: $APP"
du -sh "$APP" | cut -f1 | sed 's/^/size: /'
