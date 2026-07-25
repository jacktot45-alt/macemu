# Status — wat werkt, wat niet

Bijwerken bij elke fase die verandert. Dit bestand is de maatstaf voor voortgang in
een project dat maanden kan duren.

Laatste update: bij het opzetten van Fase 0 t/m 4.

## Testresultaten

```
ctest --output-on-failure
  cpu ............ 83 checks   x86-64 interpreter
  memory ......... 19 checks   pagetabel, rechten, grensgevallen
  pe ............. 35 checks   PE-parser + haalbaarheidscheck
  integration ....             hele emulator op 3 gegenereerde .exe's
```

Alle vier slagen. De integratietest draait echte PE-bestanden en controleert onder
meer dat de gast via `WriteFile` de juiste bytes schrijft en dat de WndProc
daadwerkelijk in de framebuffer tekent.

## Per fase

### Fase 0 — Haalbaarheidscheck ✅

| Onderdeel | Status |
|---|---|
| DOS/NT/COFF/optional header | werkt |
| Sectietabel | werkt |
| Imports (normaal + delay-load, naam en ordinal) | werkt |
| Exports incl. forwarders | werkt (geparsed, niet gevolgd) |
| Base relocations | werkt |
| CLR-header (.NET-detectie) | werkt |
| TLS-directory | gedetecteerd |
| DLL-classificatie + oordeel | werkt, ~45 regels |
| Exit-codes voor scripting | werkt |
| Resources uitpakken | ontbreekt |

### Fase 1 — CPU-core ✅ (voor de geïmplementeerde subset)

| Onderdeel | Status |
|---|---|
| Decoder: prefixes, REX, 1/2/3-byte opcodes, ModRM, SIB | werkt |
| ALU, shifts, bits, mul/div (8–64 bit) | werkt |
| Control flow, call/ret, loop | werkt |
| String-instructies met REP/REPE/REPNE | werkt |
| CMOVcc, SETcc, CMPXCHG(16B), XADD, BSWAP | werkt |
| CPUID, RDTSC, XGETBV | werkt |
| SSE/SSE2-subset (~60 instructies) | werkt |
| Eager flags incl. AF, OF, PF | werkt |
| Herintreedbare `callGuest` | werkt |
| Disassembler voor `--trace` | werkt (ruw maar bruikbaar) |
| x87-FPU | **ontbreekt** |
| AVX/AVX2 | **ontbreekt** (bewust; CPUID meldt ze niet) |
| SEH / exception unwinding | **ontbreekt** |
| Echte threads | **ontbreekt** |
| JIT | **ontbreekt** (interpreter, ~10–50M instr/s) |

### Fase 2 — PE-loader ✅

| Onderdeel | Status |
|---|---|
| Secties mappen incl. `.bss` | werkt |
| Headers in het image | werkt |
| Relocations (DIR64, HIGHLOW) | werkt |
| Imports → HLE-thunks, met DLL-aliassen | werkt |
| Definitieve sectierechten | werkt |
| Stack + guard page | werkt |
| TEB/PEB/LDR/ProcessParameters | werkt |
| Command line + environment | werkt |
| TLS-callbacks aanroepen | **ontbreekt** (wel gedetecteerd) |
| Echte DLL's laden | **ontbreekt** (nep-modules) |
| Delay-load bij eerste gebruik | **ontbreekt** (direct gekoppeld) |

### Fase 3 — Win32-stubs 🟡 werkt voor console-apps

| DLL | Aantal | Status |
|---|---|---|
| kernel32 | ~95 | proces, heap, geheugen, handles, files, tijd, TLS, sync |
| msvcrt/ucrtbase | ~30 | geheugen, strings, printf-familie, CRT-init |
| ntdll | ~10 | heap + unwind-no-ops |
| advapi32 | 9 | registry meldt "niet gevonden" |
| shlwapi | 1 | `PathFindFileNameA` |

Bewuste beperkingen: single-threaded, geen SEH, geen registry, geen resources,
ruwe padvertaling.

### Fase 4 — Venster en rendering 🟡 werkt voor de demo

| Onderdeel | Status |
|---|---|
| Window classes, `CreateWindowEx`, `ShowWindow` | werkt |
| Message loop (`GetMessage`/`PeekMessage`/`Dispatch`) | werkt |
| WndProc-callback via `callGuest` | werkt |
| `WM_CREATE`/`WM_PAINT`/`WM_DESTROY`/`WM_TIMER`/muis/toets | werkt |
| `BeginPaint`/`EndPaint`/`GetDC`/`InvalidateRect` | werkt |
| GDI: brushes, pens, rechthoeken, lijnen, pixels, tekst | werkt |
| Ingebouwd 8×8-font | werkt (ASCII 32–95) |
| SDL2-backend | werkt (als SDL2 aanwezig is) |
| Headless backend + `.ppm`-screenshot | werkt |
| Timers | werkt |
| `BitBlt` / geheugen-DC's | **ontbreekt** (no-op) |
| Non-client rendering, menu's, controls | **ontbreekt** |
| comctl32 | **ontbreekt** |
| Echte fonts | **ontbreekt** |

## De vier eerlijke grenzen

Wat er tussen "dit werkt" en "een echte moderne applicatie draait" nog zit — in
volgorde van hoe groot de sprong is:

1. **SEH / exception unwinding.** Elke MSVC-gecompileerde C++-applicatie gebruikt
   het. `.pdata` parsen, unwind-codes uitvoeren, filters aanroepen. Zonder dit
   crasht elk programma dat één `throw` doet.
2. **Echte threading.** Vraagt een scheduler, per-thread TEB's en nadenken over het
   geheugenmodel. Nu draait `CreateThread` de functie synchroon uit — dat is voor
   veel programma's verkeerd gedrag, geen benadering.
3. **32-bit ondersteuning.** De decoder kan het grotendeels al; de Win32-laag heeft
   een tweede ABI nodig (stdcall, argumenten op de stack). Nodig voor bijvoorbeeld
   `winmine.exe` uit XP.
4. **Prestaties.** Een interpreter haalt ~10–50M instructies per seconde. Voor een
   GUI-app die op invoer wacht is dat prima; voor iets rekenintensiefs niet. Een
   JIT is de oplossing, maar pas als de correctheid staat.

## Volgende stappen, in deze volgorde

1. **Resource-directory parsen** — `LoadIcon`, `LoadString`, dialogen. Klein en
   direct nuttig zodra je een echte applicatie draait.
2. **`BitBlt` en geheugen-DC's** — bijna elke Win32-app tekent gebufferd.
3. **TLS-callbacks aanroepen** — een handvol regels, voorkomt rare bugs.
4. **x87-FPU** — nodig voor oudere code.
5. **Basale SEH** — begin met `__C_specific_handler` en `RtlUnwindEx`.
6. **32-bit modus** — pas hieraan beginnen als 64-bit stabiel is.

## Meten

Elke wijziging: `cd build && ctest --output-on-failure`.
Nieuwe instructie of API? Schrijf er een test bij. `tools/mkpe.py` maakt in een paar
regels een `.exe` die precies dat ene geval isoleert — zonder Windows-compiler.
