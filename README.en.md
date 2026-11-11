[Italiano](README.md) · **English**

# Pokopia Native on PS5

Static recompilation of **Pokémon Pokopia** (Nintendo Switch 2) into native x86-64 code for the PS5's **AMD Zen 2**
cores, packaged as a homebrew title (folder `PPSA97656`). No emulator, no JIT: the game's AArch64 code becomes C++ and
then x86-64 instructions compiled with `-march=znver2`; NVN graphics run on Vulkan (RADV) and the sound goes to the
console's audio output.

> **No game file is in this repository or in the releases.** The releases hold source code only. Everyone builds the
> folder for the PS5 **from their own legal dump** of the game, following this guide: the recompiled game code, the
> modules, the RomFS and the translated shaders stay on your computer (`build/`, `dist/` and the dump folder are
> ignored by git). Hobby project, not affiliated with Nintendo, The Pokémon Company, Koei Tecmo or Sony.

## Status

| | |
|---|---|
| ✅ Playable on the PS5 | start, language choice, intro (Ditto, Pokédex), name entry with the system keyboard, arrival in the first area |
| ✅ Graphics | NVN → Vulkan on RADV (PS5_Vulkan port), shaders translated from your own dump, lighting, sky and clouds right |
| ✅ Audio | the console's main output (`sceAudioOut`) |
| ✅ Controller | DualSense through `libScePad` |
| ✅ Saves | in the title's folder; a save from an emulator can be imported ([below](#importing-a-save-from-an-emulator)) |
| ⏳ Beyond the first area | not tested yet: report what goes wrong (logs in `logs/`) |

### Supported consoles

| Console | Status |
|---|---|
| **PS5** (standard model) | ✅ **tested**: everything in the table above was verified on this console |
| **PS5 Pro** | ⚠️ **not tested yet**: it has a different GPU, and the port's Vulkan driver (RADV of PS5_Vulkan) is developed for the standard PS5's |

