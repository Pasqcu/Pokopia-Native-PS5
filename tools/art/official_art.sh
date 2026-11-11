#!/usr/bin/env bash
# The title's home-screen art from the game's official artwork, for your own build (macOS: Swift + clang++). Downloads the
# Pokopia logo and key art of pokopia.pokemon.com into build/art/official/ and writes build/art/local/:
#   icon0.png            512x512 PNG          the logo over the key art
#   pic0.dds, pic1.dds   3840x2160 BC7 DX10   the key art
# scripts/make_ppsa.sh takes build/art/local/ over ppsa/sce_sys/. The pictures belong to Nintendo / The Pokémon Company:
# they stay in build/ (ignored by git) and are never committed. The repository's own art is tools/art/make_art.sh.
#
#   tools/art/official_art.sh [--download]   (--download fetches the pictures again)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$ROOT/build/art/official"
OUT="$ROOT/build/art/local"
BASE=https://pokopia.pokemon.com/assets/en-us/pages/index
mkdir -p "$WORK" "$OUT"
for f in logo-pokopia.png hero-img.jpg; do
    if [[ ! -s "$WORK/$f" || "${1:-}" == --download ]]; then
        curl -fsSL --retry 2 -o "$WORK/$f.part" "$BASE/$f"
        mv "$WORK/$f.part" "$WORK/$f"
    fi
done
swift "$ROOT/tools/art/official_art.swift" "$WORK/logo-pokopia.png" "$WORK/hero-img.jpg" "$WORK"
clang++ -std=c++17 -O2 "$ROOT/tools/art/bc7_dds.cpp" -o "$WORK/bc7_dds"
"$WORK/bc7_dds" "$WORK/background.rgba" 3840 2160 "$OUT/pic0.dds"
cp "$OUT/pic0.dds" "$OUT/pic1.dds"
cp "$WORK/icon0.png" "$OUT/icon0.png"
ls -la "$OUT"
