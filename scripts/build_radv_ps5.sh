#!/usr/bin/env bash
# The PS5's Vulkan driver for the stage-3 title: RADV of PS5_Vulkan (Mesa's RADV on a PS5 winsys over the console's AGC,
# kernel and VideoOut functions, github.com/mihawk-99/PS5_Vulkan, GPL-3.0), built from its Mesa fork (PS5_Mesa) with the SDK
# fork it is validated with (PS5_PayloadSDK: payload SDK v0.42 + libps5platform). Everything at the revisions PS5_Vulkan pins,
# with one change of ours: RADV's device-memory region moves from 0x40_0000_0000 to 0x80_0000_0000, above the game's fixed
# guest address space (0x60_0000_0000 .. 0x80_0000_0000).
#
#   scripts/build_host_llvm.sh     (once: Mesa's build-time OpenCL tools need the host's LLVM, Clang, SPIRV-LLVM-Translator, libclc)
#   scripts/build_radv_ps5.sh      -> ~/.cache/pokopia-native/ps5gfx/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a
#
# Works in ~/.cache/pokopia-native/ps5gfx: the SDK fork's Makefiles and Mesa's meson files do not take paths with spaces.
# On macOS it supplies what PS5_Vulkan's scripts expect of a Linux host: GNU make >= 3.82, wget, flock, a GNU sha256sum, and an
# llvm-config for the SDK's prospero-clang (Apple clang, which targets x86_64-sie-ps5, plus Zig's ELF tools).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
R="${PKP_PS5GFX_DIR:-$HOME/.cache/pokopia-native/ps5gfx}"
VK_COMMIT=ace98a9                                         # PS5_Vulkan (tools/build-radv.sh, tools/radv-link.sh)
SDK_COMMIT=b83202be73e930050e00de0f1b4d9ec46c0391bf       # PS5_PayloadSDK, the pin of PS5_Vulkan's setup-native-dependencies.sh
MESA_COMMIT=1ba9874f478657ccf6da72ce55f0260164cc0bd0      # PS5_Mesa, the pin of PS5_Vulkan's build-radv.sh
HOST_LLVM="${PKP_HOST_LLVM:-$R/host-llvm}"
TOOLS="$R/tools/bin"
mkdir -p "$R/src" "$TOOLS"
case "$R" in *" "*) echo "the work folder $R has a space in it: set PKP_PS5GFX_DIR" >&2; exit 2 ;; esac
[[ -x "$HOST_LLVM/bin/llvm-config" ]] || { echo "host LLVM missing at $HOST_LLVM: run scripts/build_host_llvm.sh" >&2; exit 2; }

echo "== host tools in $TOOLS"
if [[ ! -x "$R/tools/bin/make" ]]; then   # GNU make >= 3.82 (pattern-rule order): macOS ships 3.81
    (cd "$R/src" && curl -sSL --fail -o make-4.4.1.tar.gz https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz &&
     echo "dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3  make-4.4.1.tar.gz" | shasum -a 256 -c - >/dev/null &&
     rm -rf make-4.4.1 && tar -xzf make-4.4.1.tar.gz && cd make-4.4.1 &&
     ./configure --prefix="$R/tools" --disable-nls >/dev/null && make -j8 >/dev/null && make install >/dev/null)
fi
cat > "$TOOLS/wget" <<'EOF'
#!/bin/sh
url=""; out=""
while [ $# -gt 0 ]; do case "$1" in -q) shift ;; -O) out="$2"; shift 2 ;; *) url="$1"; shift ;; esac; done
exec curl -sSL --fail -o "$out" "$url"
EOF
printf '#!/bin/sh\nexit 0\n' > "$TOOLS/flock"
cat > "$TOOLS/sha256sum" <<'EOF'
#!/bin/sh
check=0; files=""
for a in "$@"; do case "$a" in --check|-c) check=1 ;; --strict|--status|--quiet|-s) ;; *) files="$files $a" ;; esac; done
if [ $check = 1 ]; then exec shasum -a 256 -c $files >/dev/null; else exec shasum -a 256 $files; fi
EOF
# The SDK's bin/prospero-* wrappers run "<llvm-config --bindir>/{clang,ld.lld,llvm-ar,...}". Their LLVM is this folder:
# Apple clang (the compiler of the rest of the title), Zig's LLD as the linker (the host LLVM has no lld), the host LLVM's
# binutils. The SDK finds it as llvm-config-21, which its wrappers try before llvm-config; Mesa's meson, for the host build
# of mesa_clc, finds the host LLVM's own llvm-config first on PATH.
ZIG="$ROOT/third_party/zig/zig"
printf '#!/bin/sh\nexec "%s" ld.lld "$@"\n' "$ZIG" > "$TOOLS/ld.lld"
for n in ar ranlib objcopy nm strip; do
    rm -f "$TOOLS/llvm-$n"
    if [[ -x "$HOST_LLVM/bin/llvm-$n" ]]; then ln -s "$HOST_LLVM/bin/llvm-$n" "$TOOLS/llvm-$n"
    else printf '#!/bin/sh\nexec "%s" %s "$@"\n' "$ZIG" "$n" > "$TOOLS/llvm-$n"; fi
