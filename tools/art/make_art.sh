#!/usr/bin/env bash
# Renders the title's original artwork and converts it to the PS5 shell formats (macOS: Swift + clang++):
#   ppsa/sce_sys/icon0.png            512x512 PNG          home-screen tile
#   ppsa/sce_sys/pic0.dds, pic1.dds   3840x2160 BC7 DX10   selected-app and launch backgrounds
#   ppsa/art/background.png           the editable source of the backgrounds
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$ROOT/build/art"
mkdir -p "$WORK" "$ROOT/ppsa/sce_sys" "$ROOT/ppsa/art"
swift "$ROOT/tools/art/render_art.swift" "$WORK"
clang++ -std=c++17 -O2 "$ROOT/tools/art/bc7_dds.cpp" -o "$WORK/bc7_dds"
"$WORK/bc7_dds" "$WORK/background.rgba" 3840 2160 "$WORK/pic0.dds"
cp "$WORK/icon0.png" "$ROOT/ppsa/sce_sys/icon0.png"
cp "$WORK/pic0.dds" "$ROOT/ppsa/sce_sys/pic0.dds"
cp "$WORK/pic0.dds" "$ROOT/ppsa/sce_sys/pic1.dds"
cp "$WORK/background.png" "$ROOT/ppsa/art/background.png"
ls -la "$ROOT/ppsa/sce_sys"
