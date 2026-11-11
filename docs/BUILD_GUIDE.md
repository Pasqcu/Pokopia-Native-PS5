[Italiano](GUIDA_COMPILAZIONE.md) · **English**

# Build guide, from scratch

This guide takes you from your dump of the game to the `PPSA97656` folder you copy to the PS5. Everything happens on
your computer: no game file is downloaded from the internet or published, and the folder you get is yours only.

## In short

```bash
git clone https://github.com/Pasqcu/Pokopia-Native-PS5.git
cd Pokopia-Native-PS5
# copy your dump here, into the folder "Pokemon Pokopia [0400A3A01EE4C000]" (step 2)
scripts/build_all.sh
scripts/deploy_ftp.sh <ps5-address> 2121 --romfs --shader-pack
```

`scripts/build_all.sh` runs steps 3 to 8 in order and skips the ones already done: if it stops (network, power), run it
again and it carries on. Each step is explained below.

## 1. What you need

**Computer**

- Intel Mac: **tested**. Linux x86-64 and Apple Silicon Macs: not tested. Windows: not supported (the scripts are bash).
- About **60 GB free**: dump 6.2 GB, shader cache 25 GB plus the PS5 pack 9.5 GB, generated code and build about 3 GB,
  tools about 7 GB.
- **16 GB of RAM** (the build takes up to 1.2 GB per process).
- Tools: git, curl, unzip, CMake ≥ 3.25, Ninja, clang (Xcode's is fine), Python ≥ 3.10.
  On a Mac: `xcode-select --install`, then with [Homebrew](https://brew.sh) `brew install cmake ninja python`.
- An internet connection: the scripts download the third-party components at pinned versions (sources, SDKs, tools).
- Time, the first time: several hours (LLVM 1-2 hours, shader cache about 2 hours, build about 12 minutes on an 8-core
  processor). Later runs take minutes.

**Console**

- **PS5** (standard model), jailbroken so that it starts native homebrew: kstuff + elfldr, **ShadowMountPlus** and an
  **FTP server** (etaHEN's, for example, on port 2121). The **PS5 Pro is not tested yet**.

## 2. Preparing the dump

**Only dumps extracted into folders.** NSP, XCI, NSZ, XCZ, NCA, NUP and any other package are **not** supported:
extract the ExeFS (the code modules) and the RomFS (the game's files) into ordinary folders first.

Copy the dump into the repository root, with this name and this layout:

```
Pokemon Pokopia [0400A3A01EE4C000]/            exact folder name
├── base/
│   └── exefs/                                 the base game: main, main.npdm, rtld, sdk, subsdk0
└── updates/
    └── 327680/                                the update: the folder is named after its version number
        ├── exefs/                             main, main.npdm, rtld, sdk, subsdk0
        └── romfs/                             every file of the game (about 6.6 GB)
```

- The ExeFS files are the NSO modules **without an extension** (`main`, not `main.nso`).
- The scripts use the update with the highest number in `updates/`; without updates they use `base/exefs/` and
  `base/romfs/`.
- The `romfs/` must be **complete**: if your tool keeps the update apart from the base game, merge the two RomFS first
  (the update's files replace the base's).
- Tested with update **327680**. The game may not work with other versions.

The dump folder is ignored by git: nothing of it ends up in a commit.

## 3. Components and tools

```bash
scripts/setup_host.sh
```

Creates the project's Python environment (`.venv`), checks the tools and downloads into `third_party/` at pinned
versions: pokopia-recomp (the recompiler and the runtime; its original repository is no longer online, so the pinned
commit comes from the fork featureJosh/pokopia-recomp), ps5-native-app-boilerplate, ps5-payload-sdk v0.45 and Zig.
A few minutes.

## 4. The PS5's Vulkan driver (once)

```bash
scripts/build_host_llvm.sh     # LLVM, Clang and libclc for Mesa's build tools: 1-2 hours, about 3 GB
scripts/build_radv_ps5.sh      # RADV of PS5_Vulkan and the SDK with libps5platform
```

They work in `~/.cache/pokopia-native/ps5gfx` (a path without spaces, as Mesa and the SDK require). On a Mac the driver
script provides GNU make, wget, flock and sha256sum itself, and installs meson and mako in its own Python environment.

## 5. Recompiling the game's code

```bash
scripts/lift.sh
```

Translates the 4 AArch64 modules (`main`, `sdk`, `subsdk0`, `rtld`) of your dump's newest ExeFS to C++: about 770 MB of
sources in `build/lift/`.

## 6. The shaders

```bash
scripts/setup_shader_tools.sh                    # the shader translator (Ryujinx/Ryubing) and .NET 10, no administrator rights
scripts/build_shader_cache.sh                    # about 99,000 shaders of your dump -> SPIR-V: about 2 hours, resumable
.venv/bin/python3 scripts/pack_shader_cache.py   # one file for the PS5: build/shader_cache/spirv.pack (about 9.5 GB)
```

`build_shader_cache.sh` can be stopped and run again at any time: it never translates the same shader twice.

## 7. Building the title

```bash
cmake -S . -B build/ps5-gfx -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ps5.cmake \
      -DPKP_MODULES="rtld;subsdk0;sdk;main" -DPKP_TITLE_STAGE=boot -DPKP_NXRT_GFX=ON \
      -DPS5_PAYLOAD_SDK="$HOME/.cache/pokopia-native/ps5gfx/PS5_Vulkan/.deps/native/ps5-payload-sdk"
ninja -C build/ps5-gfx -j8
```

About 12 minutes on an 8-core i9. With less than 16 GB of RAM use `-j4`.

## 8. Making the folder

```bash
scripts/make_ppsa.sh --build build/ps5-gfx
```

The result is `dist/PPSA97656/`: `eboot.bin`, `sce_sys/` (icon, backgrounds, `param.json`), `sce_module/libc.prx` and
the 5 ExeFS files of your dump in `game/exefs/`. The RomFS and the shaders are not copied here (they are large): step 9
uploads them.

## 9. Installing on the PS5

With the PS5 on and its FTP server running:

```bash
scripts/deploy_ftp.sh <ps5-address> 2121 --romfs --shader-pack
```

Uploads the title, the RomFS (6.6 GB) and the shaders (9.5 GB) to `/data/homebrew/PPSA97656/`. If it stops, run it
again: it resumes where it was. Then **restart ShadowMountPlus** and launch **Pokopia Native** from the home screen.

Without the script you can copy everything by hand over FTP, with this layout on the console:

```
/data/homebrew/PPSA97656/
├── eboot.bin, sce_sys/, sce_module/        from dist/PPSA97656/
├── game/
│   ├── exefs/                              from dist/PPSA97656/game/exefs/
│   ├── romfs/                              the content of your dump's romfs/
│   └── shader_cache/spirv/spirv.pack       from build/shader_cache/spirv.pack
└── logs/, save/, cache/                    made by the title
```

The [release](https://github.com/Pasqcu/Pokopia-Native-PS5/releases) also has `PPSA97656-template-no-eboot.zip`, this
layout ready-made with empty folders: it is not playable by itself, fill it with the files of steps 7-8.

## 10. After the first session: the missing shaders

The game also uses shaders that are not in the RomFS, so the cache of step 6 does not hold them: without them some
characters or objects are black. The title saves them when it meets them; after playing, with the PS5 on:

```bash
scripts/sync_missing_shaders.sh <ps5-address> 2121
```

downloads them, translates them and sends them back to the console: at the next start they are right. Run it again
after visiting new areas.

## Updating

- **New version of the project**: `git pull`, `scripts/build_all.sh`, then `scripts/deploy_ftp.sh <address> 2121`
  (the title only: the RomFS and the shaders are already on the console).
- **New version of the game**: update the dump, delete `build/lift` and `build/shader_cache/spirv.pack`, run
  `scripts/build_all.sh` again and reinstall with `--romfs --shader-pack`. Only update 327680 is tested.

## Common problems

| Message or symptom | What to do |
|---|---|
| `no "Pokemon Pokopia [0400A3A01EE4C000]" folder` | the dump is not in the repository root, or its name differs (step 2) |
| `missing: cmake` (or ninja, clang++, ...) | install that tool (step 1) |
| `host LLVM missing` | run `scripts/build_host_llvm.sh` (step 4) |
| black characters or objects | `scripts/sync_missing_shaders.sh` after playing (step 10) |
| short stutters the first time a scene appears | expected: shaders compile on first use, then stay in `cache/` |
| the game closes or the console turns off | add `PKP_HEALTH_MS=200` to `/data/homebrew/PPSA97656/env.txt`, play again and attach `logs/nxrt.log` and `logs/stderr.log` to a report |

Logs, `env.txt` options and importing a save from an emulator: [README](../README.en.md).