done
ln -sf /usr/bin/clang "$TOOLS/clang"
ln -sf /usr/bin/clang++ "$TOOLS/clang++"
printf '#!/bin/sh\ncase "$1" in --bindir) echo "%s" ;; --version) echo 21.1.8 ;; --prefix) echo "%s" ;; *) exit 1 ;; esac\n' \
    "$TOOLS" "$R/tools" > "$TOOLS/llvm-config"
cp "$TOOLS/llvm-config" "$TOOLS/llvm-config-21"
chmod +x "$TOOLS"/*

echo "== sources at their pins"
[[ -d "$R/PS5_Vulkan/.git" ]] || git clone -q https://github.com/mihawk-99/PS5_Vulkan.git "$R/PS5_Vulkan"
git -C "$R/PS5_Vulkan" -c advice.detachedHead=false checkout -q "$VK_COMMIT"
[[ -d "$R/PS5_PayloadSDK/.git" ]] || git clone -q https://github.com/mihawk-99/PS5_PayloadSDK.git "$R/PS5_PayloadSDK"
if [[ ! -d "$R/PS5_Mesa/.git" ]]; then
    git init -q "$R/PS5_Mesa"
    git -C "$R/PS5_Mesa" remote add origin https://github.com/mihawk-99/PS5_Mesa.git
fi
git -C "$R/PS5_Mesa" cat-file -e "$MESA_COMMIT^{commit}" 2>/dev/null || git -C "$R/PS5_Mesa" fetch -q --depth 1 origin "$MESA_COMMIT"
# our one change, as a commit on top of the pin (tools/build-radv.sh exports a revision with git archive)
patched="$(git -C "$R/PS5_Mesa" log --format=%H --grep='device-memory region at 0x8000000000' -1 2>/dev/null || true)"
if [[ -z "$patched" ]]; then
    git -C "$R/PS5_Mesa" -c advice.detachedHead=false checkout -q "$MESA_COMMIT"
    sed -i '' 's|^#define RADV_PS5_REGION_BASE UINT64_C(0x4000000000)|#define RADV_PS5_REGION_BASE UINT64_C(0x8000000000)   /* Pokopia Native: above the guest window */|' \
        "$R/PS5_Mesa/src/amd/vulkan/winsys/ps5/radv_ps5_platform.c"
    git -C "$R/PS5_Mesa" -c user.name="Pokopia Native" -c user.email="noreply@example.invalid" commit -q -am \
        "radv/ps5: device-memory region at 0x8000000000 (above a recompiled title's guest window)"
    patched="$(git -C "$R/PS5_Mesa" rev-parse HEAD)"
fi

echo "== SDK fork $SDK_COMMIT"
SDK="$R/PS5_Vulkan/.deps/native/ps5-payload-sdk"
if [[ "$(cat "$SDK/.ps5-sdk-revision" 2>/dev/null)" != "$SDK_COMMIT" ]]; then
    rm -rf "$R/sdk-tree" && mkdir -p "$R/sdk-tree" && git -C "$R/PS5_PayloadSDK" archive "$SDK_COMMIT" | tar -x -C "$R/sdk-tree"
    PATH="$TOOLS:$PATH" LLVM_CONFIG="$TOOLS/llvm-config" bash "$R/sdk-tree/platform/tools/setup-sdk.sh" "$SDK" "$SDK_COMMIT" "$R/PS5_Vulkan/.deps/native"
fi

echo "== Python for Mesa's generators (meson, mako)"
[[ -x "$R/venv/bin/meson" ]] || { python3 -m venv "$R/venv" && "$R/venv/bin/pip" install -q meson ninja mako markupsafe pyyaml packaging; }

# build-radv.sh's host build of mesa_clc links the host LLVM, built without RTTI (LLVM's default; distributions turn it on):
# Mesa then has to build without it too. A meson in front of the venv's adds the option to that setup only.
cat > "$TOOLS/meson" <<EOF
#!/bin/sh
case " \$* " in *" setup "*"-Dmesa-clc=enabled"*) exec "$R/venv/bin/meson" "\$@" -Dcpp_rtti=false ;; esac
exec "$R/venv/bin/meson" "\$@"
EOF
chmod +x "$TOOLS/meson"

echo "== RADV (release) at ${patched:0:12}"
# Mesa's meson adds Objective-C on a macOS build machine: Apple's compiler for it (the host LLVM's clang, first on PATH, has
# no macOS SDK).
OBJC=/usr/bin/clang OBJCXX=/usr/bin/clang++ \
PATH="$HOST_LLVM/bin:$TOOLS:$R/venv/bin:$PATH" PKG_CONFIG_PATH="$HOST_LLVM/lib/pkgconfig:$HOST_LLVM/share/pkgconfig:${PKG_CONFIG_PATH:-}" \
    PS5_MESA_FORK="$R/PS5_Mesa" PS5_MESA_REVISION="$patched" MESON="$TOOLS/meson" NINJA="$R/venv/bin/ninja" \
    bash "$R/PS5_Vulkan/tools/build-radv.sh" release
ls -la "$R/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
