#!/usr/bin/env bash
# The whole pipeline, from the user's dump to the PS5 title folder:
#   1 setup      Python venv, pinned upstream, PS5 SDK, linker
#   2 lift       AArch64 NSO modules -> C++ (nxrecomp)                       build/lift/
#   3 host test  recompiled sdk on this x86-64 machine                       build/host/pkp_selftest
#   4 PS5        objects for x86_64-sie-ps5 (-march=znver2 -O2)             build/ps5/
#   5 art        icon + 4K BC7 backgrounds (macOS; committed copies otherwise) ppsa/sce_sys/
#   6 title      eboot.bin + PPSA folder                                     dist/PPSA97656/
# The dump folder "Pokemon Pokopia [0400A3A01EE4C000]" (base/exefs, updates/<version>/exefs) must be in the repository
# root. Nothing of it leaves this machine except into dist/, which is never committed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"

bash scripts/setup_host.sh
bash scripts/lift.sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DPKP_MODULES="rtld;sdk"
ninja -C build/host -j"$JOBS"
dump="Pokemon Pokopia [0400A3A01EE4C000]"
latest="$(ls -1 "$dump/updates" 2>/dev/null | sort -n | tail -1 || true)"
./build/host/pkp_selftest "$dump/${latest:+updates/$latest/}exefs" build/host/selftest.log
cmake -S . -B build/ps5 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ps5.cmake \
    -DPKP_MODULES="rtld;sdk"
ninja -C build/ps5 -j"$JOBS"
if [[ "$(uname -s)" == Darwin ]] && command -v swift >/dev/null; then bash tools/art/make_art.sh; fi
bash scripts/make_ppsa.sh
