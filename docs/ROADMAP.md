# Roadmap

| Fase | Stato | Contenuto |
|---|---|---|
| M0 Analisi | ✅ | formati, moduli, build ID, servizi, confronto con le versioni supportate (ANALISI.md) |
| M1 Lift | ✅ | nxrecomp sui 4 moduli del dump: copertura 100%, ~770 MB di C++ in 81 s |
| M2 CPU su x86-64 | ✅ host / ✅ console | loader NSO + rilocazioni + linking per nome + ABI + dispatch; self-test 10/10 su Intel x86-64 e **10/10 sulla PS5**; mappa di memoria del gioco prenotabile agli indirizzi fissi (ANALISI.md, Risultati sulla console) |
| M3 Toolchain PS5 | ✅ | `x86_64-sie-ps5` + Zen 2, oggetti ELF, link con ld.lld, conversione e FSELF, libc.prx riproducibile, cartella PPSA con icona e sfondi |
| M4 Kernel HLE | ✅ host / ✅ console | port su questo layer di piattaforma del kernel di nxrt: SVC di memoria (SetHeapSize, Map/UnmapMemory, QueryMemory), thread (CreateThread/StartThread con mappa CCX), sincronizzazione (ArbitrateLock, WaitProcessWideKeyAtomic, WaitForAddress), TLS per thread, tick a 19,2 MHz. Obiettivo: `nninitStartup` e `nnMain` del gioco in esecuzione. **Raggiunto sull'host e sulla PS5**: il gioco arriva fino alla prima richiesta NVN (274 SVC, 0 sconosciute; 20 interfacce di servizio, 0 comandi non gestiti; 9 thread). Sulla console: heap da 10,4 GiB allocato al primo accesso (130 MiB usati), pagine da 16 KiB, allocazioni host grandi in memoria diretta (PORT_NXRT.md) |
| M5 Servizi IPC | ✅ console | HIPC/CMIF e i servizi del boot: sm, fsp-srv (RomFS da `/app0/game/romfs`, salvataggi in `/app0/save` con journal), set, time, appletOE (tastiera di sistema: libSceImeDialog), hid (DualSense con libScePad → Npad), pl (font), audren (mixer di nxrt) → `sceAudioOut` |
| M6 Grafica | ✅ host / ✅ console | backend NVN2 di nxrt su Vulkan. **Host (macOS, MoltenVK)**: il guscio grafico di nxrt compila e gira (SDL3, Vulkan 1.3, VMA, ImGui, ASTC→BC7 con astcenc + bc7e/ISPC); il gioco disegna la sua prima schermata (scelta della lingua), con 99.012 shader Ampere → SPIR-V tradotti sul Mac col traduttore di Ryujinx. **PS5**: **PS5_Vulkan** (RADV su AGC/VideoOut, alpha) per gfx1013; presentazione con la sua WSI. **Raggiunto**: il gioco è giocabile sulla PS5 (intro, creazione del personaggio, prima landa), con le uscite degli shader di vertice inizializzate (RADV le lasciava NaN: cielo e nuvole neri, aloni bianchi) e la vista D24S8 sull'immagine D32S8 |
| M7 Filmati e rifiniture | ⏳ | movie:: (subsdk0) con libvpx, frame pacing 30/60, profilo dei thread caldi, scatti alla prima compilazione degli shader |
| M8 Release | ✅ v0.1.0 (sorgenti) | la cartella PPSA si genera dal proprio dump (README); nessun file del gioco nelle release; opzionale `.ffpfsc` compresso per l'archiviazione |

## Prossimi passi concreti

1. ~~Provare il titolo di bring-up sulla console~~ — fatto: 10/10, indirizzi fissi concessi, memoria diretta tipo 12,
   `/app0` scrivibile. Le basi del lift restano quelle attuali.
2. ~~Port del kernel di nxrt~~ (M4) — fatto sull'host: nxrt compila invariato sopra uno shim Win32 → POSIX/PS5
   (docs/PORT_NXRT.md) e il gioco esegue `nnMain` fino alla grafica.
3. ~~Stadio 2 sulla console~~ — fatto: stesso punto dell'host ("the game reached NVN"), RomFS servita da `/app0`.
4. **M6, backend grafico** — sull'host: completare la cache shader (`scripts/build_shader_cache.sh`, ~2 ore) e vedere i
   primi frame; poi PS5_Vulkan nel titolo (build di Mesa/RADV per il target, finestra GPU `[0x2_0000_0000, 0x3_0000_0000)`).
5. Allineare il dump alla 2.0.1 (consigliato) o rigenerare le site list per il build `816C5358`.
