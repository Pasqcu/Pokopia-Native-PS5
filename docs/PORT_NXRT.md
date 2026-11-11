# Port del runtime HLE (nxrt) su POSIX e PS5

Il runtime che fa da Horizon OS per il codice ricompilato è **nxrt** di pokopia-recomp: kernel (SVC, thread,
sincronizzazione, memoria), IPC HIPC/CMIF, servizi di sistema (fs, hid, audio, time, settings, am, …), loader dei
moduli, eccezioni C++. Upstream è scritto per Windows. Il port lo compila **senza modificarne i sorgenti** (l'albero
upstream resta identico al commit fissato):

```
third_party/pokopia-recomp/runtime/src/**   upstream, invariato (commit fissato in scripts/fetch_upstream.sh)
runtime/nxrt_port/compat/                   windows.h, psapi.h, tlhelp32.h, avrt.h, bcrypt.h, mmsystem.h, intrin.h, dbghelp.h
runtime/nxrt_port/src/win32_shim.cpp        sincronizzazione, thread, tempo, memoria virtuale, sezioni/viste
runtime/nxrt_port/src/win32_files.cpp       file, conversioni UTF-8, lingua di sistema
runtime/nxrt_port/src/diag_port.cpp         sostituti dei file diagnostici solo-Windows
runtime/nxrt_port/src/port_stubs.cpp        risposte dei moduli esclusi
runtime/nxrt_port/src/pkp_boot.cpp          sequenza di boot comune a host e PS5, cancello NVN
runtime/nxrt_port/src/port_fs.cpp           elenco cartelle (opendir, poi getdents sulla PS5) per la RomFS
runtime/platform/common/paged_vm.*          memoria guest a pagine da 16 KiB, heap allocato al primo accesso (PS5)
runtime/platform/prospero/big_heap.cpp      allocazioni host grandi e stack dei thread in memoria diretta (PS5)
runtime/include/pkp/platform.h              il layer di piattaforma sotto lo shim (posix / prospero)
```

## Come lo shim mappa Win32

| Win32 | POSIX (host) | PS5 |
|---|---|---|
| `SRWLOCK`, `CONDITION_VARIABLE` (un puntatore, nxrt lo verifica) | parola atomica + `os_sync_wait_on_address` (macOS) / futex | parola atomica + `_umtx_op` |
| `CreateThread`, `ExitThread`, `WaitForSingleObject` | pthread, uscita con `siglongjmp` al trampolino del thread | idem (pthread di libkernel), stack da 16 MiB in memoria diretta |
| `VirtualAlloc/Free/Protect/Query` | `mmap`/`mprotect` + tabella di stato per pagina (per `VirtualQuery`) | VM a pagine (`paged_vm`): `sceKernelReserveVirtualRange` + memoria diretta per pagina da 16 KiB |
| sezione heap + viste alias (`CreateFileMapping`, `MapViewOfFileEx`) | `shm_open` + `mmap MAP_SHARED` | memoria diretta per blocchi da 64 KiB, allocata **al primo accesso** e mappata in tutte le viste |
| `QueryPerformanceCounter` | `clock_gettime(CLOCK_MONOTONIC)` | idem |
| file (`CreateFileW`, `ReadFile` con offset, …) | descrittori POSIX, percorsi UTF-8 | idem (`/app0`, `/download0`) |
| `thread_local` | nativo | `__emutls_get_address` fornito da `runtime/platform/prospero/emutls.cpp` |

## File upstream sostituiti o esclusi

