# Fase 1 — De x86-64 CPU-core

Bestanden: `src/cpu/decoder.cpp`, `src/cpu/exec.cpp`, `src/cpu/exec_sse.cpp`,
`src/cpu/disasm.cpp`, `src/memory.cpp`. Test: `tests/test_cpu.cpp` (83 checks).

## Ontwerpkeuzes en waarom

| Keuze | Reden | Wat het kost |
|---|---|---|
| Interpreter, geen JIT | correctheid eerst; een JIT-bug is tien keer lastiger te vinden | ~10–50M instructies/s |
| Decode-struct + grote switch | leest als de Intel-manual, uitbreiden = `case` erbij | iets trager dan een sprongtabel |
| Eager flags | geen subtiele "flag was toch nog nodig"-bugs | elke ALU-instructie berekent 6 flags |
| Alleen long mode | een Win64-proces gebruikt geen ringen, segmentatie of paging | 32-bit vraagt extra werk |
| Sparse pagetabel | per-pagina rechten, foute toegang faalt hard en duidelijk | hash-lookup per toegang |

De laatste is belangrijker dan hij lijkt: doordat `.text` na het laden echt
alleen-uitvoerbaar is, krijg je bij een wilde pointer meteen
`geheugenfout: geen schrijfrecht op 0x...` in plaats van stille corruptie.

## Geheugenmodel

```cpp
class Memory {
  static constexpr uint64_t kPageSize = 0x1000;
  std::unordered_map<uint64_t, Page> pages_;  // 4 KiB per pagina, R/W/X-vlaggen
  std::map<uint64_t, Region> regions_;        // alleen voor rapportage
};
```

- toegangen binnen één pagina: één hash-lookup + `memcpy` (met een 1-entry cache),
- toegangen over een paginagrens: byte-voor-byte (correct, zelden),
- `fetch()` eist het X-recht en mag aan het eind van een pagina korter leveren.

## Decoder

`Cpu::decode()` verwerkt in volgorde: legacy prefixes → REX → opcode (1, 2 of 3
bytes) → ModRM → SIB → displacement → immediate. Het resultaat is een `Instr`
met alles al uitgerekend, inclusief de effectieve operandgrootte.

Twee dingen die makkelijk fout gaan en hier expliciet goed staan:

- **REX moet direct voor de opcode staan.** Een REX-byte eerder in de rij is geen
  REX maar een `inc`/`dec`-opcode uit 32-bit tijden.
- **32-bit schrijven wist de bovenste 32 bits.** `mov eax, 1` maakt van
  `rax = 0xFFFFFFFFFFFFFFFF` niet `0xFFFFFFFF00000001` maar `0x1`. Zie
  `Cpu::writeGpr()` en de test die daarop staat.

## Wat er is geïmplementeerd

**Geheel/data:** MOV (alle vormen incl. moffs), MOVZX/MOVSX/MOVSXD, LEA, XCHG,
PUSH/POP, PUSHFQ/POPFQ, CMOVcc, SETcc, CBW/CWDE/CDQE, CWD/CDQ/CQO, SAHF/LAHF, BSWAP.

**Rekenen:** ADD/OR/ADC/SBB/AND/SUB/XOR/CMP in alle zes vormen, INC/DEC, NEG/NOT,
MUL/IMUL (1-, 2- en 3-operand), DIV/IDIV (8/16/32/64-bit, met 128-bit tussenstap),
TEST.

**Bits:** SHL/SHR/SAR/ROL/ROR/RCL/RCR, SHLD/SHRD, BT/BTS/BTR/BTC, BSF/BSR,
TZCNT/LZCNT, POPCNT.

**Control flow:** Jcc (rel8 en rel32), JMP, CALL/RET (near, direct en indirect),
LOOP/LOOPE/LOOPNE/JRCXZ, LEAVE.

**Strings:** MOVS/STOS/LODS/SCAS/CMPS met REP/REPE/REPNE, DF-gestuurd.

