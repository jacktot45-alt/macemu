# Fase 0 — Haalbaarheidscheck

> Doe dit altijd eerst. Eén regel emulatorcode schrijven voordat je weet of het
> doelbestand haalbaar is, is verspilde moeite.

Gereedschap: `macemu-peinfo` (`tools/peinfo_main.cpp`, logica in
`src/pe/feasibility.cpp`).

```bash
./build/macemu-peinfo programma.exe          # rapport
./build/macemu-peinfo programma.exe -v       # + alle geïmporteerde symbolen
./build/macemu-peinfo programma.exe -e       # + exports
```

## Wat er gecontroleerd wordt

| Controle | Waarom het uitmaakt |
|---|---|
| Machine-type | ARM64-binaries hebben niets aan een x86-emulator |
| PE32 vs PE32+ | 32-bit vereist een andere ABI (stdcall, args op de stack) |
| CLR-header | `.NET` = IL-bytecode, geen x86 — de CPU-emulator draait dan nagenoeg niets |
| Import-DLL's | de echte maat van het project |
| Subsystem | console = geen grafische laag nodig; GUI = Fase 4 verplicht |
| TLS-directory | callbacks die vóór het entry point moeten draaien |
| `.pdata` | x64 SEH; alleen nodig zodra de app exceptions gooit |
| Relocations/ASLR | kan het image ergens anders geladen worden? |

## De DLL-classificatie

Elke import wordt in één van vijf bakjes gestopt (tabel `kRules` in
`src/pe/feasibility.cpp` — die groeit mee met wat macemu echt kan):

| Bakje | Betekenis | Voorbeelden |
|---|---|---|
| `[OK]` | macemu heeft hier stubs voor | kernel32, user32, gdi32, msvcrt |
| `[TE DOEN]` | realistisch met handwerk | advapi32, shlwapi, ole32, ucrtbase |
| `[GROOT]` | een heel subsysteem, apart deelproject | comctl32, gdiplus, ws2_32 |
| `[BLOKKADE]` | vereist een complete externe runtime | mscoree, combase, d3d11, d2d1 |
| `[ONBEKEND]` | handmatig uitzoeken | alles wat niet in de tabel staat |

## Het oordeel

| Oordeel | Exit-code | Wat je moet doen |
|---|---|---|
| `HAALBAAR` | 0 | door naar Fase 2 |
| `HAALBAAR MET WERK` | 1 | door, maar reken op weken tot maanden |
| `GROTE SPRONG` | 2 | overweeg eerst een eenvoudiger doelbestand |
| `GEBLOKKEERD` | 3 | **kies een ander bestand** |

De exit-code maakt dit scriptbaar, bijvoorbeeld om een hele map te screenen:

```bash
for f in *.exe; do ./build/macemu-peinfo "$f" >/dev/null; echo "$? $f"; done | sort -n
```

## Over "moderne Minesweeper" — de blocker die je gaat tegenkomen

Dit is geen theoretisch scenario, dus het staat hier expliciet:

**De Minesweeper uit de Microsoft Store is een UWP-app.** Dat betekent:

- de code zit in een app-container met een `AppxManifest.xml`,
- de UI is XAML, niet GDI,
- activatie loopt via WinRT-COM-interfaces (`combase.dll`,
  `api-ms-win-core-winrt-*`),
- de rendering gaat via DirectX/Direct2D,
- er zit vaak nog een .NET- of C++/WinRT-laag tussen.

Elk van die vier is op zichzelf groter dan het hele project tot nu toe. Een
handgeschreven Win32-laag brengt je hier niet; `macemu-peinfo` geeft dan ook
`GEBLOKKEERD` met precies deze reden.

### Wat wél haalbaar is

| Doelbestand | Waarom |
|---|---|
| `winmine.exe` (Windows XP, 32-bit) | puur kernel32 + user32 + gdi32 + een paar advapi32-calls |
| `MineSweeper.exe` (Vista/7) | Win32, maar met comctl32 en meer gdi32 |
| Eigen Win32-app gecompileerd met MinGW-w64 | jij bepaalt de dependency-set |
| `fixtures/window.exe` uit deze repo | het minimale GUI-voorbeeld, draait nu al |

Let op: `winmine.exe` uit XP is **32-bit**. De CPU-core van macemu is op long mode
(64-bit) gericht. De decoder deelt hij grotendeels, maar 32-bit vraagt een aparte
ABI in de Win32-laag (stdcall, argumenten op de stack in plaats van in RCX/RDX/R8/R9).
Dat is een afgebakend, realistisch uitbreidingsproject — zie
[status.md](status.md).

De snelste route naar een draaiend GUI-programma is daarom: compileer zelf een
kleine 64-bit Win32-app met MinGW-w64 (`x86_64-w64-mingw32-gcc -mwindows`), of
gebruik `tools/mkpe.py` zoals `fixtures/window.exe` doet.

## De check in code

`assessFeasibility()` in `src/pe/feasibility.cpp` is bewust een pure functie op een
geparste `PeFile`: geen I/O, makkelijk te testen (`tests/test_pe.cpp`) en makkelijk
uit te breiden. Voeg je een nieuwe DLL-implementatie toe, verplaats hem dan in
`kRules` naar `DepClass::Implemented` — dan blijft het oordeel eerlijk.
