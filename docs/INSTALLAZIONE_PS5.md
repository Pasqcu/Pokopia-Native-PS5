# Installazione sulla PS5

## Requisiti della console

- PS5 con jailbreak in grado di avviare titoli homebrew nativi (kernel exploit + **kstuff**, **elfldr**).
- **ShadowMountPlus**: registra le cartelle di `/data/homebrew/` come titoli sulla home.
- Un server **FTP** sulla console (per esempio quello di etaHEN o zftpd).

Il boilerplate da cui deriva il titolo è stato validato dai suoi autori sui firmware 6.02 e 12.70; altri firmware
potrebbero comportarsi diversamente: per questo il titolo prova ogni primitiva e scrive l'esito nel log.

## Copia

1. Sul PC: `scripts/build_all.sh` (o i singoli passi del README) produce `dist/PPSA97656/`.
2. Copia l'intera cartella `PPSA97656` in `/data/homebrew/` sulla console via FTP.
3. Avvia o riavvia ShadowMountPlus e attendi che **Pokopia Native** compaia sulla home (icona e sfondo inclusi).
4. Avvia il titolo. Lo schermo mostra righe PASS / FAIL; una notifica riassume il risultato. Chiudi dalla home (tasto PS).

## Stadio 2 (il gioco sul runtime HLE)

Il titolo costruito con `-DPKP_TITLE_STAGE=boot` (README, "Stadio 2") contiene i 4 moduli del dump in `game/exefs/` e
legge i dati del gioco da `game/romfs/`. La RomFS (~6,6 GB) non entra nella cartella generata: si carica una volta sola.

1. `scripts/deploy_ftp.sh <ip-della-ps5> 2121 --romfs` carica `dist/PPSA97656/` e la RomFS del dump in
   `/data/homebrew/PPSA97656/game/romfs/` (salta i file già presenti con la stessa dimensione, riprende quelli
   interrotti). In seguito, dopo una modifica del runtime, basta `scripts/deploy_ftp.sh <ip> 2121` (solo il titolo).
2. Riavvia ShadowMountPlus se il titolo è nuovo, poi avvialo.
3. Dopo 20 s lo schermo mostra lo stadio raggiunto, lo stato del runtime (SVC, servizi, thread) e la memoria usata.
   Atteso finché manca il backend grafico (e ottenuto sulla console): **"the game reached NVN"**, come sull'host.
4. Log completo: `/data/homebrew/PPSA97656/logs/nxrt.log` (il precedente in `nxrt.log.prev`). Un crash vi lascia segnale, indirizzo, frame host e la
   catena dei frame guest (modulo+offset e simbolo), come in [PORT_NXRT.md](PORT_NXRT.md#diagnostica).

Note sul caricamento: lo script rimanda ogni volta l'intera cartella del titolo (un `eboot.bin` può cambiare senza
cambiare dimensione) e riprende solo i file della RomFS, che non cambiano. Il server FTP di etaHEN mostra `eboot.bin` e
`sce_module/libc.prx` già decrittati: il loro hash via FTP non coincide con quello locale, il contenuto sì
(`build/ppsa/eboot.elf`).

## Stadio 3 (titolo grafico): schermata di avvio

Il titolo con `-DPKP_TITLE_STAGE=boot -DPKP_NXRT_GFX=ON` (`boot-gfx`: NVN su RADV di PS5_Vulkan) toglie subito la
splash screen di sistema e, finché il gioco non presenta il suo primo fotogramma, mostra una **schermata di stato**:

- la fase (1/4 runtime, 2/4 grafica Vulkan/RADV, 3/4 moduli del gioco, 4/4 attesa del primo fotogramma), una
  percentuale **stimata** con barra, il tempo trascorso e un indicatore che gira finché il titolo risponde;
- l'attività: CPU, memoria guest e heap, contatori del backend grafico (submit, draw, texture, pipeline, shader in coda),
  righe di log scritte e da quanto tempo è arrivata l'ultima: se i numeri cambiano, sta lavorando;
- le ultime righe di `logs/nxrt.log` (avvisi in giallo, errori in rosso). Se l'avvio si interrompe, l'errore resta a
  schermo.

Fino all'avvio della grafica la disegna la CPU su VideoOut, poi il renderer sulla swapchain del gioco. La stima
dell'ultima fase segue la durata dell'avvio precedente (`cache/boot_seconds.txt`). `PKP_BOOT_SCREEN=0` in
`/data/homebrew/PPSA97656/env.txt` la toglie (la splash screen sparisce comunque). Log: `logs/nxrt.log`,
`logs/stderr.log` (RADV e Mesa), `logs/boot_trace.txt`.

## Perché una cartella e non un pacchetto

| Formato | Modificabile? | Note |
|---|---|---|
| **Cartella PPSA** (scelto) | sì, file per file via FTP | `eboot.bin`, `game/exefs/*`, `sce_sys/*` si sostituiscono singolarmente; i log restano accanto |
| `.ffpkg` (UFS2) / `.ffpfsc` (PFS compresso) | no, va ricostruito | utile per archiviare o distribuire una versione finale; il boilerplate li genera (`make packages`) |
| fPKG firmato | no | richiede i Publishing Tools proprietari Sony: non usati |

## Log

- `logs/pokopia_native.log` dentro la cartella del titolo (`/data/homebrew/PPSA97656/logs/` via FTP). Verificato sulla
  console: con ShadowMountPlus `/app0` è scrivibile e il log finisce qui.
- Se `/app0` è in sola lettura sul tuo firmware, il log va in `/download0/pokopia_native.log`; l'ultima riga dello
  schermo dice quale percorso è stato usato.
- Per scaricarlo dal PC: `curl -O ftp://<ip-della-ps5>:2121/data/homebrew/PPSA97656/logs/pokopia_native.log`
  (la porta dipende dal server FTP: 2121 per quello di etaHEN).

Cosa guardare:

| Riga | Significato |
|---|---|
| `[loader] sdk: … at 0x7500000000 (sceKernelReserveVirtualRange / direct memory type N)` | prenotazione a indirizzo fisso concessa e tipo di memoria diretta usato |
| `… (mmap (flexible …))` | ripiego sulla memoria flessibile: funziona ma il budget è piccolo (~448 MiB) |
| `FAIL load sdk … cannot map` | indirizzo fisso rifiutato: serve un lift con basi diverse |
| `PASS …` / `FAIL …` | test del codice ricompilato; un FAIL riporta segnale, indirizzo e ultima funzione guest |
| `OK/FAIL heap 8G …` | se lo spazio per l'heap del gioco (8 GiB) è prenotabile |

## Sostituire i file senza ricompilare

- nuovo `eboot.bin` (dopo una modifica del runtime): basta copiare quel file;
- `game/exefs/` da un altro dump (per esempio la 2.0.1): copia i nuovi `rtld` e `sdk` (stadio 2: anche `main` e
  `subsdk0`; il codice ricompilato va rigenerato per quel dump, perché è legato al build ID);
- icona/sfondi: sostituisci `sce_sys/icon0.png`, `pic0.dds`, `pic1.dds`, poi riavvia ShadowMountPlus (la home li tiene in cache).
  Per usare il logo e la key art ufficiali di pokopia.pokemon.com: `tools/art/official_art.sh` (macOS) li scarica e li
  converte in `build/art/local/`, che `scripts/make_ppsa.sh` preferisce a `ppsa/sce_sys/`. Restano solo nella tua copia:
  appartengono a Nintendo / The Pokémon Company e non vanno nel repository.
