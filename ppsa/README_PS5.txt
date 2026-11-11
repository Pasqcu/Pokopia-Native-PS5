POKOPIA NATIVE ON PS5
=====================

What it is
  Pokemon Pokopia (Nintendo Switch 2), statically recompiled from AArch64 to x86-64 and running natively on the
  PS5's Zen 2 cores, with NVN graphics on Vulkan (RADV), sound on the console's audio output and the DualSense.
  Built from YOUR OWN dump: this folder holds the game's code, files and shaders only because you built it from them.
  Do not share it. Tested on a standard PS5; the PS5 Pro is not tested yet.

Layout (/data/homebrew/PPSA97656/)
  eboot.bin, sce_sys/, sce_module/        the title (scripts/make_ppsa.sh)
  game/exefs/                             main, main.npdm, rtld, sdk, subsdk0 from your dump
  game/romfs/                             the game's files from your dump (about 6.6 GB)
  game/shader_cache/spirv/spirv.pack      the shaders translated from your dump
  env.txt                                 optional settings, NAME=value lines (see the repository's README)
  logs/, save/, cache/                    made by the title

Install (jailbroken PS5 that starts native homebrew: kstuff + elfldr, ShadowMountPlus, an FTP server)
  1. Copy this whole folder to /data/homebrew/ (scripts/deploy_ftp.sh <ps5 address> 2121 --romfs --shader-pack).
  2. Start (or restart) ShadowMountPlus; "Pokopia Native" appears on the home screen. Launch it.
  3. After playing, run scripts/sync_missing_shaders.sh <ps5 address> 2121 on the PC: it translates the shaders the
     game used that the RomFS does not hold (without them some characters or objects are black).

While the game starts, a status screen shows the step, an estimated percentage and the last lines of the log
(PKP_BOOT_SCREEN=0 in env.txt turns it off).

Logs
  logs/nxrt.log (and nxrt.log.prev, the previous run), logs/stderr.log, logs/boot_trace.txt.
  PKP_HEALTH_MS=200 in env.txt writes them to disk every 200 ms, for crashes that take the console down.

Saves
  save/account/010000000000000070636552484e4341/ (S00, G00, B00). A yuzu-style emulator's save of the game
  (nand/user/save/0000000000000000/<user>/0400A3A01EE4C000/) can be copied there with the game closed.

Editing without rebuilding
  Every file can be replaced over FTP: eboot.bin (a new build), game/exefs/* (another dump), sce_sys/* (art).
  After replacing sce_sys art, restart ShadowMountPlus (the home screen caches it).