You need a jailbroken PS5 that starts native homebrew titles: kstuff + elfldr, ShadowMountPlus, an FTP server
(etaHEN's, for example, on port 2121).

## Preparing the game dump

**Only dumps extracted into folders are supported.** Packages and images are **not**: NSP, XCI, NSZ, XCZ, NCA, NUP or
any other container file. If your dump is in one of these formats, first extract the ExeFS (the code modules) and the
RomFS (the game's files) into ordinary folders.

Put the dump in the root of this repository, with this name and this layout:

```
Pokemon Pokopia [0400A3A01EE4C000]/            exact folder name
├── base/
│   └── exefs/                                 the base game
│       ├── main
│       ├── main.npdm
│       ├── rtld
│       ├── sdk
│       └── subsdk0
└── updates/
    └── 327680/                                the update: the folder is named after its version number
        ├── exefs/                             the same five files of the update
        │   ├── main
        │   ├── main.npdm
        │   ├── rtld
        │   ├── sdk
        │   └── subsdk0
        └── romfs/                             every file of the game (about 6.6 GB)
```

- The ExeFS files are the NSO modules **without an extension** (`main`, not `main.nso`).
- The scripts use the update with the highest number in `updates/`; without updates they use `base/exefs/` and
  `base/romfs/`.
- The `romfs/` must be **complete**: if your extraction tool keeps the update apart from the base game, merge the two
  RomFS first (the update's files replace the base's).
- Tested with update **327680**. Other versions have not been tried.

The dump folder is ignored by git: nothing of it ends up in the repository.

## Building

**Full step-by-step guide**, with requirements, times, commands and common problems:
**[docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md)**.

In short: an Intel Mac (tested; Linux x86-64 and Apple Silicon Macs not tested), about 60 GB free, 16 GB of RAM, git,
curl, unzip, CMake ≥ 3.25, Ninja, clang (Xcode's is fine) and Python ≥ 3.10. With the dump in place:

```bash
scripts/build_all.sh       # components, the PS5's Vulkan driver, lift, shaders, title -> dist/PPSA97656/
```

The script runs the steps in order and skips the ones already done: if it stops, run it again. The first time takes
several hours (LLVM 1-2 hours, the shader cache about 2 hours), later runs a few minutes. The scripts download the
third-party components at pinned versions (pokopia-recomp, ps5-native-app-boilerplate, ps5-payload-sdk, PS5_Vulkan and
Mesa, Zig, .NET for the shader translator).

## Installing on the PS5

The first time, title + RomFS + shaders (resumes if interrupted):

```bash
scripts/deploy_ftp.sh <ps5-address> 2121 --romfs --shader-pack
```

After a rebuild `scripts/deploy_ftp.sh <ps5-address> 2121` is enough. Then restart ShadowMountPlus and launch
**Pokopia Native** from the home screen. On the console the folder looks like this:

```
/data/homebrew/PPSA97656/
├── eboot.bin, sce_sys/, sce_module/         from your build (dist/PPSA97656)
├── game/
│   ├── exefs/                               the 5 ExeFS files of your dump
│   ├── romfs/                               the RomFS of your dump
│   └── shader_cache/spirv/spirv.pack        the shaders translated from your dump
├── env.txt                                  optional: settings (below)
└── logs/, save/, cache/                     made by the title
```

Without the scripts everything can be copied by hand over FTP with the same layout. The
[release](https://github.com/Pasqcu/Pokopia-Native-PS5/releases) has this layout ready-made, without `eboot.bin` and
without game files (`PPSA97656-template-no-eboot.zip`). Details and logs (in Italian):
[docs/INSTALLAZIONE_PS5.md](docs/INSTALLAZIONE_PS5.md).

### Missing shaders (black characters or objects)

The game also uses shaders that are not in the RomFS, so the cache built from the dump does not hold them: without them
some characters or objects are black. The title saves them in `cache/shader_missing/` when it meets them; after playing
(and whenever you visit new areas):

```bash
scripts/sync_missing_shaders.sh <ps5-address> 2121
```

downloads those blocks, translates them and sends them back to the console. At the next start they are right.

### Importing a save from an emulator

A save from a yuzu-style emulator (Eglantine on the PS5, for example) is in
`nand/user/save/0000000000000000/<user>/0400A3A01EE4C000/` and holds the folders `S00`, `G00` (and `B00`).

1. Start Pokopia Native once, so that it makes its save folder, then close it.
2. Copy `S00`, `G00` and `B00` into `/data/homebrew/PPSA97656/save/account/010000000000000070636552484e4341/`,
   replacing the ones there. Leave the file `010000000000000070636552484e4341.meta` next to the folder; no `.work.*`
   folder or `.commit` file must be left.
3. Start the game: it continues from the imported progress.

Keep a copy of the saves you replace first.

## Settings (`env.txt`)

`/app0/env.txt` (that is `/data/homebrew/PPSA97656/env.txt`) holds `NAME=value` lines, read at start. None is needed:
the defaults are the ones that work.

| Setting | Effect |
|---|---|
| `PKP_HEALTH_MS=200` | writes the logs to disk every 200 ms and a memory line every second: useful if the game closes or the console turns off |
| `PKP_SWKBD_TEXT=Name` | the name used if the system keyboard cannot open |
| `NXRT_THREADS_HOST_STACK_MB=16` | the game threads' stacks (16 by default on the PS5: larger values run out of memory) |
| `NXRT_NVN_INIT_OUTPUTS=false` | turns off the shader-output fix (comparisons only: the black sky and the halos come back) |
| `PKP_BOOT_SCREEN=0` | no start-up screen with the progress |
| `NXRT_NVN_TRACE_SEC=44` | logs every draw of one frame, 44 s after the game's first frame (graphics debugging) |

## Known issues

- At the first start, and the first time a scene appears, the shaders get compiled: there can be short stutters or a
  moment before an object shows up. It improves over the next sessions (the caches stay in `cache/`).
- Some indirect draws written by the GPU are skipped (`indirect draw conversion` warning in the log): an effect may be
  missing here and there.
- Tested up to the first area: beyond it, report problems with `logs/nxrt.log` and `logs/stderr.log` attached.

## Layout

```
CMakeLists.txt, cmake/          build: PS5 toolchain (x86_64-sie-ps5) and the Zen 2 optimisation policy
scripts/                        setup, lift, shader cache, build, PPSA packaging, FTP deploy, missing-shader sync
runtime/include/pkp/            runtime and platform-layer API
runtime/src/                    NSO loader (LZ4, MOD0, RELA/RELR), linking by name, the generated code's ABI, self-test
runtime/platform/posix/         host platform (macOS/Linux) + test executable
runtime/platform/prospero/      PS5 platform (direct memory, signals, files) + the titles' entries, start-up screen
runtime/nxrt_port/              port of pokopia-recomp's HLE runtime: Win32 shim, audio, keyboard, DualSense, NVN on RADV
ppsa/                           param.json, icon, 4K BC7 backgrounds (original artwork), README for the console
tools/                          dump analysis (tools/nx: NSO, NPDM, LZ4), artwork generator
docs/                           build guide, architecture, analysis, roadmap, PS5 install, HLE port
```

pokopia-recomp's code is not modified: the PS5 fixes are transforms CMake applies to a copy of its sources
(`runtime/nxrt_port/CMakeLists.txt`, `runtime/nxrt_port/gfx.cmake`) or files of the port.

## Documentation

- [Build guide, from scratch](docs/BUILD_GUIDE.md)
- In Italian: [architecture and Zen 2 optimisations](docs/ARCHITETTURA.md), [analysis of the game](docs/ANALISI.md),
  [roadmap](docs/ROADMAP.md), [the HLE runtime's port to POSIX and PS5](docs/PORT_NXRT.md),
  [installing on the PS5 and logs](docs/INSTALLAZIONE_PS5.md)

## Credits and licences

This repository's code: **GPL-3.0-or-later** ([LICENSE](LICENSE)), since it links GPL-3.0 components.
Downloaded components (not included), each under its own licence:
pokopia-recomp / nxrecomp by drakolordx7 (GPL-3.0; the original repository is no longer online: the scripts fetch the
pinned commit from the fork [featureJosh/pokopia-recomp](https://github.com/featureJosh/pokopia-recomp)),
[ps5-native-app-boilerplate](https://github.com/mpereiraesaa/ps5-native-app-boilerplate) (GPL-3.0),
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) (GPL-3.0),
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (GPL-3.0) with Mesa RADV (MIT), [SDL3](https://libsdl.org) (zlib),
[Zig](https://ziglang.org) (MIT, LLVM Apache-2.0).
The shader-output fix for RADV (`runtime/nxrt_port/gfx/spirv_init_outputs.cpp`) comes from
[featureJosh/pokopia-recomp](https://github.com/featureJosh/pokopia-recomp/pull/14) (GPL-3.0).
Project references: [Recompia](https://github.com/Dittobyte/Recompia), [nxnative](https://github.com/seedds/nxnative),
[LASRecomp](https://github.com/peiikai/LASRecomp), [R-comp](https://github.com/Phi1ow/360-rcomp).

PlayStation and PS5 are trademarks of Sony Interactive Entertainment; Pokémon and Pokopia are trademarks of their
respective owners.
