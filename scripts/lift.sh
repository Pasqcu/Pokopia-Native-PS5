#!/usr/bin/env bash
# Stage 1 of the pipeline: lift the game's NSO modules (AArch64) to C++ with nxrecomp.
#
#   scripts/lift.sh [exefs dir] [out dir]
#
# Defaults: the newest update's exefs in the dump folder, build/lift. The bases and module indexes must match the runtime's
# address space (third_party/pokopia-recomp/runtime/include/nxrt/memory.h) and the game config (recomp/games/pkp/config.py).
# Shards: about 45k instructions per translation unit, so the C++ build parallelises evenly.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUMP="$ROOT/Pokemon Pokopia [0400A3A01EE4C000]"
EXEFS="${1:-}"
if [[ -z "$EXEFS" ]]; then
    latest="$(ls -1 "$DUMP/updates" 2>/dev/null | sort -n | tail -1 || true)"
    if [[ -n "$latest" ]]; then EXEFS="$DUMP/updates/$latest/exefs"; else EXEFS="$DUMP/base/exefs"; fi
fi
OUT="${2:-$ROOT/build/lift}"
PY="$ROOT/.venv/bin/python"
RECOMP="$ROOT/third_party/pokopia-recomp/recomp"
JOBS="${NX_JOBS:-8}"   # each lifter process of main peaks near 1 GB

[[ -x "$PY" ]] || { echo "missing $PY: run scripts/setup_host.sh first" >&2; exit 2; }
[[ -d "$RECOMP" ]] || { echo "missing $RECOMP: run scripts/fetch_upstream.sh first" >&2; exit 2; }
[[ -f "$EXEFS/main" ]] || { echo "no main NSO in $EXEFS" >&2; exit 2; }

# name index base shards
MODULES=(
    "rtld    3 0x7700000000   1"
    "subsdk0 2 0x7600000000   4"
    "sdk     1 0x7500000000  45"
    "main    0 0x7100000000 230"
)

echo "exefs: $EXEFS"
echo "out:   $OUT"
mkdir -p "$OUT"
cd "$RECOMP"
for spec in "${MODULES[@]}"; do
    read -r name index base shards <<<"$spec"
    if [[ -n "${NX_ONLY:-}" && " $NX_ONLY " != *" $name "* ]]; then continue; fi
    echo "=== lift $name (index $index, base $base, $shards shards) $(date +%T)"
    start=$SECONDS
    "$PY" -m nxrecomp lift "$EXEFS/$name" "$OUT/$name" --name "$name" --index "$index" --base "$base" \
        --shards "$shards" --jobs "$JOBS" --game pkp
    echo "    $name: $((SECONDS - start)) s, $(du -sh "$OUT/$name" | cut -f1)"
done
echo "=== lift done $(date +%T)"
