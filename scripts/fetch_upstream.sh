#!/usr/bin/env bash
# Fetches the third-party components at pinned versions into third_party/ (never committed: each keeps its own licence).
#   pokopia-recomp            nxrecomp (the AArch64 -> C++ lifter) and nxrt (the Windows HLE runtime being ported)  GPL-3.0
#   ps5-native-app-boilerplate  PS5 title CRT, VideoOut renderer, ELF -> PS5 converter, FSELF writer, libc.prx    GPL-3.0
#   ps5-payload-sdk v0.45     public PS5 headers, libc++ headers, libkernel / Sce* stubs                         GPL-3.0
#   Zig 0.17.1                for its ld.lld (LLD 22) on hosts without an LLVM linker (Intel macOS)               MIT
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$ROOT/third_party"
DL="$ROOT/build/downloads"
mkdir -p "$TP" "$DL"

PKP_URL=https://github.com/drakolordx7/pokopia-recomp.git
PKP_REV=2c29515b23a6d78d967775fd7e9f24f50402cca7
BP_URL=https://github.com/mpereiraesaa/ps5-native-app-boilerplate.git
BP_REV=38c9175869f101c8ab7f2c14195c69c5e581e905
SDK_URL=https://github.com/ps5-payload-dev/sdk/releases/download/v0.45/ps5-payload-sdk.zip
SDK_SHA=5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c
case "$(uname -s)-$(uname -m)" in
    Darwin-x86_64) ZIG=zig-x86_64-macos-0.17.1 ;;
    Darwin-arm64) ZIG=zig-aarch64-macos-0.17.1 ;;
    Linux-x86_64) ZIG=zig-x86_64-linux-0.17.1 ;;
    *) ZIG="" ;;
esac
ZIG_SHA_X86_64_MACOS=9af07b20319fbd0c92ee46ad23e81d6a782b1aaea2cfb8dcbff35f1d2c7ddbb7

sha256() { if command -v shasum >/dev/null; then shasum -a 256 "$1" | cut -d' ' -f1; else sha256sum "$1" | cut -d' ' -f1; fi; }

clone_pinned() {   # url rev dir
    if [[ ! -d "$3/.git" ]]; then git clone --quiet "$1" "$3"; fi
    git -C "$3" fetch --quiet origin "$2" 2>/dev/null || git -C "$3" fetch --quiet origin
    git -C "$3" checkout --quiet "$2"
    echo "$(basename "$3") at $(git -C "$3" rev-parse --short HEAD)"
}

clone_pinned "$PKP_URL" "$PKP_REV" "$TP/pokopia-recomp"
clone_pinned "$BP_URL" "$BP_REV" "$TP/ps5-native-app-boilerplate"

if [[ ! -f "$TP/ps5-payload-sdk-v0.45/ps5-payload-sdk/target/include/c++/v1/__config" ]]; then
    curl -sSL -o "$DL/ps5-payload-sdk-v0.45.zip" "$SDK_URL"
    [[ "$(sha256 "$DL/ps5-payload-sdk-v0.45.zip")" == "$SDK_SHA" ]] || { echo "SDK checksum mismatch" >&2; exit 2; }
    rm -rf "$TP/ps5-payload-sdk-v0.45" && mkdir -p "$TP/ps5-payload-sdk-v0.45"
    unzip -q "$DL/ps5-payload-sdk-v0.45.zip" -d "$TP/ps5-payload-sdk-v0.45"
fi
echo "ps5-payload-sdk v0.45 ok"

if command -v ld.lld >/dev/null; then
    echo "ld.lld found on PATH: Zig not needed"
elif [[ -n "$ZIG" && ! -x "$TP/zig/zig" ]]; then
    curl -sSL -o "$DL/$ZIG.tar.xz" "https://ziglang.org/download/0.17.1/$ZIG.tar.xz"
    if [[ "$ZIG" == zig-x86_64-macos-0.17.1 ]]; then
        [[ "$(sha256 "$DL/$ZIG.tar.xz")" == "$ZIG_SHA_X86_64_MACOS" ]] || { echo "Zig checksum mismatch" >&2; exit 2; }
    else
        echo "note: check $ZIG.tar.xz against https://ziglang.org/download/index.json" >&2
    fi
    rm -rf "$TP/zig" && mkdir -p "$TP/zig" && tar -xJf "$DL/$ZIG.tar.xz" -C "$TP/zig" --strip-components=1
fi
[[ -x "$TP/zig/zig" ]] && echo "zig $("$TP/zig/zig" version): $("$TP/zig/zig" ld.lld --version)"
