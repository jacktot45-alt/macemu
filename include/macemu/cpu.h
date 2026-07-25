// macemu - cpu.h
//
// Fase 1: de x86-64 interpreter.
//
// Ontwerpkeuzes (bewust, met het oog op leerbaarheid boven snelheid):
//   * Eén decode-struct per instructie, daarna een grote switch. Dat leest als
//     de Intel-manual en is makkelijk uit te breiden.
//   * Flags worden direct ("eager") berekend. Lazy flags is de klassieke
//     optimalisatie, maar levert subtiele bugs op; die stap komt pas als de
//     correctheid staat.
//   * Alleen long mode (64-bit). Geen segmentatie behalve FS/GS-base, geen
//     paging, geen ring 0. Een Win64-proces gebruikt dat allemaal niet.
//   * Host-calls: imports wijzen naar een speciaal "HLE"-gebied. Zodra RIP daar
//     landt roept de CPU een C++-functie aan in plaats van instructies uit te
//     voeren. Zo hoeven we geen enkele echte Windows-DLL te emuleren.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "macemu/memory.h"

namespace macemu {

enum GpReg : int {
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15,
};

extern const char* const kReg64Names[16];
extern const char* const kReg32Names[16];
extern const char* const kReg16Names[16];
extern const char* const kReg8Names[16];   // met REX
extern const char* const kReg8LegacyNames[8]; // zonder REX (al/cl/../bh)

struct Xmm {
    uint64_t lo = 0;
    uint64_t hi = 0;
};

struct Flags {
    bool cf = false;
    bool pf = false;
    bool af = false;
    bool zf = false;
    bool sf = false;
    bool tf = false;
    bool if_ = true;
    bool df = false;
    bool of = false;

    uint64_t pack() const;
    void unpack(uint64_t v);
};

// Eén gedecodeerde instructie.
struct Instr {
    uint64_t rip = 0;
    uint8_t len = 0;

    // 1 byte : 0x00..0xFF
    // 2 bytes: 0x0F00 | b
    // 3 bytes: 0x3800 | b (0F 38 xx), 0x3A00 | b (0F 3A xx)
    uint16_t opcode = 0;

    bool hasModrm = false;
    uint8_t modrm = 0;
    uint8_t mod = 0;
    uint8_t regField = 0; // inclusief REX.R
    uint8_t rmField = 0;  // inclusief REX.B (alleen zinvol als rmIsReg)
    bool rmIsReg = false;

    // Geheugenoperand (geldig als hasModrm && !rmIsReg)
    int baseReg = -1;
    int indexReg = -1;
    uint8_t scale = 1;
    int64_t disp = 0;
    bool ripRelative = false;
    uint8_t segment = 0; // 0 = geen, 1 = FS, 2 = GS

    int64_t imm = 0;
    uint8_t immSize = 0;

    int opSize = 4;   // bytes: 1, 2, 4 of 8
    int addrSize = 8; // bytes: 4 of 8

    bool rex = false, rexW = false, rexR = false, rexX = false, rexB = false;
    bool p66 = false, pF2 = false, pF3 = false, lock = false;

    uint8_t bytes[16] = {0};
};

class Cpu;

// Callback-interface voor de Win32-laag: als RIP in het HLE-gebied landt.
class HostCallSink {
public:
    virtual ~HostCallSink() = default;
    // `id` is de index van de thunk. Return true als de CPU door mag gaan.
    virtual void onHostCall(Cpu& cpu, uint32_t id) = 0;
};

// Wordt gegooid als de interpreter een instructie niet kent. Bevat genoeg
// informatie om er direct een nieuwe case voor te schrijven.
class UnsupportedInstruction : public EmuError {
public:
    UnsupportedInstruction(const Instr& in, const std::string& detail);
    Instr instr;
};

class Cpu {
public:
    explicit Cpu(Memory& mem);

    // --- architecturale toestand ---
    uint64_t gpr[16] = {0};
    uint64_t rip = 0;
    Xmm xmm[16];
    Flags flags;
    uint64_t fsBase = 0;
    uint64_t gsBase = 0;
    uint32_t mxcsr = 0x1F80;
    uint16_t fpuControl = 0x037F;

