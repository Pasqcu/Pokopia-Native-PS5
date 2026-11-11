**Italiano** · [English](BUILD_GUIDE.md)

# Guida alla compilazione, da zero

Questa guida porta dal tuo dump del gioco alla cartella `PPSA97656` da copiare sulla PS5. Tutto avviene sul tuo
computer: nessun file del gioco viene scaricato da internet o pubblicato, e la cartella che ottieni è solo tua.

## In breve

```bash
git clone https://github.com/Pasqcu/Pokopia-Native-PS5.git
cd Pokopia-Native-PS5
# copia qui il tuo dump, nella cartella "Pokemon Pokopia [0400A3A01EE4C000]" (passo 2)
scripts/build_all.sh
scripts/deploy_ftp.sh <indirizzo-della-PS5> 2121 --romfs --shader-pack
```

`scripts/build_all.sh` esegue in ordine i passi da 3 a 8 e salta quelli già completati: se si interrompe (rete,
spegnimento), rilancialo e riprende. Sotto, ogni passo spiegato.

## 1. Cosa serve

**Computer**

- Mac con processore Intel: **provato**. Linux x86-64 e Mac con Apple Silicon: non provati. Windows: non supportato
  (gli script sono bash).
- Circa **60 GB liberi**: dump 6,2 GB, cache degli shader 25 GB più il pacchetto per la PS5 9,5 GB, codice generato e
  build circa 3 GB, strumenti circa 7 GB.
