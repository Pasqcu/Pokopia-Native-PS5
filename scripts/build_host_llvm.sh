#!/usr/bin/env bash
# Host tools for building the PS5's Vulkan driver (RADV of PS5_Vulkan / PS5_Mesa): Mesa's mesa_clc and vtn_bindgen2 compile
# RADV's OpenCL C kernels (geometry and tessellation run as compute on the PS5's GPU) at build time, and they link the host's
# LLVM, Clang, SPIRV-LLVM-Translator and libclc. Linux distributions package these; for an Intel Mac neither Homebrew nor
# LLVM publish binaries, so this builds them once from the release sources into ~/.cache/pokopia-native/ps5gfx/host-llvm
# (where scripts/build_radv_ps5.sh looks; a folder without spaces, for Mesa's meson and pkg-config files):
#   LLVM + Clang 21.1.8 (X86 only, shared libLLVM / libclang-cpp), SPIRV-LLVM-Translator v21.1.13 (in-tree),
#   libclc for spirv-mesa3d- / spirv64-mesa3d- (Mesa's OpenCL builtins).
# About 1-2 hours on an 8-core laptop, 2-3 GB installed.
#
#   scripts/build_host_llvm.sh [--jobs N]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM_VER=21.1.8
SPV_VER=21.1.13
OUT="${PKP_HOST_LLVM:-${PKP_PS5GFX_DIR:-$HOME/.cache/pokopia-native/ps5gfx}/host-llvm}"
case "$OUT" in *" "*) echo "the install folder $OUT has a space in it: set PKP_HOST_LLVM" >&2; exit 2 ;; esac
WORK="$ROOT/build/host-llvm"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
[[ ${1:-} == --jobs ]] && JOBS="$2"
mkdir -p "$WORK"
cd "$WORK"

fetch() {   # url file sha256
    [[ -f "$2" ]] || curl -sSL --fail -o "$2.part" "$1" && { [[ -f "$2" ]] || mv "$2.part" "$2"; }
    local got
    got="$(shasum -a 256 "$2" | cut -d' ' -f1)"
    [[ -z "$3" || "$got" == "$3" ]] || { echo "$2: sha256 $got, expected $3" >&2; exit 2; }
    echo "$2 $got"
}
fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz" \
      "llvm-project-$LLVM_VER.src.tar.xz" "${LLVM_SHA256:-}"
fetch "https://github.com/KhronosGroup/SPIRV-LLVM-Translator/archive/refs/tags/v$SPV_VER.tar.gz" \
      "SPIRV-LLVM-Translator-$SPV_VER.tar.gz" "${SPV_SHA256:-}"
[[ -d "llvm-project-$LLVM_VER.src" ]] || tar -xf "llvm-project-$LLVM_VER.src.tar.xz"
[[ -d "SPIRV-LLVM-Translator-$SPV_VER" ]] || tar -xf "SPIRV-LLVM-Translator-$SPV_VER.tar.gz"

echo "== LLVM + Clang + SPIRV-LLVM-Translator $LLVM_VER -> $OUT"
cmake -G Ninja -S "llvm-project-$LLVM_VER.src/llvm" -B build-llvm \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_EXTERNAL_PROJECTS=SPIRV-LLVM-Translator \
    -DLLVM_EXTERNAL_SPIRV_LLVM_TRANSLATOR_SOURCE_DIR="$WORK/SPIRV-LLVM-Translator-$SPV_VER" \
    -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON -DCLANG_LINK_CLANG_DYLIB=ON \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_OCAMLDOC=OFF > cmake-llvm.log
nice -n 5 ninja -C build-llvm -j "$JOBS" install > ninja-llvm.log

echo "== libclc (spirv-mesa3d-, spirv64-mesa3d-)"
PATH="$OUT/bin:$PATH" cmake -G Ninja -S "llvm-project-$LLVM_VER.src/libclc" -B build-libclc \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT" -DLLVM_DIR="$OUT/lib/cmake/llvm" \
    -DLIBCLC_TARGETS_TO_BUILD="spirv-mesa3d-;spirv64-mesa3d-" > cmake-libclc.log
PATH="$OUT/bin:$PATH" nice -n 5 ninja -C build-libclc -j "$JOBS" install > ninja-libclc.log
echo "host LLVM ready: $OUT ($("$OUT/bin/llvm-config" --version))"
ls "$OUT/lib/pkgconfig" "$OUT/share/pkgconfig" 2>/dev/null || true
