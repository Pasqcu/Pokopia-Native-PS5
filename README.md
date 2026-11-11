# Pokopia Native on PS5

Ricompilazione statica di **Pokémon Pokopia** (Nintendo Switch 2) in codice x86-64 nativo per i core **AMD Zen 2**
della PS5, impacchettata come titolo homebrew (cartella `PPSA97656`). Niente emulatore, niente JIT: il codice
AArch64 del gioco diventa C++ e poi istruzioni x86-64 compilate con `-march=znver2`; la grafica NVN passa su Vulkan
(RADV) e l'audio sull'uscita della console.

> **Nessun file del gioco è in questo repository né nelle release.** Le release contengono solo il codice sorgente.
> Ognuno genera la cartella da copiare sulla PS5 **dal proprio dump legale** del gioco, seguendo questa guida: il codice
> del gioco ricompilato, i moduli, la RomFS e gli shader tradotti restano sul tuo computer (`build/`, `dist/` e la
> cartella del dump sono esclusi da git). Progetto amatoriale, non affiliato a Nintendo, The Pokémon Company,
> Koei Tecmo o Sony.

**English summary.** Static recompilation of Pokémon Pokopia (Switch 2) to a native PS5 homebrew title. Playable on a
jailbroken **PS5** (standard model; PS5 Pro not tested yet). No game files are distributed: you build the title folder
from your own dump, which must be **extracted into folders** (`exefs/`, `romfs/`); NSP, XCI, NSZ, NCA, NUP and other
packages are not supported. Build and install steps below (in Italian).

## Stato