| File | Perché | Sostituto |
|---|---|---|
| `diag/crash, sampler, stackwalk, watchdog, symbols` | SEH, sospensione thread, dbghelp | `diag_port.cpp`: crash handler a segnali, il resto inerte |
| `kernel/starvation_guard` | work-around dello scheduler Windows | spento |
| `mem/write_watch` | tracciamento scritture per la GPU | inerte fino alla fase grafica |
| `services/audio_xaudio2, audio_devices` | XAudio2/MMDevice | sink "paced" di nxrt; `sceAudioOut` sulla PS5 (fase servizi) |
| `services/album, swkbd_prompt` | screenshot WIC, finestra Win32 | non registrati / prompt annullato |
| `media/movie_mf, testtools/tt_game` | Media Foundation (filmati Switch 1), sonde SEH | stub (Pokopia usa `video_codec.cpp`) |

## Trasformazioni di sorgente

Quando un file upstream è portabile tranne poche righe, CMake ne compila una **copia modificata** nella cartella di
build (`build/.../runtime/nxrt_port/transformed/`), con `port_transform(file testo-esatto sostituzione)` in
`runtime/nxrt_port/CMakeLists.txt`. Se il testo da sostituire non si trova più (aggiornamento di upstream) la
configurazione si ferma con un errore, invece di compilare in silenzio una versione sbagliata.

