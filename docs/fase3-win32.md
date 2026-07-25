# Fase 3 — De Win32-laag (HLE)

Bestanden: `src/win32/win32.cpp` (kern), `kernel32.cpp`, `user32.cpp`, `gdi32.cpp`,
`misc.cpp` (CRT/ntdll/advapi32/shlwapi).

## Het idee: geen enkele echte DLL

Er wordt **geen Windows-code uitgevoerd**. Geen `kernel32.dll`, geen `ntdll.dll`,
geen syscalls. Elke geïmporteerde functie wordt in C++ herschreven zodat hij
hetzelfde *waarneembare* gedrag heeft. Dat heet High Level Emulation.

```
gast:  call qword [rip+0x1234]      ; IAT-slot voor WriteFile
       → RIP = 0x7FFE00000070

Cpu::step():
       isHleAddress(RIP)?  ja
       slot = (RIP - hleBase) / 16 = 7
       Win32::onHostCall(cpu, 7)
         → slots_[7].handler(emu, cpu)     ← C++-implementatie
         → RAX = resultaat
       RIP = pop64()                        ← terug naar de gast
```

Het HLE-gebied is 1 MiB gemapt als `r-x` en bevat alleen nullen. Er staan nooit
instructies in; het adres zelf is het signaal.

**Waarom dit werkt:** de gast weet niet of hij echte code aanroept. Hij zet
argumenten in RCX/RDX/R8/R9, doet `call`, en verwacht het resultaat in RAX. Zolang
wij ons aan die afspraak houden, is het niet te onderscheiden van het origineel.

## Argumenten lezen

```cpp
uint64_t apiArg(Cpu& cpu, int index);
```

- index 0–3 → RCX, RDX, R8, R9
- index ≥ 4 → `[rsp + 8 + index*8]`

Die `+8` is er omdat het returnadres nog op de stack staat als de host-functie
draait — het wordt pas ná de call gepopt. Stack-argument 5 (index 4) staat dus op
`[rsp+0x28]`, precies achter de 32 bytes shadow space.

Voor `double` (varargs `printf`) is er `apiArgDouble`: bij varargs staat de waarde
zowel in het integer- als in het XMM-register.

## Een functie toevoegen

Stel, het programma stopt met:

```
niet-geïmplementeerde Win32-functie: kernel32.dll!GetComputerNameA
```

Dan voeg je in `src/win32/kernel32.cpp` toe:

```cpp
reg("GetComputerNameA", [](Emulator& emu, Cpu& cpu) -> uint64_t {
    uint64_t buf = apiArg(cpu, 0);
    uint64_t sizePtr = apiArg(cpu, 1);
    std::string name = "MACEMU";
    if (buf) cpu.mem.writeCString(buf, name);
    if (sizePtr) cpu.mem.write32(sizePtr, static_cast<uint32_t>(name.size()));
    return 1; // TRUE
});
```

Herbouwen, opnieuw draaien, en je stuit op de volgende. Dat is de hele Fase 3-lus.
Zet in `src/pe/feasibility.cpp` de DLL op `DepClass::Implemented` zodra hij
werkelijk bruikbaar is, dan blijft het Fase 0-oordeel eerlijk.

## Wat er nu in zit (~140 functies)

**kernel32** — modules (`GetModuleHandle*`, `GetProcAddress`, `LoadLibrary*`),
heap (`HeapAlloc/Free/ReAlloc/Size`, `LocalAlloc`, `GlobalAlloc`), virtueel geheugen
(`VirtualAlloc/Free/Protect/Query`), bestanden en console (`CreateFile*`, `ReadFile`,
`WriteFile`, `WriteConsole*`, `GetStdHandle`, `GetFileType`), proces
(`ExitProcess`, `GetCurrentProcess*`, `GetCommandLine*`, `GetStartupInfo*`,
`GetEnvironmentStrings*`), tijd (`GetSystemTimeAsFileTime`, `GetTickCount64`,
`QueryPerformanceCounter/Frequency`, `Sleep`), synchronisatie (critical sections,
SRW-locks, events, `WaitForSingleObject`), TLS/FLS, tekencodering
(`MultiByteToWideChar`, `WideCharToMultiByte`), en diversen (`GetLastError`,
`IsProcessorFeaturePresent`, `GetSystemInfo`, `OutputDebugString*`).

**msvcrt / ucrtbase** — `malloc/calloc/realloc/free`, `memset/memcpy/memmove/memcmp`,
`strlen/strcmp/strcpy/strcat`, `printf/sprintf/snprintf/puts/putchar` (met een
eigen mini-printf die `%d %u %x %c %s %p %f` aankan), `exit`, `atexit`,
`_initterm(_e)` (die de CRT-initialisatiearray echt aanroept via `callGuest`).

**ntdll** — `RtlAllocateHeap/FreeHeap`, en de unwind-functies als no-op.

**advapi32** — registry-functies die netjes "niet gevonden" melden.

**user32 / gdi32** — zie [fase4-rendering.md](fase4-rendering.md).

## De heap

`GuestHeap` (in `win32.cpp`) is een first-fit allocator ín het gastgeheugen:
een `std::map` van blokken, splitsen bij allocatie, samenvoegen met beide buren bij
vrijgeven, en pagina's die pas gecommit worden als ze nodig zijn. 16-byte alignment.

Simpel, maar het klopt: `HeapSize` geeft de echte blokgrootte, `HeapReAlloc` kopieert
correct, en een dubbele `free` geeft `FALSE` in plaats van corruptie.

## Bewuste beperkingen

| Beperking | Gevolg | Waarom |
|---|---|---|
| Single-threaded | `CreateThread` draait de functie meteen synchroon uit | echte threads vragen een scheduler + geheugenmodel |
| Geen SEH | `RaiseException` en C++-`throw` zijn fataal | x64-unwinding is een deelproject op zich |
| Geen registry | apps vallen terug op standaardwaarden | een fake registry is te doen, maar nog niet nodig geweest |
| Ruwe padvertaling | `C:\x` wordt `./x` | genoeg voor tests; echte VFS is later werk |
| Geen security/ACL's | alles mag | irrelevant voor een leerproject |
| Geen resources | `LoadIcon`/`LoadString` geven vaste waarden | resource-directory parsen is de logische volgende stap |

`CreateThread` verdient een waarschuwing: hij logt expliciet dat hij synchroon
draait. Voor een applicatie die een worker-thread start en dan verdergaat is dat
verkeerd gedrag, geen benadering. Zodra je zo'n programma tegenkomt, is dat het
moment om echte threading te bouwen — niet eerder.

## Rapportage

Met `-v` (debug) print macemu bij het afsluiten hoe vaak elke Win32-functie is
aangeroepen. Dat is de snelste manier om te zien waar een applicatie zijn tijd aan
besteedt en welke stub je als volgende serieus moet nemen.
