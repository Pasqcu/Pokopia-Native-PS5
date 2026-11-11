#!/usr/bin/env bash
# The whole pipeline, from your own dump to the PS5 title folder dist/PPSA97656/. Step by step, with what each step needs and
# how long it takes: docs/BUILD_GUIDE.md (English), docs/GUIDA_COMPILAZIONE.md (italiano).
#   1 setup       Python venv, pinned third-party components, PS5 SDK, linker              scripts/setup_host.sh
#   2 host LLVM   LLVM / Clang / libclc for the build tools of the PS5's Vulkan driver      scripts/build_host_llvm.sh
#   3 PS5 Vulkan  RADV of PS5_Vulkan and the SDK fork with libps5platform                   scripts/build_radv_ps5.sh
#   4 lift        the game's AArch64 modules -> C++                                         scripts/lift.sh -> build/lift/
#   5 shaders     the translator, the shader cache of your dump, one pack file for the PS5  build/shader_cache/spirv.pack
#   6 title       PS5 objects (-march=znver2 -O2), eboot.bin, the PPSA folder               build/ps5-gfx/, dist/PPSA97656/
# A step whose result is already there is skipped (PKP_FORCE=1 runs steps 2-5 again, after a new game version for
# example); the C++ build is incremental. JOBS sets the parallel C++ compiles (default 8: the game's largest files take
# up to 1.2 GB of RAM each, so 8 fit in 16 GB).
# The dump folder "Pokemon Pokopia [0400A3A01EE4C000]" must be in the repository root, extracted into folders (README).
# Nothing of it leaves this machine except into dist/, which is never committed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
JOBS="${JOBS:-8}"
FORCE="${PKP_FORCE:-0}"
R="${PKP_PS5GFX_DIR:-$HOME/.cache/pokopia-native/ps5gfx}"     # where scripts/build_host_llvm.sh and build_radv_ps5.sh work
HOST_LLVM="${PKP_HOST_LLVM:-$R/host-llvm}"
SDK="$R/PS5_Vulkan/.deps/native/ps5-payload-sdk"
RADV="$R/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
DUMP="Pokemon Pokopia [0400A3A01EE4C000]"
[[ -d "$DUMP" ]] || { echo "no \"$DUMP\" folder in $ROOT: prepare your dump first (README)" >&2; exit 2; }
step() { echo; echo "=== $*"; }
have() { [[ $FORCE != 1 ]] && for f in "$@"; do [[ -e "$f" ]] || return 1; done; }

step "1/6 setup"
bash scripts/setup_host.sh

step "2/6 host LLVM (once, 1-2 hours)"
if have "$HOST_LLVM/bin/llvm-config"; then echo "already built: $HOST_LLVM"; else bash scripts/build_host_llvm.sh; fi

step "3/6 PS5 Vulkan driver (once)"
if have "$RADV" "$SDK/target/lib/libps5platform.a"; then echo "already built: $RADV"; else bash scripts/build_radv_ps5.sh; fi

step "4/6 lift"
if have build/lift/main build/lift/sdk build/lift/rtld build/lift/subsdk0; then echo "already lifted: build/lift"; else bash scripts/lift.sh; fi

step "5/6 shaders (about 2 hours the first time, resumable)"
if have build/shader_cache/spirv.pack; then echo "already packed: build/shader_cache/spirv.pack"; else
    bash scripts/setup_shader_tools.sh
    bash scripts/build_shader_cache.sh
    .venv/bin/python3 scripts/pack_shader_cache.py
fi

step "6/6 PS5 title"
cmake -S . -B build/ps5-gfx -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ps5.cmake \
    -DPKP_MODULES="rtld;subsdk0;sdk;main" -DPKP_TITLE_STAGE=boot -DPKP_NXRT_GFX=ON \
    -DPS5_PAYLOAD_SDK="$SDK" -DPKP_RADV_ARCHIVE="$RADV"
ninja -C build/ps5-gfx -j"$JOBS"
bash scripts/make_ppsa.sh --build build/ps5-gfx

echo
echo "Done: dist/PPSA97656/ (eboot.bin and your dump's exefs). Install it with:"
echo "  scripts/deploy_ftp.sh <ps5 address> 2121 --romfs --shader-pack"
