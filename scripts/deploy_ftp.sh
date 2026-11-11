#!/usr/bin/env bash
# Uploads the title folder to the console's FTP server (etaHEN / zftpd), into /data/homebrew/<TITLE_ID>/.
#
#   scripts/deploy_ftp.sh <ps5 address> [port] [--romfs] [--shader-pack] [--no-title] [--title-id PPSA97656]
#
# --romfs also uploads the game's RomFS from the dump (about 6.6 GB) to game/romfs/ for the stage-2 title.
# --shader-pack uploads the packed shader cache (scripts/pack_shader_cache.py: build/shader_cache/spirv.pack) to
# game/shader_cache/spirv/spirv.pack for the graphics title, resuming a partial upload.
# The title folder is sent whole every time (eboot.bin can change without changing size, and a resumed upload would append
# the new file to the old one). RomFS files never change: those already there with the same size are skipped, a partial
# one is resumed. Then restart ShadowMountPlus if the title is new or its sce_sys art changed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${1:?usage: deploy_ftp.sh <ps5 address> [port] [--romfs]}"
shift
PORT=2121
ROMFS=0
PACK=0
TITLE=1
TITLE_ID=PPSA97656
while [[ $# -gt 0 ]]; do
    case "$1" in
        --romfs) ROMFS=1; shift ;;
        --shader-pack) PACK=1; shift ;;
        --no-title) TITLE=0; shift ;;               # only the RomFS (the title folder is already there)
        --title-id) TITLE_ID="$2"; shift 2 ;;
        [0-9]*) PORT="$1"; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
APP="$ROOT/dist/$TITLE_ID"
[[ -d "$APP" ]] || { echo "no $APP: run scripts/make_ppsa.sh first" >&2; exit 2; }
BASE="ftp://$HOST:$PORT/data/homebrew/$TITLE_ID"

remote_size() {   # size of a remote file, empty when missing (or its folder is: curl fails, which is not an error here)
    { curl -s -g --connect-timeout 5 -I "$1" 2>/dev/null || true; } | awk 'tolower($1)=="content-length:"{print $2}' | tr -d '\r'
}

upload_tree() {   # local dir, remote url, mode: replace (every file sent whole) | resume (immutable files: skip or resume)
    local dir="$1" url="$2" mode="$3" n=0 skipped=0 t0=$SECONDS
    while IFS= read -r -d '' f; do
        local rel="${f#"$dir"/}" size remote=""
        size=$(stat -f %z "$f" 2>/dev/null || stat -c %s "$f")
        if [[ $mode == resume ]]; then
            remote="$(remote_size "$url/$rel")"
            if [[ "$remote" == "$size" ]]; then skipped=$((skipped + 1)); continue; fi
            [[ -n "$remote" && "$remote" -gt "$size" ]] && remote=""   # not a prefix of this file: send it whole
        fi
        echo "  $(date +%T) $rel ($((size / 1048576)) MiB${remote:+, resuming at $((remote / 1048576)) MiB})"
        if [[ -n "$remote" ]]; then
            curl -sS -g --ftp-create-dirs -C - -T "$f" "$url/$rel"
        else
            curl -sS -g --ftp-create-dirs -T "$f" "$url/$rel"
        fi
        n=$((n + 1))
    done < <(find "$dir" -type f ! -name '.DS_Store' -print0 | sort -z)
    echo "  $url: $n uploaded, $skipped unchanged ($((SECONDS - t0)) s)"
}

if [[ $TITLE == 1 ]]; then
    echo "== title folder -> $BASE"
    upload_tree "$APP" "$BASE" replace
fi
if [[ $ROMFS == 1 ]]; then
    dump="$ROOT/Pokemon Pokopia [0400A3A01EE4C000]"
    latest="$(ls -1 "$dump/updates" 2>/dev/null | sort -n | tail -1 || true)"
    romfs="$dump/${latest:+updates/$latest/}romfs"
    [[ -d "$romfs" ]] || { echo "no RomFS at $romfs" >&2; exit 2; }
    echo "== RomFS ($(du -sh "$romfs" | cut -f1)) -> $BASE/game/romfs"
    upload_tree "$romfs" "$BASE/game/romfs" resume
fi
if [[ $PACK == 1 ]]; then
    pack="$ROOT/build/shader_cache/spirv.pack"
    [[ -f "$pack" ]] || { echo "no $pack: run scripts/pack_shader_cache.py" >&2; exit 2; }
    stage="$ROOT/build/pack_upload"
    rm -rf "$stage" && mkdir -p "$stage/spirv"
    ln "$pack" "$stage/spirv/spirv.pack" 2>/dev/null || cp "$pack" "$stage/spirv/spirv.pack"   # a hard link: find -type f sees it
    echo "== shader pack ($(du -h "$pack" | cut -f1)) -> $BASE/game/shader_cache/spirv/"
    upload_tree "$stage" "$BASE/game/shader_cache" resume
    rm -rf "$stage"
fi
echo "done. Launch '$TITLE_ID' from the home screen (restart ShadowMountPlus first if it is new); logs: $BASE/logs/"
