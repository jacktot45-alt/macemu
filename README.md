# macemu

Een x86-64 CPU-emulator met een handgeschreven Win32-compatibiliteitslaag, zodat
een Windows `.exe` op macOS (Apple Silicon of Intel) kan draaien — **zonder** Wine,
Box64 of enige andere bestaande compatibiliteitslaag.

Dit is een **leerproject** over CPU-emulatie en OS/API-emulatie, geen product.

```
    jouw.exe                       macemu
 ┌──────────────┐         ┌────────────────────────────┐
 │ x86-64 code  │────────▶│ Fase 1: CPU-interpreter    │
 │ PE-header    │────────▶│ Fase 2: PE-loader          │
 │ imports      │────────▶│ Fase 3: Win32-stubs (HLE)  │
 │ GDI-calls    │────────▶│ Fase 4: framebuffer → SDL2 │
 └──────────────┘         └────────────────────────────┘
                                     │
                                 macOS-venster
```

## Eerlijke verwachting

Een moderne `.exe` volledig draaien is geen weekendproject. Wine bestaat sinds
1993 en heeft honderden bijdragers. Wat hier staat is een werkend *fundament*
met een duidelijke methode om het uit te breiden — niet een afgevinkte taak.

Wat er **nu werkt** (met bewijs: `ctest` draait het):

| Fase | Onderdeel | Status |
|---|---|---|
| 0 | PE/COFF-analyse + haalbaarheidsoordeel | werkt |
| 1 | x86-64 interpreter (ALU, control flow, strings, SSE2-subset) | werkt |
| 2 | PE-loader: secties, relocations, imports, TEB/PEB, stack | werkt |
| 3 | ~140 Win32-functies (kernel32, msvcrt, ntdll, advapi32) | werkt voor console-apps |
| 4 | Vensters, message loop, WndProc-callback, GDI-tekenen | werkt voor de demo-app |

Wat er **niet** werkt: x87-FPU, AVX, SEH/exceptions, echte threads, DirectX,
.NET, UWP/WinRT, comctl32. Zie [docs/status.md](docs/status.md) voor de volledige,
eerlijke lijst.

## Installeren

macOS (Apple Silicon of Intel) of Linux:

```bash
git clone <deze repo> && cd macemu
./install.sh
```

Het script controleert je gereedschap, installeert zo nodig `cmake` en `sdl2` via
Homebrew, bouwt, draait de tests en doet een rooktest. Uitgebreide uitleg staat in
**[docs/TUTORIAL.md](docs/TUTORIAL.md)**.

## Gebruik

Altijd in deze volgorde — Fase 0 sla je nooit over:

```bash
# Fase 0 — is dit bestand überhaupt haalbaar?
./build/macemu-peinfo /pad/naar/jouw.exe

# Fase 2 — laadt het image, en welke imports mis ik nog?
./build/macemu /pad/naar/jouw.exe --dry-run

# Fase 3+ — draaien
./build/macemu /pad/naar/jouw.exe -v
```

Meegeleverde testprogramma's (gegenereerd door `tools/mkpe.py`, geen Windows nodig):

```bash
./build/macemu fixtures/hello.exe          # console: WriteFile
./build/macemu fixtures/window.exe         # GUI: venster + WndProc + GDI
./build/macemu fixtures/window.exe --headless --screenshot uit.ppm
./build/macemu fixtures/crash.exe          # laat zien hoe een ontbrekende API eruitziet
```

## Hoe het werkt

De kern is **HLE** (High Level Emulation): er wordt geen enkele echte Windows-DLL
uitgevoerd. Elke geïmporteerde functie krijgt een adres in een speciaal geheugen-
gebied. Zodra de geëmuleerde CPU daarheen springt, roept hij een C++-functie aan
die hetzelfde gedrag nabootst en volgens de Win64-conventie terugkeert.

Voor callbacks werkt het andersom: `DispatchMessage` roept via `Cpu::callGuest`
de WndProc van de gast aan, die weer als geëmuleerde x86-64 code draait en van
daaruit weer host-functies aanroept.

## Documentatie per fase

| Document | Inhoud |
|---|---|
| [docs/fase0-haalbaarheid.md](docs/fase0-haalbaarheid.md) | PE-analyse, waarom moderne Minesweeper een doodlopend pad is |
| [docs/fase1-cpu.md](docs/fase1-cpu.md) | Decoder, interpreter, flags, welke instructies er zijn |
| [docs/fase2-loader.md](docs/fase2-loader.md) | Secties, relocations, imports, TEB/PEB |
| [docs/fase3-win32.md](docs/fase3-win32.md) | Het HLE-mechanisme en hoe je een API bijbouwt |
| [docs/fase4-rendering.md](docs/fase4-rendering.md) | Vensters, message loop, GDI → framebuffer |
| [docs/status.md](docs/status.md) | Wat werkt, wat niet, en wat de volgende stap is |
| [docs/TUTORIAL.md](docs/TUTORIAL.md) | Installatie en gebruik, stap voor stap |

## Projectstructuur

```
include/macemu/     publieke headers
src/cpu/            decoder.cpp, exec.cpp, exec_sse.cpp, disasm.cpp   (Fase 1)
src/pe/             pe.cpp, feasibility.cpp, loader.cpp               (Fase 0/2)
src/win32/          win32.cpp, kernel32.cpp, user32.cpp, gdi32.cpp    (Fase 3/4)
src/host/           framebuffer.cpp, window.cpp (SDL2 + headless)     (Fase 4)
tools/mkpe.py       genereert test-.exe's zonder Windows-compiler
tests/              unit- en integratietests (ctest)
```

## Licentie

Leerproject. Gebruik het waarvoor je wilt.
