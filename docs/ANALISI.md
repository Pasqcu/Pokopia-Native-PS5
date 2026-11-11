# Analisi del gioco

Rigenerabile con `.venv/bin/python tools/analyze_game.py` (scrive `build/analysis/analysis.json`, mai pubblicato).
Qui solo misure e identificativi, nessun contenuto del gioco.

## Il dump

| | |
|---|---|
| Titolo | Pokémon Pokopia, program ID `0400A3A01EE4C000` (Nintendo Switch 2, codename *Ounce*) |
| Motore | KTGL di Koei Tecmo, grafica NVN2, SDK Nintendo 20.5.25 |
| Layout | `base/exefs` + `updates/327680/{exefs,romfs}` (la RomFS dell'update è completa, 6,1 GB) |
| Formato moduli | NSO0 con segmenti LZ4 (flag `0x3f`), già decifrati: nessuna chiave necessaria |
| NPDM | applicazione 64-bit, spazio indirizzi 39-bit, thread principale priorità 44, core 0, stack 1 MiB; priorità consentite 28–59 sui core 0–5 (sei core applicativi); 46 servizi dichiarati; 49 SVC consentite |

## Moduli (update 327680)

| Modulo | Build ID (prefisso) | .text | Parole istruzione | Funzioni (FDE) | Ruolo |
|---|---|---|---|---|---|
| `main` | `816C5358E8178D80` | 38,4 MiB | 10.053.880 | 145.121 | il gioco (KTGL) |
| `sdk` | `CC7DA00D672399EF` | 7,7 MiB | 2.023.164 | 38.283 | Nintendo SDK: libc, libm, nn::os/fs/hid/crypto…, NVN2 |
| `subsdk0` | `9646A69CB1024BA9` | 0,7 MiB | 181.208 | 1.909 | SDK movie (WebM) |
| `rtld` | `BCF5E512E7B0DF65` | 9,6 KiB | 2.464 | 34 | loader dinamico |

Import tra moduli: `main` prende 921 simboli da `sdk` e 20 da `subsdk0`; `subsdk0` 317 da `sdk` e 40 da `main`;
`sdk` 8 da `main`. Le SVC sono tutte in `sdk` (57 numeri distinti) e `rtld`: il gioco parla col kernel solo tramite l'SDK.

Composizione di `main` per classe di codifica A64: load/store 3,23 M, data-processing immediato 2,14 M, salti/sistema
1,85 M, data-processing registro 1,64 M, SIMD/FP 0,52 M, load/store SIMD 0,34 M, atomici LSE 20,5 k.

### Confronto con le versioni supportate dai progetti di riferimento

pokopia-recomp e nxnative supportano la 2.0.1 (update 655360). Nel dump dell'utente **`sdk`, `subsdk0` e `rtld` sono
identici** alla 2.0.1 (stesso SHA-256), mentre `main` è un build diverso (`816C5358…` contro `76A489F3…`).
Il lifter non dipende dal build; le *site list* e gli hook di gioco del runtime Windows invece sì (101 indirizzi fissi
della 2.0.1, solo per funzioni PC: tastiera/mouse, menu, TAA). Aggiornare il gioco alla 2.0.1 sulla console e rifare il
dump di `main` dà la massima compatibilità con quei componenti.

## Risultati della ricompilazione

| Fase | Risultato |
|---|---|
| Copertura istruzioni (`nxrecomp cover`) | **100%** su tutti e 4 i moduli, 0 parole non traducibili |
| Lift completo | 81 s: `main` 146.822 funzioni (8.934 landing pad C++, 5.977 jump table), `sdk` 47.943, `subsdk0` 2.730, `rtld` 37 → ~770 MB di C++ |
| Compilazione per PS5 (`x86_64-sie-ps5`, `-march=znver2 -O2`) | tutti i moduli, 0 errori. `sdk`+`rtld`: 46 oggetti in ~2 min (16 job); `main`+`subsdk0`: 236 oggetti in 690 s (8 job, 5.231 s CPU, picco 1,2 GB per processo), 323 MB di oggetti per `main` |
| Self-test CPU su x86-64 | **10/10**: strlen, memcpy (104 casi fino a 1 MiB), memset, strcmp/memcmp, snprintf con FP e variadici AAPCS64, strtod bit-exact, sqrtf/sinf/cosf, qsort con comparatore host (116.996 chiamate indirette), SHA-256 (vettori FIPS + 16 MiB), HMAC-SHA256 (RFC 4231) |
| Prestazioni | SHA-256 del codice SDK ricompilato: 134 MB/s contro 219 MB/s di un'implementazione C di riferimento (Intel i9-9880H) |

## Risultati sulla console

Prima esecuzione del titolo `PPSA97656` su una PS5 (cartella registrata da ShadowMountPlus), log letto via FTP da
`/data/homebrew/PPSA97656/logs/pokopia_native.log`:

```
CPU: AMD Eng Sample: 100-000000189-11
memory: direct 12288 MiB, flexible free 399 MiB, page 16 KiB
[guest] arena 0x7e00000000 (direct memory type 12), stack top 0x7e00100000
[loader] rtld: build BCF5E512E7B0DF65 ... at 0x7700000000 (direct memory type 12), 39 relative
[loader] sdk: build CC7DA00D672399EF, 0xe08870 bytes + bss 0x1e7790 at 0x7500000000 (direct memory type 12), 20950 relocations
[loader] sdk: 20950 symbol relocations bound, 0 unresolved
PASS strlen / memcpy (104 casi) / memset / strcmp / snprintf / strtod / libm / qsort (116996 chiamate host) / hmac
PASS sha256   3 vectors + 16 MiB; guest 135 MB/s, host ref 232 MB/s
CPU self-test: 10 passed, 0 failed, 0 skipped
OK   main image 0x7100000000 +0x10000000   sceKernelReserveVirtualRange
OK   hle thunks 0x7400000000 +0x1000000    sceKernelReserveVirtualRange
OK   heap 8G    0x7800000000 +0x200000000  sceKernelReserveVirtualRange commit:ok
OK   alias 8G   0x7a00000000 +0x200000000  sceKernelReserveVirtualRange
OK   stacks 4G  0x7c00000000 +0x100000000  sceKernelReserveVirtualRange
layout probe: 5 of 5 regions
```

Cosa conferma:

- il codice dell'SDK del gioco, ricompilato, dà su Zen 2 **gli stessi risultati bit per bit** che su un x86-64 Intel
  (stessa stringa di `snprintf`, `strtod` bit-exact, digest SHA-256 e HMAC corretti);
- le prenotazioni a **indirizzo fisso** alle basi del lift (0x71…–0x7E…) sono concesse dal kernel: il layout del runtime
  nxrt (heap 8 GiB, alias 8 GiB, stack 4 GiB) si può usare così com'è, senza rifare il lift con altre basi;
- la memoria diretta di **tipo 12** mappata `MAP_FIXED` sopra una prenotazione funziona per la memoria CPU;
- `/app0` (la cartella del titolo) è **scrivibile** con ShadowMountPlus: i log si leggono via FTP accanto all'eseguibile;
- prestazioni: lo SHA-256 dell'SDK ricompilato va al 58% di un'implementazione C nativa (135 contro 232 MB/s),
  il costo della semantica ARM esatta (flag, registri in memoria, crittografia ARMv8 emulata) senza ottimizzazioni dedicate.

## Cosa serve ancora per giocare

Il gioco chiede al sistema (via SDK) memoria, thread e sincronizzazione (SVC), file della RomFS (`fsp-srv`),
input (`hid`), audio (`audren`/`audout`), video (`vi`, `nvdrv`) e soprattutto la GPU tramite NVN2
(131.426 shader Ampere secondo il profilo nxnative del gioco). Il piano è in [ROADMAP.md](ROADMAP.md).
