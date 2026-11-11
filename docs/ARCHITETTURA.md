# Architettura della pipeline

Obiettivo: trasformare il codice AArch64 del gioco (moduli NSO di Horizon OS, Switch 2) in un binario x86-64 nativo
per i core Zen 2 della PS5, impacchettato come titolo homebrew (cartella `PPSAxxxxx`), **senza emulazione e senza JIT**.

```
 dump dell'utente (exefs: rtld, main, subsdk0, sdk; romfs)
        │
        ▼  1. LIFT  (nxrecomp, Python)                        scripts/lift.sh
 C++ per funzione: f_<indirizzo>(Ctx *__restrict c)            build/lift/<modulo>/c_NNN.cpp
        │
        ▼  2. COMPILAZIONE  clang --target=x86_64-sie-ps5     cmake/toolchains/ps5.cmake, cmake/Zen2.cmake
 oggetti ELF x86-64 (-march=znver2 -O2, FP bit-exact)          build/ps5/
        │
        ▼  3. RUNTIME  (questo repo + port di nxrt)            runtime/
 loader NSO, rilocazioni, linking per nome, dispatch,
 ABI del codice generato, piattaforma PS5 (memoria diretta,
 thread CCX-aware, segnali, file, log)
        │
        ▼  4. LINK + PACKAGING  ld.lld + ps5-native-tool       scripts/make_ppsa.sh
 eboot.bin (FSELF) + sce_module/libc.prx + sce_sys/ (icona,    dist/PPSA97656/
 sfondi 4K BC7, param.json) + game/exefs (file dell'utente)
```

## Da dove vengono i pezzi

