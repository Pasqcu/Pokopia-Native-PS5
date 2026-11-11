#!/usr/bin/env bash
# Translates the shader blocks a run on the console had no translation for, and sends the translations back.
#
#   scripts/sync_missing_shaders.sh <ps5 address> [port] [--title-id PPSA97656]
#
# With NXRT_NVN_DUMP_MISSING=/app0/cache/shader_missing in /app0/env.txt the title writes every such block there
# (code_<hash>.bin, ctrl_<hash>.bin, manifest.tsv: the layout of the shader corpus). The offline cache
# (scripts/build_shader_cache.sh) only holds the blocks found in the RomFS: the game also builds programs from blocks
# that are not there, and their draws have no shader (black characters and scenery, missing ground). This fetches the
# dump over FTP, runs the same translator on the blocks build/shader_cache/spirv does not have yet, adds the results to
# that cache (the next spirv.pack holds them) and uploads to game/shader_cache/spirv/ the ones the console lacks: the
# title reads a loose <hash>.spv when the pack has none. Run it again after playing further: new places, new blocks.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${1:?usage: sync_missing_shaders.sh <ps5 address> [port] [--title-id PPSA97656]}"
shift
PORT=2121
TITLE_ID=PPSA97656
while [[ $# -gt 0 ]]; do
    case "$1" in
        --title-id) TITLE_ID="$2"; shift 2 ;;
        [0-9]*) PORT="$1"; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
BASE="ftp://$HOST:$PORT/data/homebrew/$TITLE_ID"
WORK="$ROOT/build/shader_missing"
DUMP="$WORK/dump"
STAGE="$WORK/stage"
SPIRV="$ROOT/build/shader_cache/spirv"
HARNESS="$ROOT/third_party/pokopia-recomp/tools/shaders/build/bin/Release/net10.0/spirv_harness.dll"
DOTNET="$ROOT/third_party/dotnet/dotnet"
[[ -x "$DOTNET" ]] || DOTNET="$(command -v dotnet)"
[[ -f "$HARNESS" ]] || { echo "no $HARNESS: run scripts/setup_shader_tools.sh" >&2; exit 2; }
[[ -d "$SPIRV" ]] || { echo "no $SPIRV: run scripts/build_shader_cache.sh" >&2; exit 2; }
mkdir -p "$DUMP"

remote_names() { curl -s -g --connect-timeout 8 "$1" | tr -d '\r' | awk '{print $NF}'; }   # the server answers NLST with nothing

echo "== dump <- $BASE/cache/shader_missing/"
names="$(remote_names "$BASE/cache/shader_missing/" | grep -E '^(code|ctrl)_[0-9a-f]+\.bin$|^manifest\.tsv$' || true)"
[[ -n "$names" ]] || { echo "nothing there (NXRT_NVN_DUMP_MISSING=/app0/cache/shader_missing in env.txt, then a run)"; exit 0; }
while IFS= read -r n; do   # the manifest grows with every run; a block never changes
    if [[ $n == manifest.tsv || ! -s "$DUMP/$n" ]]; then echo "$n"; fi
done <<<"$names" | xargs -P 6 -I{} curl -sS -g --connect-timeout 8 -o "$DUMP/{}" "$BASE/cache/shader_missing/{}"
[[ -s "$DUMP/manifest.tsv" ]] || { echo "no manifest.tsv in the dump" >&2; exit 1; }

# one row per block (the title appends its rows at every run), only the blocks the cache has no translation for
{
    head -1 "$DUMP/manifest.tsv"
    tail -n +2 "$DUMP/manifest.tsv" | sort -u -t$'\t' -k1,1 | while IFS= read -r row; do
        id="${row%%$'\t'*}"
        [[ -f "$SPIRV/${id#run_}.spv" ]] || printf '%s\n' "$row"
    done
} > "$DUMP/unique.tsv"
blocks=$(tail -n +2 "$DUMP/manifest.tsv" | cut -f1 | sort -u | wc -l | tr -d ' ')
todo=$(($(wc -l < "$DUMP/unique.tsv") - 1))
echo "   $blocks blocks dumped, $todo not translated yet"

if [[ $todo -gt 0 ]]; then
    echo "== translating $todo blocks"
    rm -rf "$STAGE"
    DOTNET_CLI_TELEMETRY_OPTOUT=1 "$DOTNET" "$HARNESS" --batch "$DUMP" --out "$STAGE" --jobs "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)" --no-val \
        > "$WORK/harness.log" 2>&1 || { echo "translator failed (see $WORK/harness.log)" >&2; exit 1; }
    tail -n +2 "$STAGE/results.tsv" | cut -f5 | sort | uniq -c | sed 's/^/   status/'
    tail -n +2 "$STAGE/results.tsv" | cut -f1 | while IFS= read -r h; do
        [[ -f "$STAGE/$h.spv" ]] && mv -f "$STAGE/$h.spv" "$SPIRV/$h.spv"
    done
    [[ -f "$SPIRV/results.tsv" ]] || head -1 "$STAGE/results.tsv" > "$SPIRV/results.tsv"
    tail -n +2 "$STAGE/results.tsv" >> "$SPIRV/results.tsv"
fi

echo "== translations -> $BASE/game/shader_cache/spirv/"
have="$(remote_names "$BASE/game/shader_cache/spirv/" | grep -E '^[0-9a-f]+\.spv$' || true)"
sent=0 missing=0
while IFS= read -r id; do
    h="${id#run_}"
    if [[ ! -f "$SPIRV/$h.spv" ]]; then missing=$((missing + 1)); continue; fi   # the translator had no result for it
    grep -qx "$h.spv" <<<"$have" && continue
    curl -sS -g --connect-timeout 8 -T "$SPIRV/$h.spv" "$BASE/game/shader_cache/spirv/$h.spv"
    sent=$((sent + 1))
done < <(tail -n +2 "$DUMP/manifest.tsv" | cut -f1 | sort -u)
echo "   $sent uploaded, $((blocks - sent - missing)) already there, $missing without a translation"
