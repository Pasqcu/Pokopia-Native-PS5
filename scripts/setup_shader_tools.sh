#!/usr/bin/env bash
# The offline shader translator of pokopia-recomp on macOS / Linux: Ryujinx's Maxwell/Ampere -> SPIR-V translator (MIT,
# Ryubing fork) built with pokopia-recomp's patches into tools/shaders/harness (C#, .NET 10). The same steps as
# tools/shaders/scripts/setup_ryujinx.ps1 + `dotnet build` there, without PowerShell.
#
#   scripts/setup_shader_tools.sh        then scripts/build_shader_cache.sh
#
# Needs git and the .NET 10 SDK: third_party/dotnet (installed there by this script with Microsoft's dotnet-install.sh,
# no administrator rights) or one on PATH. Downloads only the three projects the translator uses.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RYU_URL="https://git.ryujinx.app/Ryubing/ryujinx.git"
RYU_COMMIT="a82350bb774f70fcbd41c9987bf67a3775409963"   # the pin of pokopia-recomp's tools/shaders (mono-jit-v2, #173)
REFS="$ROOT/third_party/refs/ryubing"
SHADERS="$ROOT/third_party/pokopia-recomp/tools/shaders"
STAGED="$SHADERS/third_party/ryubing"

[[ -d "$SHADERS/harness" ]] || { echo "pokopia-recomp not found: run scripts/fetch_upstream.sh" >&2; exit 2; }
DOTNET="$ROOT/third_party/dotnet/dotnet"
if [[ ! -x "$DOTNET" ]]; then
    if command -v dotnet >/dev/null && dotnet --list-sdks | grep -q '^10\.'; then DOTNET="$(command -v dotnet)"
    else
        echo "== .NET 10 SDK -> third_party/dotnet (Microsoft's dotnet-install.sh)"
        curl -sSL https://dot.net/v1/dotnet-install.sh -o "$ROOT/build/dotnet-install.sh"
        bash "$ROOT/build/dotnet-install.sh" --channel 10.0 --install-dir "$ROOT/third_party/dotnet" --no-path
    fi
fi

echo "== Ryubing $RYU_COMMIT (sparse: Ryujinx.Common, Spv.Generator, Ryujinx.Graphics.Shader)"
if [[ ! -d "$REFS/.git" ]]; then
    mkdir -p "$(dirname "$REFS")"
    GIT_TERMINAL_PROMPT=0 git clone -q --filter=blob:none --no-checkout "$RYU_URL" "$REFS"
fi
git -C "$REFS" sparse-checkout init --no-cone >/dev/null 2>&1 || true
printf '%s\n' /src/Ryujinx.Common/ /src/Spv.Generator/ /src/Ryujinx.Graphics.Shader/ /Directory.Build.props \
    /Directory.Packages.props /global.json /nuget.config /LICENSE.txt > "$REFS/.git/info/sparse-checkout"
GIT_TERMINAL_PROMPT=0 git -C "$REFS" -c advice.detachedHead=false checkout -q "$RYU_COMMIT"

echo "== staging into $STAGED (the build writes obj/bin next to the sources: never in the checkout)"
mkdir -p "$STAGED/src"
for p in Ryujinx.Common Spv.Generator Ryujinx.Graphics.Shader; do
    rsync -a --delete --exclude obj --exclude bin "$REFS/src/$p/" "$STAGED/src/$p/"
done
for f in Directory.Build.props Directory.Packages.props global.json nuget.config LICENSE.txt; do cp "$REFS/$f" "$STAGED/$f"; done

echo "== dotnet build (Release)"
DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 "$DOTNET" build "$SHADERS/harness/SpirvHarness.csproj" -c Release -nologo -v quiet
H="$(ls "$SHADERS"/build/bin/Release/net10.0/spirv_harness.dll)"   # tools/shaders/harness/Directory.Build.props: output in tools/shaders/build
echo "translator ready: $H"