- **16 GB di RAM** (la compilazione usa fino a 1,2 GB per processo).
- Strumenti: git, curl, unzip, CMake ≥ 3.25, Ninja, clang (quello di Xcode va bene), Python ≥ 3.10.
  Su Mac: `xcode-select --install`, poi con [Homebrew](https://brew.sh) `brew install cmake ninja python`.
- Connessione a internet: gli script scaricano a versioni fisse i componenti di terze parti (sorgenti, SDK, strumenti).
- Tempo, la prima volta: diverse ore (LLVM 1-2 ore, cache degli shader circa 2 ore, compilazione circa 12 minuti su un
  processore a 8 core). Le volte successive pochi minuti.

**Console**

- **PS5** (modello standard) con jailbreak che avvia homebrew nativi: kstuff + elfldr, **ShadowMountPlus** e un
  **server FTP** (per esempio quello di etaHEN, porta 2121). La **PS5 Pro non è ancora stata provata**.

## 2. Preparare il dump

**Solo dump estratto in cartelle.** NSP, XCI, NSZ, XCZ, NCA, NUP e qualsiasi altro pacchetto **non** sono supportati:
estrai prima la ExeFS (i moduli del codice) e la RomFS (i file del gioco) in cartelle normali.

Copia il dump nella radice del repository, con questo nome e questa struttura:

```
Pokemon Pokopia [0400A3A01EE4C000]/            nome esatto della cartella
├── base/
│   └── exefs/                                 il gioco base: main, main.npdm, rtld, sdk, subsdk0
└── updates/
    └── 327680/                                l'aggiornamento: la cartella prende il numero di versione
        ├── exefs/                             main, main.npdm, rtld, sdk, subsdk0
        └── romfs/                             tutti i file del gioco (circa 6,6 GB)
```

- I file della ExeFS sono i moduli NSO **senza estensione** (`main`, non `main.nso`).
- Gli script usano l'aggiornamento con il numero più alto in `updates/`; senza aggiornamenti usano `base/exefs/` e
  `base/romfs/`.
- La `romfs/` deve essere **completa**: se il tuo strumento separa l'aggiornamento dal gioco base, unisci prima le due
  RomFS (i file dell'aggiornamento sostituiscono quelli del base).
- Provato con l'aggiornamento **327680**. Con altre versioni il gioco potrebbe non funzionare.

La cartella del dump è esclusa da git: niente di essa finisce in un commit.

## 3. Componenti e strumenti

```bash
scripts/setup_host.sh
```

Crea l'ambiente Python del progetto (`.venv`), controlla gli strumenti e scarica a versioni fisse in `third_party/`:
pokopia-recomp (il ricompilatore e il runtime; il repository originale non è più online, quindi il commit fisso viene
dal fork featureJosh/pokopia-recomp), ps5-native-app-boilerplate, ps5-payload-sdk v0.45 e Zig. Pochi minuti.

## 4. Il driver Vulkan della PS5 (una volta sola)

```bash
scripts/build_host_llvm.sh     # LLVM, Clang e libclc per gli strumenti di build di Mesa: 1-2 ore, circa 3 GB
scripts/build_radv_ps5.sh      # RADV di PS5_Vulkan e l'SDK con libps5platform
```

Lavorano in `~/.cache/pokopia-native/ps5gfx` (un percorso senza spazi, come richiedono Mesa e l'SDK). Sul Mac lo script
del driver si procura da solo GNU make, wget, flock e sha256sum, e installa meson e mako in un suo ambiente Python.

## 5. Ricompilare il codice del gioco

```bash
scripts/lift.sh
```

Traduce in C++ i 4 moduli AArch64 (`main`, `sdk`, `subsdk0`, `rtld`) della ExeFS più recente del tuo dump:
circa 770 MB di sorgenti in `build/lift/`.

## 6. Gli shader

```bash
scripts/setup_shader_tools.sh                    # traduttore degli shader (Ryujinx/Ryubing) e .NET 10, senza permessi di amministratore
scripts/build_shader_cache.sh                    # circa 99.000 shader del tuo dump -> SPIR-V: circa 2 ore, riprendibile
.venv/bin/python3 scripts/pack_shader_cache.py   # un unico file per la PS5: build/shader_cache/spirv.pack (circa 9,5 GB)
```

`build_shader_cache.sh` si può fermare e rilanciare in qualsiasi momento: non ritraduce ciò che ha già fatto.

## 7. Compilare il titolo

```bash
cmake -S . -B build/ps5-gfx -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ps5.cmake \
      -DPKP_MODULES="rtld;subsdk0;sdk;main" -DPKP_TITLE_STAGE=boot -DPKP_NXRT_GFX=ON \
      -DPS5_PAYLOAD_SDK="$HOME/.cache/pokopia-native/ps5gfx/PS5_Vulkan/.deps/native/ps5-payload-sdk"
ninja -C build/ps5-gfx -j8
```

Circa 12 minuti su un i9 a 8 core. Con meno di 16 GB di RAM usa `-j4`.

## 8. Creare la cartella

```bash
scripts/make_ppsa.sh --build build/ps5-gfx
```

Il risultato è `dist/PPSA97656/`: `eboot.bin`, `sce_sys/` (icona, sfondi, `param.json`), `sce_module/libc.prx` e i 5
file della ExeFS del tuo dump in `game/exefs/`. La RomFS e gli shader non vengono copiati qui (sono grandi): li carica
il passo 9.

## 9. Installare sulla PS5

Con la PS5 accesa e il server FTP attivo:

```bash
scripts/deploy_ftp.sh <indirizzo-della-PS5> 2121 --romfs --shader-pack
```

Carica in `/data/homebrew/PPSA97656/` il titolo, la RomFS (6,6 GB) e gli shader (9,5 GB). Se si interrompe, rilancialo:
riprende da dove era arrivato. Poi **riavvia ShadowMountPlus** e avvia **Pokopia Native** dalla schermata home.

Senza lo script puoi copiare tutto a mano via FTP, con questa struttura sulla console:

```
/data/homebrew/PPSA97656/
├── eboot.bin, sce_sys/, sce_module/        da dist/PPSA97656/
├── game/
│   ├── exefs/                              da dist/PPSA97656/game/exefs/
│   ├── romfs/                              il contenuto della romfs/ del tuo dump
│   └── shader_cache/spirv/spirv.pack       da build/shader_cache/spirv.pack
└── logs/, save/, cache/                    li crea il titolo
```

Nella [release](https://github.com/Pasqcu/Pokopia-Native-PS5/releases) c'è anche `PPSA97656-template-no-eboot.zip`,
questa struttura già pronta con le cartelle vuote: da sola non è giocabile, va completata con i file dei passi 7-8.

## 10. Dopo la prima partita: gli shader mancanti

Il gioco usa anche shader che non sono nella RomFS, quindi la cache del passo 6 non li contiene: senza, alcuni
personaggi o oggetti appaiono neri. Il titolo li salva quando li incontra; dopo aver giocato, con la PS5 accesa:

```bash
scripts/sync_missing_shaders.sh <indirizzo-della-PS5> 2121
```

Li scarica, li traduce e li rimanda alla console: al prossimo avvio sono corretti. Ripetilo dopo aver visitato zone nuove.

## Aggiornare

- **Nuova versione del progetto**: `git pull`, `scripts/build_all.sh`, poi `scripts/deploy_ftp.sh <indirizzo> 2121`
  (solo il titolo: RomFS e shader sono già sulla console).
- **Nuova versione del gioco**: aggiorna il dump, cancella `build/lift` e `build/shader_cache/spirv.pack`, rilancia
  `scripts/build_all.sh` e reinstalla con `--romfs --shader-pack`. Solo l'aggiornamento 327680 è stato provato.

## Problemi comuni

| Messaggio o sintomo | Cosa fare |
|---|---|
| `no "Pokemon Pokopia [0400A3A01EE4C000]" folder` | il dump non è nella radice del repository o il nome è diverso (passo 2) |
| `missing: cmake` (o ninja, clang++, ...) | installa lo strumento indicato (passo 1) |
| `host LLVM missing` | esegui `scripts/build_host_llvm.sh` (passo 4) |
| personaggi o oggetti neri | `scripts/sync_missing_shaders.sh` dopo aver giocato (passo 10) |
| brevi scatti la prima volta che appare una scena | normale: gli shader si compilano al primo uso, poi restano in `cache/` |
| il gioco si chiude o la console si spegne | aggiungi `PKP_HEALTH_MS=200` in `/data/homebrew/PPSA97656/env.txt`, ripeti e allega `logs/nxrt.log` e `logs/stderr.log` a una segnalazione |

Log, opzioni di `env.txt` e importazione dei salvataggi da un emulatore: [README](../README.md) e
[installazione sulla PS5](INSTALLAZIONE_PS5.md).