**Atomics:** CMPXCHG, CMPXCHG16B, XADD, LOCK-prefix (single-threaded, dus geen no-op
met gevolgen).

**Systeem:** CPUID, RDTSC, XGETBV, fences, LDMXCSR/STMXCSR.

**SSE/SSE2** (`exec_sse.cpp`): MOVUPS/MOVAPS/MOVDQU/MOVDQA/MOVSS/MOVSD/MOVD/MOVQ,
XORPS/ANDPS/ANDNPS/ORPS, PXOR/PAND/PANDN/POR, PCMPEQB/W/D, PCMPGTB/W/D, PMOVMSKB,
MOVMSKPS/PD, PMINUB/PMAXUB, PADDB/W/D/Q, PSUBB/W/D/Q, PUNPCK*, PSHUFD/PSHUFLW/PSHUFHW,
SHUFPS/PD, PSLLDQ/PSRLDQ en de bit-shifts, ADDSD/SUBSD/MULSD/DIVSD/MINSD/MAXSD/SQRT
(scalar en packed, single en double), UCOMISS/UCOMISD, CVT-familie, CMPPS/SS/SD,
PSHUFB, PTEST, PCMPEQQ.

Die SSE2-set is geen luxe: de 64-bit MSVC-CRT doet `strlen`, `memset` en `memcmp`
met `pcmpeqb` + `pmovmskb`. Zonder die drie instructies kom je geen enkele echte
Windows-executable door het opstarten heen.

## Wat er bewust níét is

| Ontbreekt | Gevolg | Moeilijkheid om toe te voegen |
|---|---|---|
| x87-FPU | oude 32-bit code met `long double` crasht | middelmatig: 80-bit floats + stackmodel |
| AVX/AVX2/AVX-512 | geen — CPUID meldt ze niet, dus de gast kiest SSE2 | groot |
| SEH / exception unwinding | `RaiseException` en C++-`throw` zijn fataal | groot: `.pdata`, unwind-codes, filters |
| Echte threads | `CreateThread` draait de functie synchroon uit | groot: scheduling + geheugenmodel |
| Segmentatie/paging/ring 0 | geen — een Win64-proces gebruikt het niet | n.v.t. |

Dat CPUID geen AVX meldt is een bewuste beslissing: zou het dat wel doen, dan kiest
de CRT een AVX-pad dat de emulator niet aankan, en crasht hij op een plek die niets
met het probleem te maken heeft.

## Een instructie toevoegen

1. Draai met `--trace`; de foutmelding geeft opcode en adres.
2. Klopt de opcode niet, kijk dan eerst in `oneByteProps()`/`twoByteProps()` in
   `decoder.cpp`: staat de ModRM-vlag en de immediate-grootte goed?
3. Voeg een `case` toe in `Cpu::execute()` (of `Cpu::executeSse()`).
4. Schrijf een test in `tests/test_cpu.cpp` met de ruwe bytes.

De testopzet maakt dat laatste goedkoop:

```cpp
Machine m;
m.load({0xB8, 0x05, 0x00, 0x00, 0x00,   // mov eax, 5
        0x83, 0xC0, 0x01,               // add eax, 1
        0xF4});                         // hlt
m.run();
CHECK_EQ(m.cpu.gpr[RAX], 6ull);
```

## Callbacks: `Cpu::callGuest`

Dit is het stukje dat Fase 4 mogelijk maakt. De host moet soms midden in een
host-functie gastcode aanroepen (een WndProc bijvoorbeeld). `callGuest`:

1. bewaart alle registers,
2. zet de argumenten neer volgens Win64 (RCX/RDX/R8/R9, daarna de stack, altijd
   32 bytes shadow space),
3. pusht een sentinel-returnadres in het HLE-gebied,
4. draait de CPU tot RIP dat sentinel-adres bereikt,
5. zet de registers terug en geeft RAX terug.

Het geheugen wordt bewust *niet* teruggedraaid — de gast hoort zijn eigen
schrijfacties te zien.
