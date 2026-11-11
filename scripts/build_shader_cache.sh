#!/usr/bin/env bash
# The game's shader cache, from your own dump: every GLSLC shader block in the RomFS and in exefs/main, translated to
# SPIR-V for the NVN back end. The same procedure as pokopia-recomp's launcher (launcher/src-tauri/src/shaders.rs):
#   1. tools/shaders/ampere/extract_corpus.py writes the distinct blocks to <out>/corpus (unique_all.tsv);
#   2. the translator (scripts/setup_shader_tools.sh) turns them into <hash>.spv + results.tsv rows, 1000 per chunk.
# Resumable: a block with a row in spirv/results.tsv is never translated again; stop it any time and run it again.
#
#   scripts/build_shader_cache.sh [--out build/shader_cache] [--jobs N] [--limit N]
#
# The cache is derived from the game: it stays on this machine (build/ is git-ignored) and is never published.
# The runtime reads it with [nvn] shader_cache = <out>/spirv.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/shader_cache"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
LIMIT=0
CHUNK=1000
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --limit) LIMIT="$2"; shift 2 ;;   # translate at most N blocks this run (a quick test)
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
SHADERS="$ROOT/third_party/pokopia-recomp/tools/shaders"
HARNESS="$SHADERS/build/bin/Release/net10.0/spirv_harness.dll"
DOTNET="$ROOT/third_party/dotnet/dotnet"
[[ -x "$DOTNET" ]] || DOTNET="$(command -v dotnet)"
PY="$ROOT/.venv/bin/python3"
[[ -x "$PY" ]] || PY="$(command -v python3)"
[[ -f "$HARNESS" ]] || { echo "translator missing: run scripts/setup_shader_tools.sh" >&2; exit 2; }
dump="$ROOT/Pokemon Pokopia [0400A3A01EE4C000]"
latest="$(ls -1 "$dump/updates" 2>/dev/null | sort -n | tail -1 || true)"
GAME="$dump/${latest:+updates/$latest/}"
ROMFS="${GAME}romfs"
MAIN="${GAME}exefs/main"
[[ -d "$ROMFS" && -f "$MAIN" ]] || { echo "the dump's romfs / exefs/main not found under $GAME" >&2; exit 2; }

CORPUS="$OUT/corpus"
STAGE="$OUT/stage"
SPIRV="$OUT/spirv"
mkdir -p "$SPIRV"

# 1. extract (once per RomFS)
if [[ ! -f "$CORPUS/unique_all.tsv" ]]; then
    echo "== extracting the shader blocks ($ROMFS, $MAIN)"
    rm -f "$CORPUS/unique.tsv"
    nice -n 10 "$PY" -I "$SHADERS/ampere/extract_corpus.py" --romfs "$ROMFS" --main "$MAIN" --out "$CORPUS" --jobs "$JOBS" --progress
    mv "$CORPUS/unique.tsv" "$CORPUS/unique_all.tsv"
fi

# 2. translate what spirv/results.tsv does not list yet, chunk by chunk
header="$(head -1 "$CORPUS/unique_all.tsv")"
total=$(($(wc -l < "$CORPUS/unique_all.tsv") - 1))
todo="$OUT/todo.tsv"
if [[ -f "$SPIRV/results.tsv" ]]; then
    awk -F'\t' 'NR==FNR { if (FNR > 1) done[$1] = 1; next } FNR > 1 && !($1 in done)' "$SPIRV/results.tsv" "$CORPUS/unique_all.tsv" > "$todo"
else
    tail -n +2 "$CORPUS/unique_all.tsv" > "$todo"
fi
left=$(wc -l < "$todo" | tr -d ' ')
done_n=$((total - left))
[[ $LIMIT -gt 0 && $left -gt $LIMIT ]] && { head -n "$LIMIT" "$todo" > "$todo.l"; mv "$todo.l" "$todo"; left=$LIMIT; }
echo "== translating: $left of $((total - done_n)) blocks left, $done_n of $total done ($JOBS jobs)"
t0=$SECONDS
while [[ -s "$todo" ]]; do
    { echo "$header"; head -n $CHUNK "$todo"; } > "$CORPUS/unique.tsv"
    rm -rf "$STAGE"
    DOTNET_CLI_TELEMETRY_OPTOUT=1 nice -n 10 "$DOTNET" "$HARNESS" --batch "$CORPUS" --out "$STAGE" --jobs "$JOBS" --no-val > "$OUT/harness.log" 2>&1 \
        || { echo "translator failed (see $OUT/harness.log)" >&2; exit 1; }
    # commit the chunk: .spv files first, then the rows that make them count
    tail -n +2 "$STAGE/results.tsv" | cut -f1 | while read -r h; do
        [[ -f "$STAGE/$h.spv" ]] && mv -f "$STAGE/$h.spv" "$SPIRV/$h.spv"
    done
    [[ -f "$SPIRV/results.tsv" ]] || head -1 "$STAGE/results.tsv" > "$SPIRV/results.tsv"
    tail -n +2 "$STAGE/results.tsv" >> "$SPIRV/results.tsv"
    n=$(($(wc -l < "$CORPUS/unique.tsv") - 1))
    tail -n +$((n + 1)) "$todo" > "$todo.next"; mv "$todo.next" "$todo"
    done_n=$((done_n + n))
    echo "  $done_n of $total ($((SECONDS - t0)) s)"
done
rm -rf "$STAGE" "$CORPUS/unique.tsv" "$todo"
if [[ $LIMIT -eq 0 ]]; then
    rm -rf "$CORPUS"   # only needed to translate (several GB)
    echo "shader cache complete: $SPIRV ($(ls "$SPIRV" | wc -l | tr -d ' ') files)"
else
    echo "partial run (--limit): $SPIRV"
fi
