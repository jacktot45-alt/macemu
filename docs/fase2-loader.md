# Fase 2 — De PE-loader

Bestanden: `src/pe/pe.cpp` (parser), `src/pe/loader.cpp` (laden).
Tests: `tests/test_pe.cpp`, `tests/test_integration.cpp`.

Wat de Windows-loader doet, doet macemu ook — alleen dan het minimum dat nodig is
om bij het entry point aan te komen.

## De zeven stappen van `Emulator::load()`

### 1. Parsen en weigeren wat niet kan

```
DOS-header ("MZ") → e_lfanew → NT-signature ("PE\0\0") → COFF → optional header
```

Is het geen AMD64-binary, dan stopt het hier met een verwijzing naar
`macemu-peinfo`. Is het een .NET-assembly, ook — tenzij je `--force` gebruikt,
maar dan crasht het in `mscoree` en dat zegt de melding er ook bij.

### 2. Secties mappen

Elke sectie gaat op `imagebase + VirtualAddress`. Tijdens het laden staat alles op
lezen+schrijven, want er moet nog in geschreven worden (inhoud, relocations, IAT).

Twee dingen die je makkelijk over het hoofd ziet:

- **`VirtualSize` kan groter zijn dan `SizeOfRawData`.** Het verschil is `.bss`:
  nulgeheugen dat niet in het bestand staat. Onze `Memory::map()` levert al
  genulde pagina's, dus dat klopt vanzelf.
- **De headers moeten ook gemapt worden.** De CRT leest `PEB->ImageBaseAddress` en
  loopt daar vandaan de PE-header af.

### 3. Relocations

Alleen nodig als het image niet op zijn voorkeursbase past. Ondersteund:
`IMAGE_REL_BASED_DIR64` en `HIGHLOW`. Andere types geven een waarschuwing in plaats
van een crash, zodat je ziet wát er ontbreekt.

In de praktijk gebeurt dit zelden: bij het starten is er nog niets gemapt, dus de
voorkeursbase is altijd vrij.

### 4. Imports resolven

Hier gebeurt het interessante. Voor elke `(dll, functie)`:

```
Win32::resolveImport("kernel32.dll", "WriteFile")
  → HLE-slot 7
  → adres 0x7FFE00000070
  → dat adres in de IAT schrijven
```

De gast doet later `call qword [rip+iat_slot]` en springt naar
`0x7FFE00000070`. Dat is geen code — er staan nullen. `Cpu::step()` ziet dat RIP in
het HLE-gebied ligt en roept de C++-implementatie aan. Zie
[fase3-win32.md](fase3-win32.md).

DLL-namen worden eerst gecanonicaliseerd: `kernelbase.dll`, `api-ms-win-core-*` en
`ext-ms-win-*` wijzen allemaal naar `kernel32.dll`, `api-ms-win-crt-*` naar
`ucrtbase.dll`. Zonder dat zou je dezelfde functie tien keer moeten implementeren.

Ontbrekende functies zijn **geen fout** op dit moment. Ze krijgen een slot dat pas
klaagt als de gast er echt naartoe springt. Daarom laat `--dry-run` de complete
werklijst zien zonder iets te draaien.

### 5. Rechten definitief maken

Pas nu krijgt elke sectie zijn echte rechten: `.text` wordt `r-x`, `.rdata` wordt
`r--`. Twee uitzonderingen blijven schrijfbaar:

- de IAT (delay-load en `GetProcAddress`-patches schrijven daarin),
- de import-directory zelf.

In de geheugenkaart van `--dry-run` zie je `.rdata` daarom als `rw-` staan bij de
testprogramma's: de IAT vult daar de hele pagina.

### 6. Stack, TEB en PEB

```
0x00C000000000  ← stacktop (RSP start net eronder, 16-byte aligned)
      ...       ← 1 MiB stack, of SizeOfStackReserve als die groter is
0x00BFFFF00000  ← guard page (PROT_NONE): stack overflow faalt hard en duidelijk
```

De TEB komt op een vast adres en `GS` wijst ernaar — precies zoals op Windows, waar
`gs:[0x30]` de TEB-pointer is. Ingevuld: stackgrenzen, `Self`, ClientId,
`ThreadLocalStoragePointer`, `LastErrorValue`, en de TLS-slots op offset `0x1480`.

De PEB krijgt `ImageBaseAddress`, `ProcessHeap`, `NtGlobalFlag`, de OS-versie
(10.0.19045) en een `PEB_LDR_DATA` met lege, naar zichzelf wijzende lijsten — code
die de modulelijst afloopt stopt dan meteen in plaats van te crashen.

Verder een `RTL_USER_PROCESS_PARAMETERS` met de command line en het environment
block (UTF-16, dubbel-NUL afgesloten), zodat `GetCommandLineW` en
`GetEnvironmentStringsW` echte data teruggeven.

### 7. Springen

`RIP = imagebase + AddressOfEntryPoint`. Als returnadres wordt HLE-slot 0 gepusht:
een interne stub die het proces netjes afsluit als `main()` gewoon `ret` doet.

## Wat er nog niet is

| Ontbreekt | Gevolg |
|---|---|
| TLS-callbacks uitvoeren | wordt gedetecteerd en gemeld, maar nog niet aangeroepen |
| Echte DLL's laden | `LoadLibrary` geeft een nep-module met geldige MZ/PE-header |
| Forwarded exports (`OTHER.Func`) | worden geparsed, niet gevolgd |
| Bound imports | genegeerd (de IAT wordt toch overschreven) |
| Resources (icons, dialogen, strings) | niet uitgepakt |
| Delay-load-stubs | de imports worden direct gekoppeld i.p.v. bij eerste gebruik |

De resource-directory is waarschijnlijk de eerstvolgende die je nodig hebt zodra je
een echte applicatie draait: `LoadIcon`, `LoadString` en dialogen halen daar hun
data vandaan.