| Ruolo | Progetto di riferimento | Come è usato |
|---|---|---|
| Lifter AArch64 → C++ | pokopia-recomp di drakolordx7 (nxrecomp; il repository originale non è più online, il commit fisso viene dal fork [featureJosh/pokopia-recomp](https://github.com/featureJosh/pokopia-recomp)) | scaricato a commit fisso, usato così com'è: copertura 100% verificata sul build `816C5358…` del dump |
| Pattern di static recompilation (dispatch, jump table, cache) | [peiikai/LASRecomp](https://github.com/peiikai/LASRecomp) | design del dispatch con cache diretta, tabelle SoA ordinate |
| HLE di Horizon (kernel, servizi IPC, NVN) | [seedds/nxnative](https://github.com/seedds/nxnative), nxrt di pokopia-recomp | riferimento per il port HLE (fase successiva, vedi ROADMAP) |
| Titolo PS5 (CRT, VideoOut, convertitore ELF→PS5, FSELF, libc.prx) | [ps5-native-app-boilerplate](https://github.com/mpereiraesaa/ps5-native-app-boilerplate) | scaricato a commit fisso; validato dagli autori su firmware 6.02 e 12.70 |
| Sysroot PS5 (header, stub libkernel/Sce*) | [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) v0.45 | sysroot del toolchain, stub per il link |
| Vincoli memoria PS5 misurati | [R-comp](https://github.com/Phi1ow/360-rcomp) (Xbox 360 → PS5) | strategia `ReserveVirtualRange` + memoria diretta |
| GPU (fase successiva) | PS5_Vulkan / RADV (Mesa) | backend Vulkan per NVN2 |

Nessun SDK proprietario Sony è usato.

## Correzioni all'impostazione iniziale

L'analisi iniziale conteneva alcune ipotesi che sui dati reali non reggono. Le ho corrette così:

| Ipotesi | Realtà | Conseguenza |
|---|---|---|
| Serve conversione di endianness | ARM64 della Switch e x86-64 sono **entrambi little-endian** | nessuna conversione: gli accessi in memoria sono `memcpy` diretti (a differenza di Xbox 360 → PC, dove la PowerPC è big-endian) |
| NEON 128-bit → AVX2 256-bit 1:1 | Un registro NEON è 128 bit: la mappatura corretta è su `__m128` codificato VEX (AVX2/FMA3 di Zen 2). Il 256-bit serve solo quando due operazioni indipendenti si possono fondere | il codice generato usa `V128`; con `-march=znver2` clang emette `vpaddd xmm`, `vfmadd`, `vpshufb`; lo SLP vectorizer allarga dove è sicuro. Zen 2 ha datapath AVX 256-bit pieni, quindi niente penalità |
| Allineamento dei dati di gioco a 64 byte, SoA al posto di AoS | Il layout dei dati è deciso dal binario del gioco: cambiarlo romperebbe la semantica | SoA e allineamento a 64 byte applicati **alle strutture del runtime** (tabelle funzioni indirizzi/puntatori separate, cache di dispatch 64 KiB allineata, `Ctx` per thread su una cache line propria) |
| L3 da 8 MB condivisa per CCX | PS5: **2 CCX × 4 MiB** di L3 (8 MiB totali) | mappa core guest → core PS5 per CCX: i core 0–2 (main/render/audio della maggior parte dei motori) sullo stesso CCX0 |
| NVN → Vulkan non disponibile su PS5 | Dal 2026 esiste **PS5_Vulkan** (Mesa/RADV, gfx1013) e Mesa OpenGL 4.6 su AGC | backend NVN2 → Vulkan (come il port Windows di pokopia-recomp) è percorribile |
| Serve un `.pkg` | fPKG firmato richiede i Publishing Tools proprietari; `.ffpkg`/`.ffpfsc` sono immagini da ricostruire a ogni modifica | **cartella PPSA** registrata da ShadowMountPlus: ogni file sostituibile via FTP, log leggibili accanto all'eseguibile |
| LTO completo su tutto | 12 M istruzioni → ~770 MB di C++: LTO completo richiederebbe decine di GB di RAM su un solo thread | ThinLTO per il codice generato, LTO completo solo per il runtime (`PKP_LTO`), con linker della stessa versione LLVM |

## Ottimizzazioni Zen 2 applicate

- **Toolchain**: `--target=x86_64-sie-ps5` (default `-target-cpu znver2`, frame pointer omesso), `-march=znver2 -mtune=znver2`.
  Codice generato `-O2` (dimensione/I-cache: L1I 32 KiB, op cache 4K op per core); runtime `-O3 -funroll-loops`.
- **FP bit-exact**: `-ffp-contract=off` (ARM arrotonda FMUL e FADD separatamente; FMADD è un `fma()` esplicito → `vfmadd`),
  niente fast-math. NaN, FMAX/FMIN, stime reciproche e conversioni half seguono la semantica ARM (`fpexact.h` di nxrecomp).
- **Registri guest**: ogni funzione riceve `Ctx *__restrict c` → LLVM tiene i registri ARM nei registri x86 anche
  attraverso gli accessi alla memoria guest ed elimina le scritture morte dei flag NZCV.
- **Flag NZCV** separati in quattro campi: calcolo branchless (`setcc`), condizioni come `cmov`/salti predicibili.
- **Atomici**: LSE → `lock xadd`/`cmpxchg`; esclusivi LDXR/STXR → CAS; LDXP/STXP 128 bit → `cmpxchg16b` (`-mcx16`).
  Barriere: `dmb` → fence seq_cst solo dove serve (x86 è TSO).
- **Dispatch indiretto**: cache diretta 4096 voci × 16 B (64 KiB, resta in L2 da 512 KiB), hit = un salto predicibile
  (`__builtin_expect`), miss = ricerca binaria su tabelle SoA ordinate.
- **Memoria guest identity-mapped**: indirizzo guest = indirizzo host, nessuna traduzione per accesso.
  Basi fisse dei moduli (0x71_0000_0000 …) piegate in costanti nel codice: `ADRP+ADD` diventa un immediato.
- **Pagine**: 16 KiB (granularità PS5), commit in blocchi grandi (limite di 16.384 voci di page table segnalato su PS5).

## Vincoli della piattaforma PS5 (da port reali)

- Memoria flessibile (mmap anonima, anche PROT_NONE, e il `malloc` di libc): solo ~448 MiB, **e i segmenti
  dell'eseguibile stesso vi sono addebitati** (misurato: eboot da 49 MB → 399 MiB liberi; gioco intero, ~294 MiB
  caricati → 154 MiB liberi). Quindi la memoria guest usa `sceKernelReserveVirtualRange` (nessun costo) + memoria
  diretta (pool ~12 GiB) mappata `MAP_FIXED`, e anche le allocazioni host da 1 MiB in su e gli stack dei thread vanno
  in memoria diretta (`runtime/platform/prospero/big_heap.cpp`, `--wrap` di malloc/free/… al link).
- Pagine da 16 KiB contro i 4 KiB di Horizon: commit pagina per pagina, protezioni arrotondate in modo sicuro, heap
  allocato al primo accesso (`runtime/platform/common/paged_vm.h`, regole in [PORT_NXRT.md](PORT_NXRT.md#memoria-sulla-ps5)).
- Finestra GPU di PS5_Vulkan `[0x2_0000_0000, 0x3_0000_0000)`: mai memoria guest lì.
- Un titolo non può fare `dlopen`: tutto linkato statico in un `eboot.bin`.
- `thread_local` richiede `__emutls_get_address`, che l'SDK non fornisce: `runtime/platform/prospero/emutls.cpp`.
- `/app0` è la cartella del titolo (scrivibile); `/download0` è persistente e scrivibile. Nella sandbox del titolo
  `opendir` su `/app0` risponde `EPERM` (si elenca con `getdents`) e `getcwd` va in errore dentro libc (non si usa).

Il titolo di bring-up verifica sul campo ognuna di queste ipotesi e scrive l'esito nel log (vedi INSTALLAZIONE_PS5.md).
Prima esecuzione sulla console: `sceKernelReserveVirtualRange` a indirizzo fisso concessa per tutte le regioni del
layout (0x71…–0x7E…, fino a 8 GiB ciascuna), memoria diretta tipo 12 mappata `MAP_FIXED` sopra la prenotazione,
399 MiB di memoria flessibile libera, `/app0` scrivibile (dettagli in ANALISI.md).