    // --- emulator-toestand ---
    bool halted = false;
    uint64_t exitCode = 0;
    uint64_t instructionsExecuted = 0;
    bool traceEnabled = false;
    uint64_t traceLimit = 0;

    Memory& mem;

    // --- HLE-gebied (imports) ---
    void setHleRegion(uint64_t base, uint64_t size, HostCallSink* sink);
    uint64_t hleBase() const { return hleBase_; }
    static constexpr uint64_t kHleSlotSize = 16;
    uint64_t hleSlotAddress(uint32_t id) const { return hleBase_ + id * kHleSlotSize; }
    bool isHleAddress(uint64_t addr) const {
        return hleBase_ && addr >= hleBase_ && addr < hleBase_ + hleSize_;
    }

    // --- uitvoeren ---
    size_t decode(uint64_t addr, Instr& out) const;
    void execute(const Instr& in);
    void step();
    // Draait tot halted of tot maxInstructions (0 = oneindig).
    void run(uint64_t maxInstructions = 0);

    // Roept een functie in de gast aan volgens de Win64-conventie en keert
    // terug met de waarde uit RAX. Wordt gebruikt als de host de gast moet
    // aanroepen (bijv. een WndProc vanuit DispatchMessage).
    uint64_t callGuest(uint64_t addr, const std::vector<uint64_t>& args,
                       uint64_t maxInstructions = 200000000ull);

    // --- stack helpers ---
    void push64(uint64_t v);
    uint64_t pop64();

    // --- register toegang ---
    uint64_t readGpr(int reg, int size) const;
    void writeGpr(int reg, int size, uint64_t value);
    uint64_t readReg8(int reg, bool hasRex) const;
    void writeReg8(int reg, bool hasRex, uint8_t value);

    // --- operand helpers (publiek zodat exec_sse.cpp ze kan gebruiken) ---
    uint64_t effectiveAddress(const Instr& in) const;
    uint64_t readRm(const Instr& in, int size) const;
    void writeRm(const Instr& in, int size, uint64_t value);
    uint64_t readRegOperand(const Instr& in, int size) const;
    void writeRegOperand(const Instr& in, int size, uint64_t value);
    Xmm readXmmRm(const Instr& in) const;
    void writeXmmRm(const Instr& in, const Xmm& v);

    // --- flags ---
    void setLogicFlags(uint64_t result, int size);
    void setAddFlags(uint64_t a, uint64_t b, uint64_t result, int size, uint64_t carryIn = 0);
    void setSubFlags(uint64_t a, uint64_t b, uint64_t result, int size, uint64_t borrowIn = 0);
    void setIncFlags(uint64_t a, uint64_t result, int size);
    void setDecFlags(uint64_t a, uint64_t result, int size);
    bool testCondition(uint8_t cc) const;

    std::string dumpState() const;
    std::string dumpStack(int slots = 8) const;

    // Uitvoerders voor de SSE/SSE2-subset (exec_sse.cpp).
    bool executeSse(const Instr& in);

private:
    void executeTwoByte(const Instr& in);
    void executeGroup(const Instr& in);
    void executeString(const Instr& in);
    bool executeStringOnce(const Instr& in);

    uint64_t hleBase_ = 0;
    uint64_t hleSize_ = 0;
    HostCallSink* hleSink_ = nullptr;

    // Sentinel-adres waar callGuest naartoe "returnt".
    uint64_t returnSentinel_ = 0;
    friend class Emulator;
};

// Hulpfuncties voor sign/zero-extensie.
inline uint64_t maskFor(int size) {
    switch (size) {
        case 1: return 0xFFull;
        case 2: return 0xFFFFull;
        case 4: return 0xFFFFFFFFull;
        default: return ~0ull;
    }
}

inline int64_t signExtend(uint64_t v, int size) {
    switch (size) {
        case 1: return static_cast<int8_t>(v);
        case 2: return static_cast<int16_t>(v);
        case 4: return static_cast<int32_t>(v);
        default: return static_cast<int64_t>(v);
    }
}

// Eenvoudige disassembler, alleen voor tracing/debuggen.
std::string disassemble(const Instr& in);

} // namespace macemu