| | |
|---|---|
| ✅ Giocabile sulla PS5 | avvio, scelta della lingua, intro (Ditto, Pokédex), nome con la tastiera di sistema, arrivo nella prima landa |
| ✅ Grafica | NVN → Vulkan su RADV (port PS5_Vulkan), shader tradotti dal proprio dump, illuminazione, cielo e nuvole corretti |
| ✅ Audio | uscita principale della console (`sceAudioOut`) |
| ✅ Controller | DualSense tramite `libScePad` |
| ✅ Salvataggi | nella cartella del titolo; si può importare un salvataggio da un emulatore ([sotto](#importare-un-salvataggio-da-un-emulatore)) |
| ⏳ Oltre la prima landa | non ancora provato: segnala cosa non va (log in `logs/`) |

### Console supportate

| Console | Stato |
|---|---|
| **PS5** (modello standard) | ✅ **provata**: tutto quello che è in tabella sopra è stato verificato su questa console |
| **PS5 Pro** | ⚠️ **non ancora provata**: ha una GPU diversa e il driver Vulkan del port (RADV di PS5_Vulkan) è sviluppato per quella della PS5 standard |

Serve una PS5 con jailbreak che avvia titoli homebrew nativi: kstuff + elfldr, ShadowMountPlus, un server FTP
(per esempio quello di etaHEN, porta 2121).

## Preparare il dump del gioco

**È supportato solo il dump estratto in cartelle.** I pacchetti e le immagini **non** sono supportati: NSP, XCI, NSZ,
XCZ, NCA, NUP o qualsiasi altro file contenitore. Se il tuo dump è in uno di questi formati, prima estrai la ExeFS
(i moduli del codice) e la RomFS (i file del gioco) in cartelle normali.

Metti il dump nella radice di questo repository, con questo nome e questa struttura:

```
Pokemon Pokopia [0400A3A01EE4C000]/            nome esatto della cartella
├── base/
│   └── exefs/                                 il gioco base
│       ├── main
│       ├── main.npdm
│       ├── rtld
│       ├── sdk
│       └── subsdk0
└── updates/
    └── 327680/                                l'aggiornamento: la cartella prende il numero di versione
        ├── exefs/                             stessi cinque file dell'aggiornamento
        │   ├── main
        │   ├── main.npdm
        │   ├── rtld
        │   ├── sdk
        │   └── subsdk0
        └── romfs/                             tutti i file del gioco (circa 6,6 GB)
```

- I file della ExeFS sono i moduli NSO **senza estensione** (`main`, non `main.nso`).
- Gli script usano l'aggiornamento con il numero più alto in `updates/`; senza aggiornamenti usano `base/exefs/` e
  `base/romfs/`.
- La `romfs/` deve essere **completa**: se il tuo strumento di estrazione separa l'aggiornamento dal gioco base,
  unisci prima le due RomFS (i file dell'aggiornamento sostituiscono quelli del base).
- Testato con l'aggiornamento **327680**. Altre versioni non sono state provate.

La cartella del dump è esclusa da git: niente di essa finisce nel repository.

## Compilare

**PC**: macOS (testato, Intel) o Linux x86-64, con git, curl, unzip, CMake ≥ 3.25, Ninja, clang ≥ 18 (Xcode va bene),
Python ≥ 3.10, ~16 GB di RAM e qualche decina di GB liberi (codice generato, cache degli shader, RomFS). Gli script
scaricano a versioni fisse i componenti di terze parti (pokopia-recomp, ps5-native-app-boilerplate, ps5-payload-sdk,
PS5_Vulkan e Mesa, Zig, .NET per il traduttore degli shader).

```bash
scripts/setup_host.sh                  # venv Python, componenti a versione fissa, SDK PS5, linker
scripts/build_host_llvm.sh             # una volta: LLVM per gli strumenti di build di Mesa
scripts/build_radv_ps5.sh              # driver Vulkan della PS5 (RADV di PS5_Vulkan) e SDK con libps5platform
scripts/lift.sh                        # i 4 moduli del tuo dump: AArch64 -> C++ (build/lift/)
scripts/setup_shader_tools.sh          # traduttore degli shader (Ryujinx/Ryubing)
scripts/build_shader_cache.sh          # shader del tuo dump -> SPIR-V (~2 ore, riprendibile)
python3 scripts/pack_shader_cache.py   # build/shader_cache/spirv.pack (un file solo per la PS5)

cmake -S . -B build/ps5-gfx -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ps5.cmake \
      -DPKP_MODULES="rtld;subsdk0;sdk;main" -DPKP_TITLE_STAGE=boot -DPKP_NXRT_GFX=ON \
      -DPS5_PAYLOAD_SDK="$HOME/.cache/pokopia-native/ps5gfx/PS5_Vulkan/.deps/native/ps5-payload-sdk"
ninja -C build/ps5-gfx -j8             # il gioco intero: ~770 MB di C++, circa 12 minuti su un i9 a 8 core
scripts/make_ppsa.sh --build build/ps5-gfx   # dist/PPSA97656/: eboot.bin e i moduli del tuo dump in game/exefs
```

## Installare sulla PS5

La prima volta, titolo + RomFS + shader (riprende se si interrompe):

```bash
scripts/deploy_ftp.sh <indirizzo-della-PS5> 2121 --romfs --shader-pack
```

Dopo una ricompilazione basta `scripts/deploy_ftp.sh <indirizzo-della-PS5> 2121`. Poi riavvia ShadowMountPlus e avvia
**Pokopia Native** dalla schermata home. Sulla console la cartella è questa:

```
/data/homebrew/PPSA97656/
├── eboot.bin, sce_sys/, sce_module/         dal tuo build (dist/PPSA97656)
├── game/
│   ├── exefs/                               i 5 file ExeFS del tuo dump
│   ├── romfs/                               la RomFS del tuo dump
│   └── shader_cache/spirv/spirv.pack        gli shader tradotti dal tuo dump
├── env.txt                                  facoltativo: opzioni (sotto)
└── logs/, save/, cache/                     creati dal titolo
```

Senza gli script si può copiare tutto a mano via FTP con la stessa struttura. Dettagli e log:
[docs/INSTALLAZIONE_PS5.md](docs/INSTALLAZIONE_PS5.md).

### Shader mancanti (personaggi o oggetti neri)

Il gioco usa anche shader che non sono nella RomFS, quindi la cache costruita dal dump non li contiene: senza di essi
alcuni personaggi o oggetti appaiono neri. Il titolo li salva in `cache/shader_missing/` quando li incontra; dopo aver
giocato (e ogni volta che visiti zone nuove):

```bash
scripts/sync_missing_shaders.sh <indirizzo-della-PS5> 2121
```

scarica quei blocchi, li traduce e li rimanda alla console. Al successivo avvio appaiono corretti.

### Importare un salvataggio da un emulatore

Un salvataggio di un emulatore in stile yuzu (per esempio Eglantine su PS5) si trova in
`nand/user/save/0000000000000000/<utente>/0400A3A01EE4C000/` e contiene le cartelle `S00`, `G00` (e `B00`).

1. Avvia una volta Pokopia Native, così crea la sua cartella dei salvataggi, poi chiudilo.
2. Copia `S00`, `G00` e `B00` dentro `/data/homebrew/PPSA97656/save/account/010000000000000070636552484e4341/`,
   sostituendo quelle presenti. Lascia il file `010000000000000070636552484e4341.meta` accanto alla cartella e non
   devono restare cartelle `.work.*` o file `.commit`.
3. Avvia il gioco: riprende dai progressi importati.

Fai prima una copia dei salvataggi che sostituisci.

## Opzioni (`env.txt`)

`/app0/env.txt` (cioè `/data/homebrew/PPSA97656/env.txt`) contiene righe `NOME=valore`, lette all'avvio. Nessuna è
necessaria: i valori predefiniti sono quelli che funzionano.

| Opzione | Effetto |
|---|---|
| `PKP_HEALTH_MS=200` | scrive su disco i log ogni 200 ms e una riga sulla memoria al secondo: utile se il gioco si chiude o la console si spegne |
| `PKP_SWKBD_TEXT=Nome` | il nome usato se la tastiera di sistema non si apre |
| `NXRT_THREADS_HOST_STACK_MB=16` | stack dei thread del gioco (predefinito 16 sulla PS5: valori più alti esauriscono la memoria) |
| `NXRT_NVN_INIT_OUTPUTS=false` | disattiva la correzione delle uscite degli shader (solo per confronti: tornano cielo nero e aloni) |
| `PKP_BOOT_SCREEN=0` | niente schermata di avvio con l'avanzamento |
| `NXRT_NVN_TRACE_SEC=44` | registra nel log tutti i draw di un frame, 44 s dopo il primo frame del gioco (debug della grafica) |

## Problemi noti

- Al primo avvio, e la prima volta che una scena appare, gli shader vengono compilati: possono esserci brevi scatti o
  un attimo di ritardo prima che un oggetto compaia. Migliora con le partite successive (le cache restano in `cache/`).
- Alcuni draw indiretti scritti dalla GPU vengono saltati (avviso `indirect draw conversion` nel log): qualche effetto
  potrebbe mancare.
- Provato fino alla prima landa: oltre, segnala i problemi allegando `logs/nxrt.log` e `logs/stderr.log`.

## Struttura

```
CMakeLists.txt, cmake/          build: toolchain PS5 (x86_64-sie-ps5) e politica di ottimizzazione Zen 2
scripts/                        setup, lift, cache shader, build, packaging PPSA, deploy FTP, sync degli shader mancanti
runtime/include/pkp/            API del runtime e del layer di piattaforma
runtime/src/                    loader NSO (LZ4, MOD0, RELA/RELR), linking per nome, ABI del codice generato, self-test
runtime/platform/posix/         piattaforma host (macOS/Linux) + eseguibile di test
runtime/platform/prospero/      piattaforma PS5 (memoria diretta, segnali, file) + entry dei titoli, schermata di avvio
runtime/nxrt_port/              port del runtime HLE di pokopia-recomp: shim Win32, audio, tastiera, DualSense, NVN su RADV
ppsa/                           param.json, icona, sfondi 4K BC7 (artwork originale), README per la console
tools/                          analisi del dump (tools/nx: NSO, NPDM, LZ4), generatore di artwork
docs/                           ARCHITETTURA, ANALISI, ROADMAP, INSTALLAZIONE_PS5, PORT_NXRT
```

Il codice di pokopia-recomp non viene modificato: le correzioni per la PS5 sono trasformazioni applicate da CMake a una
copia dei sorgenti (`runtime/nxrt_port/CMakeLists.txt`, `runtime/nxrt_port/gfx.cmake`) o file del port.

## Documentazione

- [Architettura e ottimizzazioni Zen 2](docs/ARCHITETTURA.md)
- [Analisi del gioco](docs/ANALISI.md)
- [Roadmap](docs/ROADMAP.md)
- [Port del runtime HLE su POSIX e PS5](docs/PORT_NXRT.md)
- [Installazione sulla PS5 e log](docs/INSTALLAZIONE_PS5.md)

## Crediti e licenze

Codice di questo repository: **GPL-3.0-or-later** ([LICENSE](LICENSE)), perché si collega a componenti GPL-3.0.
Componenti scaricati (non inclusi), ciascuno con la sua licenza:
[pokopia-recomp / nxrecomp](https://github.com/drakolordx7/pokopia-recomp) (GPL-3.0),
[ps5-native-app-boilerplate](https://github.com/mpereiraesaa/ps5-native-app-boilerplate) (GPL-3.0),
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) (GPL-3.0),
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (GPL-3.0) con Mesa RADV (MIT), [SDL3](https://libsdl.org) (zlib),
[Zig](https://ziglang.org) (MIT, LLVM Apache-2.0).
La correzione delle uscite degli shader su RADV (`runtime/nxrt_port/gfx/spirv_init_outputs.cpp`) viene da
[featureJosh/pokopia-recomp](https://github.com/featureJosh/pokopia-recomp/pull/14) (GPL-3.0).
Riferimenti di progetto: [Recompia](https://github.com/Dittobyte/Recompia), [nxnative](https://github.com/seedds/nxnative),
[LASRecomp](https://github.com/peiikai/LASRecomp), [R-comp](https://github.com/Phi1ow/360-rcomp).

PlayStation e PS5 sono marchi di Sony Interactive Entertainment; Pokémon e Pokopia sono marchi dei rispettivi titolari.