| File | Trasformazione | Motivo |
|---|---|---|
| `services/fs_romfs.cpp` | `long_path()` restituisce il percorso invariato | upstream converte i percorsi host in forma Windows (`\`, prefisso `\\?\`) prima di `std::filesystem`: su POSIX la RomFS non si apriva ("cannot read folder /") |
| `services/fs_romfs.cpp` | `scan()` elenca le cartelle con `src/port_fs.cpp` (`src/fs_romfs_scan.inc`) | l'iteratore di `std::filesystem` usa `opendir`, che nella sandbox del titolo PS5 risponde `EPERM` |

## Sequenza di boot (`pkp_boot.cpp`)

Comune a `pkp_boot` (host) e al titolo PS5, come `game-build/src/pkp_main.cpp` di upstream senza grafica:

1. `nxrt::init` (configurazione, log, spazio di indirizzi, tabella SVC, servizi), `[fs] exefs/romfs`, program id;
2. i 4 NSO mappati alle basi del lift, tabelle dei moduli ricompilati registrate;
3. `nvnBootstrapLoader` sostituito dal **cancello NVN**;
4. `module_protect_all()`: protezioni delle pagine e mappa di memoria di Horizon (quella che `svcQueryMemory`
   riporta), da cui l'rtld del gioco trova i moduli e li riloca da sé, come sulla console;
5. thread principale adottato, ingresso in rtld all'offset 0 con `x0 = 0`, `x1 = handle del thread`.

Il cancello NVN: senza backend grafico (fase M6) la definizione dell'sdk avvierebbe il driver GPU della console
(`NvOsDrvOpen` su `/dev/nvhost-*`) e andrebbe in errore su un puntatore nullo. Il cancello registra nel log la chiamata
e il chiamante, e ferma per sempre quel thread del gioco; gli altri thread continuano.

## Grafica (M6)

`runtime/nxrt_port/gfx.cmake` (`-DPKP_NXRT_GFX=ON`, per ora solo host) costruisce il guscio grafico di nxrt
(`src/gfx`: piattaforma SDL3, device Vulkan 1.3, front end NVN, overlay ImGui) dai sorgenti originali:

| Pezzo | Come |
|---|---|
| SDL3 3.4.18, Vulkan-Headers 1.4.363, volk, VMA 3.4.0, ImGui 1.92.9b | gli stessi URL e SHA-256 di `runtime/cmake/gfx_deps.cmake` upstream, in cache in `build/deps` |
| `tools/tiling` | sorgente |
| `tools/textures` (ASTC → BC7) | astc-encoder 5.7.0 linkato statico al posto delle DLL per ISA (trasformazione di `a2b.cpp`); `bc7e.ispc` compilato con la release ufficiale di ISPC 1.32.0 per il target (Mach-O sull'host, ELF FreeBSD per la PS5) |
| DLSS, FidelityFX API, sonda HDR Win32 | `gfx/gfx_port_stubs.cpp`: "non disponibile" (FSR 1 e NIS, che sono shader, restano) |

Trasformazioni dei file grafici (copie nella cartella di build, come per il resto del port):

| File | Trasformazione | Motivo |
|---|---|---|
| `vk_device.cpp` (Apple) | `VK_KHR_portability_enumeration` sull'istanza, `VK_KHR_portability_subset` sul device | MoltenVK è un driver "portability": senza, il loader non lo elenca |
| `nvn/nvn_gpu_texture.cpp` | lo storage su una texture sRGB (via `EXTENDED_USAGE`) si tiene solo se il driver accetta l'immagine sRGB stessa | MoltenVK rifiuta `R8G8B8A8_SRGB` così: le texture della finestra del gioco restavano senza immagine e nessun frame arrivava al renderer |
| `nvn/nvn_texprep.cpp`, `nvn/nvn_tex_edit.cpp` | `__try`/`__except` → blocco semplice | niente SEH: le letture difensive di memoria guest girano senza protezione (un guasto dà il crash report) |

Shader: il gioco ha 99.012 blocchi shader unici (90.917 pixel, 6.123 vertex, 527 di tessellazione…). Si traducono una
volta dal proprio dump, sul Mac come sul PC: `scripts/setup_shader_tools.sh` (SDK .NET 10 in `third_party/dotnet`,
sorgenti Ryubing al commit fissato `a82350bb77`, harness C# di pokopia-recomp con le sue patch), poi
`scripts/build_shader_cache.sh` (estrazione con `extract_corpus.py`, traduzione a blocchi da 1000, riprendibile;
~13 shader/s su un i9-9880H). La cache resta in `build/shader_cache` e non si pubblica.

Avvio grafico: `pkp_boot ... --gfx window|headless|none --shaders <cache> --shot-every N` (screenshot PNG del frame
presentato). Prima prova headless sul Mac (Radeon Pro 5500M, MoltenVK 1.4.2 → Vulkan 1.3.357):

```
gfx: device ready: AMD Radeon Pro 5500M, queue family 0, validation off, headless
nvn.gpu: NVN back end up on AMD Radeon Pro 5500M (36 submission slots, timeline semaphore, host-memory import available)
nvn: NVN back end installed: 544 of 544 entry points implemented
nvn back end: submits 30052 lists 30052 draws 50 (skipped 85378) presents 2726 pipelines 8 textures 194 (failed 0)
svc: 9190861 calls, 25 distinct, 0 unknown; IAudioOut 17537, fs IStorage 19712, IHidServer 8019 requests, 0 unhandled
```

Le draw saltate erano quelle con shader non ancora tradotti (cache parziale durante la prova). Con la cache completa
(99.012 shader, tutti tradotti) restava un guasto dello shim: una vista di una sezione nasceva tutta senza accesso, anche
dove la sezione aveva già pagine committate, mentre su Windows una pagina committata attraverso una vista è visibile in
tutte. Le viste private con cui il back end NVN importa la memoria del gioco nella GPU (`VK_EXT_external_memory_host`)
erano quindi `PROT_NONE` e Metal rifiutava l'import (-2): ogni indirizzo GPU finiva sul buffer fittizio. Ora lo shim
tiene lo stato di commit per sezione e lo applica a ogni vista. Risultato, headless sul Mac dopo 60 s:

```
nvn back end: submits 19587 lists 19583 draws 26222 (skipped 1172) presents 1778 pipelines 83 textures 214 (failed 4)
```

e il gioco disegna la sua prima schermata (scelta della lingua al primo avvio), corretta: font, interfaccia, sfondo
animato. MoltenVK non ha geometry shader né transform feedback: il Mac è un banco di prova; la PS5 (RADV) li ha.

## Diagnostica

Il crash handler (`diag_port.cpp`) scrive su stderr e nel log: segnale e indirizzo, la funzione guest corrente,
i frame host (catena dei frame pointer dal contesto del segnale) e i **frame guest**, ricostruiti dai record
AArch64 `[x29] = x29 del chiamante, [x29 + 8] = indirizzo di ritorno` che il codice ricompilato tiene nella memoria
guest, con modulo, offset e simbolo più vicino. Esempio reale (il guasto che ha portato al cancello NVN):

```
*** HOST SIGNAL 11 at address 0x0 in guest f_7500151f5c (lr=0x7500577eb8 sp=0x7e000feb50 x0=0x0)
  guest lr  sdk+0x577eb8 (NvOsDrvOpen+0x38)
  guest #0  sdk+0x5b03cc (NvRmGpuNvgpuGetDbgFd+0x9bc)
  ...
  guest #10 main+0xad2990 (nnMain+0xa383a0)
  guest #11 main+0x9a698 (nnMain+0xa8)
  guest #12 sdk+0x118ee8 (_ZN2nn4init5StartEmmPFvvES2_S2_+0x58)
  guest #13 rtld+0x8c
```

## Memoria sulla PS5

Tre fatti misurati sulla console hanno deciso il disegno:

1. **Memoria flessibile**: ~448 MiB per titolo, e i segmenti dell'eseguibile vi sono addebitati. Con il gioco intero
   linkato (~294 MiB caricati) restano 154 MiB: il primo avvio dello stadio 2 è andato in abort (`std::bad_alloc`)
   leggendo `main` (28 MB). Ora `malloc`, `free`, `calloc`, `realloc` e le varianti allineate del titolo passano per
   `--wrap` (scripts/make_ppsa.sh): i blocchi da 1 MiB in su vanno in un'arena di memoria diretta da 16 GiB a
   `0x40_0000_0000` (granuli da 64 KiB, tabelle dentro l'arena stessa); quelli piccoli restano alla libc, e un
   blocco che la libc di sistema ha allocato (strdup, …) torna a lei. Gli stack dei thread (16 MiB, 64 MiB quello
   del gioco) vengono dalla stessa arena.
2. **Pagine da 16 KiB** contro i 4 KiB di Horizon. Senza regole, `MapMemory` dello stack di un thread (indirizzi
   allineati a 4 KiB) falliva, e le protezioni per segmento di `module_protect_all` rendevano read-only la `.data`
   accanto alla `.rodata`. `runtime/platform/common/paged_vm.h`:
   - un commit dà memoria a pagine intere, una per una: 4 KiB dentro una pagina già usata da un vicino sono solo
     azzerati;
   - una protezione che concede (lettura-scrittura) si arrotonda verso l'esterno, una che toglie (sola lettura,
     nessun accesso) verso l'interno;
   - un decommit restituisce solo pagine intere.
3. **Heap**: il gioco chiede subito `SetHeapSize` da **10,4 GiB**. La sezione dell'heap è allocata al primo accesso,
   per blocchi da 64 KiB, dal fault handler (o prima che una chiamata di sistema o la GPU la usi: `prefault` in
   `ReadFile`/`WriteFile` e nell'import dei pool NVN), e ogni blocco è mappato in tutte le viste. Un blocco nuovo si
   azzera in una **finestra privata** prima che una vista lo mostri. All'inizio lo si azzerava attraverso la prima
   vista, già mappata: così cancellava quello che un altro thread vi scriveva nel frattempo (quell'accesso non va in
   fault). Sintomo sulla console: un puntatore nullo in un array del layout dell'interfaccia (KTGL) e
   `HOST SIGNAL 11 at address 0xc in guest f_7101efa38c (lr=0x7101f0a3e0)` al secondo frame della prima scena (~15 s).
4. **Pool NVN importati nella GPU**: nxrt importa un pool a blocchi da 80 MiB, ognuno attraverso una **vista privata**
   dell'heap (il pool più grande, da 3500 MiB, ne usa 55). Il port ammetteva 16 viste per sezione: dalla sedicesima
   l'import ripiegava sulla vista principale dell'heap (`slow pool import: … at its own address` nel log). Sulla
   console, dopo ~81 s, in apertura, `prefault` vi leggeva una pagina senza accesso e il gioco si chiudeva
   (`SIGSEGV` in `pv::prefault`, thread `nvn-render`: `import_range` ← `pool_chunk` ← `resolve_buffer` ←
   `gpu_untile`). Ora le viste sono 256 e `prefault` non tocca più la memoria: sotto il lock dà la memoria ai blocchi
   committati e lascia come sono, scrivendolo su stderr (`pkp: prefault of …`), i blocchi non committati e le pagine
   senza accesso.

Misure, identiche sull'host in simulazione e sulla console, al punto NVN:

```
guest memory: 209 MiB backed (heap 130 of 10610 MiB committed), 2084 faults
host: big blocks 613 MiB (22 blocks: stack e buffer), flexible available 154 MiB
```

Lo stesso algoritmo gira sull'host con `PKP_PS5_VM=1` (pagine da 16 KiB, pool di memoria condivisa al posto della
memoria diretta): ogni modifica si prova sul Mac prima di un avvio sulla console. Il crash dell'heap qui sopra si
riproduceva così, identico, dopo ~23 s, e mai senza `PKP_PS5_VM`; con la finestra privata il gioco arriva alla scadenza:

```bash
PKP_PS5_VM=1 build/host-game/runtime/nxrt_port/pkp_boot "<dump>/updates/327680/exefs" "<dump>/updates/327680/romfs" \
    --seconds 30 --gfx headless --shaders build/shader_cache/spirv
```

## Sandbox del titolo PS5

| Osservato | Conseguenza |
|---|---|
| `opendir` su `/app0/...` → `EPERM` (errno 1); `open`, `stat`, `getdents` funzionano | `src/port_fs.cpp` elenca con `getdents` dopo il primo `EPERM` |
| `getcwd` va in `SIGSEGV` dentro la libc di sistema | `GetFullPathNameA` risolve i percorsi relativi sotto `/app0/` |
| il server FTP di etaHEN restituisce `eboot.bin` e `.prx` **decrittati** (ELF), senza il segmento `PT_SCE_VERSION` | per verificare un caricamento si confronta il download con `build/ppsa/eboot.elf`, non con `eboot.bin` |
| lo stderr del titolo non si vede | il crash report completo (registri guest x0–x30, frame host letti solo dentro lo stack del thread, scansione dello stack, modulo di sistema del PC, ultimi file aperti) va in `logs/nxrt.log` |
| `SDL_Init failed: No available video device` dopo 0,09 s (titolo grafico): SDL3 legge l'ambiente con `dlsym(RTLD_DEFAULT, "environ")`, che il titolo non ha, quindi non vede `SDL_VIDEO_DRIVER=dummy` | `gfx/gfx_boot.cpp` passa il driver come hint SDL (`dummy`, o il valore di `SDL_VIDEO_DRIVER` in `env.txt`) |
| la splash screen di sistema resta sopra ogni immagine finché il titolo non chiama `sceSystemServiceHideSplashScreen` | `boot_screen.cc` la toglie subito e mostra la [schermata di avvio](INSTALLAZIONE_PS5.md#stadio-3-titolo-grafico-schermata-di-avvio) |

Sul target PS5 clang disattiva RTTI ed eccezioni per default: il port compila con `-frtti -fexceptions` (nxrt usa
`dynamic_cast` e `try`) e il titolo linka `libc++abi`/`libunwind` dell'SDK. `std::atomic_ref`, assente nella libc++
dell'SDK, è fornito da `compat/port_prelude.h`.

## Bug del compilatore trovato durante il port

Apple clang 21 (Xcode) genera codice sbagliato per le variabili `thread_local` di Mach-O quando è attivo un tuning
AMD Zen (`-mtune=znver1/2/3`, implicito in `-march=znver2`): la chiamata a `_tlv_get_addr` riceve il descrittore in
`%r8` invece che in `%rdi`, quindi usa come descrittore il primo argomento della funzione e scrive in uno slot
casuale della memoria di thread. Sintomo osservato: `log_set_thread_name` di nxrt corrompeva la struttura `pthread`
del thread principale e il primo `pthread_self()` successivo abortiva. Riproduzione minima:

```cpp
namespace { thread_local char t_name[64] = {}; }
void set_name(const char* n) { std::snprintf(t_name, sizeof t_name, "%s", n); }
// clang++ -O2 -march=znver2 -c t.cpp  ->  movq _t_name@TLVP(%rip), %r8 ; callq *(%r8)   (sbagliato)
// clang++ -O2 -march=x86-64-v3 -c t.cpp ->  movq _t_name@TLVP(%rip), %rdi ; callq *(%rdi) (corretto)
```

Conseguenza (`cmake/Zen2.cmake`): sull'host macOS si compila con `-march=x86-64-v3` (il sottoinsieme AVX2/FMA/BMI2 di
Zen 2) senza tuning Zen; sulla PS5 restano `-march=znver2 -mtune=znver2`, perché lì il TLS è emulato
(`__emutls_get_address`, una chiamata normale con l'argomento in `%rdi`, verificata).

## Stato

- Host (macOS x86-64): nxrt (meno i file della tabella) si linka con i 4 moduli del gioco in `pkp_boot`. Il boot
  completo arriva al **codice del gioco**: rtld del gioco (rilocazioni), `nn::init`, `nninitStartup`, `nnMain`, i
  servizi del boot (sm, lm, appletOE e le sue interfacce, aoc:u, pctl, time:u, fsp-srv con la RomFS servita dalla
  cartella, acc:u0), fino alla prima richiesta NVN dopo circa 0,5 s:

  ```
  svc: 274 calls, 17 distinct, 0 unknown (most: SendSyncRequest x117 QueryMemory x52 GetInfo x39)
  ... 20 interfacce di servizio, 0 unhandled commands ciascuna
  fs: RomFS image from .../romfs: 25 files, 6 directories, 6582876075 bytes of data
  boot: the game reached NVN: nvnBootstrapLoader("nvnDeviceGetProcAddress") from main+0x105cdbc (nvnLoadCProcs+0x703c)
  lifted functions: 197532, modules: 4, live threads: 9, handles open: 27
  ```

  ```bash
  build/host-game/runtime/nxrt_port/pkp_boot "<dump>/updates/327680/exefs" "<dump>/updates/327680/romfs" --seconds 20
  ```
- **PS5: stesso risultato sulla console** (titolo dello stadio 2, `PKP_TITLE_STAGE=boot`): i 4 moduli caricati e
  registrati, rtld, `nn::init`, `nnMain`, i servizi del boot, RomFS servita da `/app0/game/romfs` (22 file,
  6,58 GB), prima richiesta NVN dopo 0,29 s; schermo con lo stato dopo 20 s, log in `/app0/logs/nxrt.log`:

  ```
  boot: memory: direct 12288 MiB, flexible available 154 MiB (the executable is charged to it), page 16 KiB
  port: folders are listed with getdents (opendir / readdir: errno 1)
  fs: RomFS image from /app0/game/romfs: 22 files, 6 directories, 6582857631 bytes of data
  boot: the game reached NVN: nvnBootstrapLoader("nvnDeviceGetProcAddress") from main+0x105cdbc (nvnLoadCProcs+0x703c)
  boot: after 20 s: GUEST MEMORY: 209 MIB BACKED (HEAP 130 OF 10610 MIB COMMITTED), 2084 FAULTS; big blocks 613 MiB
  ```
