// macemu - x86-64 interpreter (Fase 1)
//
// Eén grote switch over de opcode. Bewust letterlijk gehouden ten opzichte van
// de Intel-manual: als je een instructie mist, voeg je hier een case toe en
// weet je precies wat er gebeurt.
#include <cstring>

#include "macemu/cpu.h"

namespace macemu {

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------
uint64_t Flags::pack() const {
    uint64_t v = 0x2; // bit 1 is altijd 1
    if (cf) v |= 1ull << 0;
    if (pf) v |= 1ull << 2;
    if (af) v |= 1ull << 4;
    if (zf) v |= 1ull << 6;
    if (sf) v |= 1ull << 7;
    if (tf) v |= 1ull << 8;
    if (if_) v |= 1ull << 9;
    if (df) v |= 1ull << 10;
    if (of) v |= 1ull << 11;
    return v;
}

void Flags::unpack(uint64_t v) {
    cf = (v >> 0) & 1;
    pf = (v >> 2) & 1;
    af = (v >> 4) & 1;
    zf = (v >> 6) & 1;
    sf = (v >> 7) & 1;
    tf = (v >> 8) & 1;
    if_ = (v >> 9) & 1;
    df = (v >> 10) & 1;
    of = (v >> 11) & 1;
}

namespace {

bool parityOf(uint64_t v) {
    uint8_t b = static_cast<uint8_t>(v);
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return (b & 1) == 0;
}

inline uint64_t msbMask(int size) { return 1ull << (size * 8 - 1); }

} // namespace

// ---------------------------------------------------------------------------
// Cpu basis
// ---------------------------------------------------------------------------
Cpu::Cpu(Memory& m) : mem(m) {}

void Cpu::setHleRegion(uint64_t base, uint64_t size, HostCallSink* sink) {
    hleBase_ = base;
    hleSize_ = size;
    hleSink_ = sink;
}

uint64_t Cpu::readGpr(int reg, int size) const {
    uint64_t v = gpr[reg];
    switch (size) {
        case 1: return v & 0xFF;
        case 2: return v & 0xFFFF;
        case 4: return v & 0xFFFFFFFF;
        default: return v;
    }
}

void Cpu::writeGpr(int reg, int size, uint64_t value) {
    switch (size) {
        case 1: gpr[reg] = (gpr[reg] & ~0xFFull) | (value & 0xFF); break;
        case 2: gpr[reg] = (gpr[reg] & ~0xFFFFull) | (value & 0xFFFF); break;
        // 32-bit schrijven wist de bovenste 32 bits - dat is x86-64-gedrag en
        // een klassieke bron van bugs in emulators.
        case 4: gpr[reg] = value & 0xFFFFFFFFull; break;
        default: gpr[reg] = value; break;
    }
}

uint64_t Cpu::readReg8(int reg, bool hasRex) const {
    if (!hasRex && reg >= 4 && reg < 8) return (gpr[reg - 4] >> 8) & 0xFF; // ah/ch/dh/bh
    return gpr[reg] & 0xFF;
}

void Cpu::writeReg8(int reg, bool hasRex, uint8_t value) {
    if (!hasRex && reg >= 4 && reg < 8) {
        gpr[reg - 4] = (gpr[reg - 4] & ~0xFF00ull) | (static_cast<uint64_t>(value) << 8);
        return;
    }
    gpr[reg] = (gpr[reg] & ~0xFFull) | value;
}

uint64_t Cpu::readRm(const Instr& in, int size) const {
    if (in.rmIsReg) {
        if (size == 1) return readReg8(in.rmField, in.rex);
        return readGpr(in.rmField, size);
    }
    uint64_t ea = effectiveAddress(in);
    switch (size) {
        case 1: return mem.read8(ea);
        case 2: return mem.read16(ea);
        case 4: return mem.read32(ea);
        default: return mem.read64(ea);
    }
}

void Cpu::writeRm(const Instr& in, int size, uint64_t value) {
    if (in.rmIsReg) {
        if (size == 1) writeReg8(in.rmField, in.rex, static_cast<uint8_t>(value));
        else writeGpr(in.rmField, size, value);
        return;
    }
    uint64_t ea = effectiveAddress(in);
    switch (size) {
        case 1: mem.write8(ea, static_cast<uint8_t>(value)); break;
        case 2: mem.write16(ea, static_cast<uint16_t>(value)); break;
        case 4: mem.write32(ea, static_cast<uint32_t>(value)); break;
        default: mem.write64(ea, value); break;
    }
}

uint64_t Cpu::readRegOperand(const Instr& in, int size) const {
    if (size == 1) return readReg8(in.regField, in.rex);
    return readGpr(in.regField, size);
}

void Cpu::writeRegOperand(const Instr& in, int size, uint64_t value) {
    if (size == 1) writeReg8(in.regField, in.rex, static_cast<uint8_t>(value));
    else writeGpr(in.regField, size, value);
}

void Cpu::push64(uint64_t v) {
    gpr[RSP] -= 8;
    mem.write64(gpr[RSP], v);
}

uint64_t Cpu::pop64() {
    uint64_t v = mem.read64(gpr[RSP]);
    gpr[RSP] += 8;
    return v;
}

// ---------------------------------------------------------------------------
// Flag-berekening
// ---------------------------------------------------------------------------
void Cpu::setLogicFlags(uint64_t result, int size) {
    uint64_t m = maskFor(size);
    result &= m;
    flags.cf = false;
    flags.of = false;
    flags.zf = (result == 0);
    flags.sf = (result & msbMask(size)) != 0;
    flags.pf = parityOf(result);
    flags.af = false;
}

void Cpu::setAddFlags(uint64_t a, uint64_t b, uint64_t result, int size, uint64_t carryIn) {
    uint64_t m = maskFor(size);
    a &= m; b &= m;
    uint64_t r = result & m;
    flags.zf = (r == 0);
    flags.sf = (r & msbMask(size)) != 0;
    flags.pf = parityOf(r);
    flags.af = (((a ^ b ^ r) & 0x10) != 0);
    if (size == 8) {
        flags.cf = (r < a) || (carryIn && r == a);
    } else {
        flags.cf = ((a + b + carryIn) & ~m) != 0;
    }
    uint64_t sign = msbMask(size);
    flags.of = (((a ^ r) & (b ^ r) & sign) != 0);
}

void Cpu::setSubFlags(uint64_t a, uint64_t b, uint64_t result, int size, uint64_t borrowIn) {
    uint64_t m = maskFor(size);
    a &= m; b &= m;
    uint64_t r = result & m;
    flags.zf = (r == 0);
    flags.sf = (r & msbMask(size)) != 0;
    flags.pf = parityOf(r);
    flags.af = (((a ^ b ^ r) & 0x10) != 0);
    flags.cf = (a < b) || (borrowIn && a == b);
    uint64_t sign = msbMask(size);
    flags.of = (((a ^ b) & (a ^ r) & sign) != 0);
}

void Cpu::setIncFlags(uint64_t a, uint64_t result, int size) {
    bool savedCf = flags.cf; // INC laat CF ongemoeid
    setAddFlags(a, 1, result, size);
    flags.cf = savedCf;
}

void Cpu::setDecFlags(uint64_t a, uint64_t result, int size) {
    bool savedCf = flags.cf;
    setSubFlags(a, 1, result, size);
    flags.cf = savedCf;
}

bool Cpu::testCondition(uint8_t cc) const {
    switch (cc & 0xF) {
        case 0x0: return flags.of;                       // O
        case 0x1: return !flags.of;                      // NO
        case 0x2: return flags.cf;                       // B/C/NAE
        case 0x3: return !flags.cf;                      // AE/NB/NC
        case 0x4: return flags.zf;                       // E/Z
        case 0x5: return !flags.zf;                      // NE/NZ
        case 0x6: return flags.cf || flags.zf;           // BE/NA
        case 0x7: return !flags.cf && !flags.zf;         // A/NBE
        case 0x8: return flags.sf;                       // S
        case 0x9: return !flags.sf;                      // NS
        case 0xA: return flags.pf;                       // P/PE
        case 0xB: return !flags.pf;                      // NP/PO
        case 0xC: return flags.sf != flags.of;           // L/NGE
        case 0xD: return flags.sf == flags.of;           // GE/NL
        case 0xE: return flags.zf || (flags.sf != flags.of); // LE/NG
        default:  return !flags.zf && (flags.sf == flags.of); // G/NLE
    }
}

// ---------------------------------------------------------------------------
// ALU-kern (gedeeld door de 00..3F-opcodes en group 1)
// ---------------------------------------------------------------------------
namespace {
enum AluOp { ALU_ADD = 0, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };
}

static uint64_t applyAlu(Cpu& cpu, int which, uint64_t a, uint64_t b, int size, bool& store) {
    uint64_t m = maskFor(size);
    uint64_t r = 0;
    store = true;
    switch (which) {
        case ALU_ADD:
            r = (a + b) & m;
            cpu.setAddFlags(a, b, r, size);
            break;
        case ALU_OR:
            r = (a | b) & m;
            cpu.setLogicFlags(r, size);
            break;
        case ALU_ADC: {
            uint64_t c = cpu.flags.cf ? 1 : 0;
            r = (a + b + c) & m;
            cpu.setAddFlags(a, b, r, size, c);
            break;
        }
        case ALU_SBB: {
            uint64_t c = cpu.flags.cf ? 1 : 0;
            r = (a - b - c) & m;
            cpu.setSubFlags(a, b, r, size, c);
            break;
        }
        case ALU_AND:
            r = (a & b) & m;
            cpu.setLogicFlags(r, size);
            break;
        case ALU_SUB:
            r = (a - b) & m;
            cpu.setSubFlags(a, b, r, size);
            break;
        case ALU_XOR:
            r = (a ^ b) & m;
            cpu.setLogicFlags(r, size);
            break;
        case ALU_CMP:
            r = (a - b) & m;
            cpu.setSubFlags(a, b, r, size);
            store = false;
            break;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Shift/rotate (group 2)
// ---------------------------------------------------------------------------
static uint64_t applyShift(Cpu& cpu, int which, uint64_t v, uint64_t count, int size) {
    uint64_t m = maskFor(size);
    int bits = size * 8;
    count &= (size == 8) ? 0x3F : 0x1F;
    if (count == 0) return v & m;
    v &= m;
    uint64_t r = v;

    switch (which) {
        case 0: { // ROL
            uint64_t n = count % bits;
            if (n) r = ((v << n) | (v >> (bits - n))) & m;
            cpu.flags.cf = r & 1;
            if (count == 1) cpu.flags.of = ((r & msbMask(size)) != 0) != cpu.flags.cf;
            break;
        }
        case 1: { // ROR
            uint64_t n = count % bits;
            if (n) r = ((v >> n) | (v << (bits - n))) & m;
            cpu.flags.cf = (r & msbMask(size)) != 0;
            if (count == 1) {
                bool b1 = (r & msbMask(size)) != 0;
                bool b2 = (r & (msbMask(size) >> 1)) != 0;
                cpu.flags.of = b1 != b2;
            }
            break;
        }
        case 2: { // RCL
            uint64_t n = count % (bits + 1);
            for (uint64_t i = 0; i < n; ++i) {
                bool msb = (r & msbMask(size)) != 0;
                r = ((r << 1) | (cpu.flags.cf ? 1 : 0)) & m;
                cpu.flags.cf = msb;
            }
            if (count == 1) cpu.flags.of = ((r & msbMask(size)) != 0) != cpu.flags.cf;
            break;
        }
        case 3: { // RCR
            uint64_t n = count % (bits + 1);
            if (count == 1) cpu.flags.of = ((r & msbMask(size)) != 0) != cpu.flags.cf;
            for (uint64_t i = 0; i < n; ++i) {
                bool lsb = r & 1;
                r = (r >> 1) | (cpu.flags.cf ? msbMask(size) : 0);
                cpu.flags.cf = lsb;
            }
            break;
        }
        case 4:
        case 6: { // SHL / SAL
            if (count <= static_cast<uint64_t>(bits))
                cpu.flags.cf = (v >> (bits - count)) & 1;
            else
                cpu.flags.cf = false;
            r = (count >= static_cast<uint64_t>(bits)) ? 0 : ((v << count) & m);
            if (count == 1) cpu.flags.of = (((r & msbMask(size)) != 0) != cpu.flags.cf);
            cpu.flags.zf = (r == 0);
            cpu.flags.sf = (r & msbMask(size)) != 0;
            cpu.flags.pf = ((__builtin_popcount(static_cast<uint8_t>(r)) & 1) == 0);
            break;
        }
        case 5: { // SHR
            cpu.flags.cf = (count <= static_cast<uint64_t>(bits)) ? ((v >> (count - 1)) & 1) : 0;
            r = (count >= static_cast<uint64_t>(bits)) ? 0 : (v >> count);
            if (count == 1) cpu.flags.of = (v & msbMask(size)) != 0;
            cpu.flags.zf = (r == 0);
            cpu.flags.sf = (r & msbMask(size)) != 0;
            cpu.flags.pf = ((__builtin_popcount(static_cast<uint8_t>(r)) & 1) == 0);
            break;
        }
        case 7: { // SAR
            int64_t sv = signExtend(v, size);
            uint64_t n = count;
            if (n >= static_cast<uint64_t>(bits)) n = bits - 1;
            cpu.flags.cf = (sv >> (n ? (n - 1) : 0)) & 1;
            if (count >= static_cast<uint64_t>(bits)) {
                r = (sv < 0) ? m : 0;
                cpu.flags.cf = (sv < 0);
            } else {
                r = static_cast<uint64_t>(sv >> count) & m;
            }
            if (count == 1) cpu.flags.of = false;
            cpu.flags.zf = (r == 0);
            cpu.flags.sf = (r & msbMask(size)) != 0;
            cpu.flags.pf = ((__builtin_popcount(static_cast<uint8_t>(r)) & 1) == 0);
            break;
        }
    }
    return r & m;
}

// ---------------------------------------------------------------------------
// String-instructies (MOVS/STOS/LODS/SCAS/CMPS) met REP-prefix
// ---------------------------------------------------------------------------
bool Cpu::executeStringOnce(const Instr& in) {
    int size = (in.opcode & 1) ? in.opSize : 1;
    int64_t delta = flags.df ? -size : size;

    switch (in.opcode) {
        case 0xA4: case 0xA5: { // MOVS
            uint64_t v = (size == 1)   ? mem.read8(gpr[RSI])
                         : (size == 2) ? mem.read16(gpr[RSI])
                         : (size == 4) ? mem.read32(gpr[RSI])
                                       : mem.read64(gpr[RSI]);
            if (size == 1) mem.write8(gpr[RDI], static_cast<uint8_t>(v));
            else if (size == 2) mem.write16(gpr[RDI], static_cast<uint16_t>(v));
            else if (size == 4) mem.write32(gpr[RDI], static_cast<uint32_t>(v));
            else mem.write64(gpr[RDI], v);
            gpr[RSI] += delta;
            gpr[RDI] += delta;
            return true;
        }
        case 0xAA: case 0xAB: { // STOS
            uint64_t v = readGpr(RAX, size);
            if (size == 1) mem.write8(gpr[RDI], static_cast<uint8_t>(v));
            else if (size == 2) mem.write16(gpr[RDI], static_cast<uint16_t>(v));
            else if (size == 4) mem.write32(gpr[RDI], static_cast<uint32_t>(v));
            else mem.write64(gpr[RDI], v);
            gpr[RDI] += delta;
            return true;
        }
        case 0xAC: case 0xAD: { // LODS
            uint64_t v = (size == 1)   ? mem.read8(gpr[RSI])
                         : (size == 2) ? mem.read16(gpr[RSI])
                         : (size == 4) ? mem.read32(gpr[RSI])
                                       : mem.read64(gpr[RSI]);
            writeGpr(RAX, size, v);
            gpr[RSI] += delta;
            return true;
        }
        case 0xAE: case 0xAF: { // SCAS
            uint64_t a = readGpr(RAX, size);
            uint64_t b = (size == 1)   ? mem.read8(gpr[RDI])
                         : (size == 2) ? mem.read16(gpr[RDI])
                         : (size == 4) ? mem.read32(gpr[RDI])
                                       : mem.read64(gpr[RDI]);
            setSubFlags(a, b, (a - b) & maskFor(size), size);
            gpr[RDI] += delta;
            return true;
        }
        case 0xA6: case 0xA7: { // CMPS
            uint64_t a = (size == 1)   ? mem.read8(gpr[RSI])
                         : (size == 2) ? mem.read16(gpr[RSI])
                         : (size == 4) ? mem.read32(gpr[RSI])
                                       : mem.read64(gpr[RSI]);
            uint64_t b = (size == 1)   ? mem.read8(gpr[RDI])
                         : (size == 2) ? mem.read16(gpr[RDI])
                         : (size == 4) ? mem.read32(gpr[RDI])
                                       : mem.read64(gpr[RDI]);
            setSubFlags(a, b, (a - b) & maskFor(size), size);
            gpr[RSI] += delta;
            gpr[RDI] += delta;
            return true;
        }
        default:
            return false;
    }
}

void Cpu::executeString(const Instr& in) {
    bool usesFlags = (in.opcode >= 0xA6 && in.opcode <= 0xA7) ||
                     (in.opcode >= 0xAE && in.opcode <= 0xAF);

    if (!in.pF3 && !in.pF2) {
        executeStringOnce(in);
        return;
    }
    // REP/REPE/REPNE. We voeren de hele lus in één "stap" uit; dat is
    // observationeel gelijk zolang er geen interrupts zijn (die hebben we niet).
    while (gpr[RCX] != 0) {
        executeStringOnce(in);
        gpr[RCX]--;
        if (usesFlags) {
            if (in.pF3 && !flags.zf) break;   // REPE  : stop bij ZF=0
            if (in.pF2 && flags.zf) break;    // REPNE : stop bij ZF=1
        }
    }
}

// ---------------------------------------------------------------------------
// De hoofdswitch
// ---------------------------------------------------------------------------
void Cpu::execute(const Instr& in) {
    const int sz = in.opSize;
    uint64_t nextRip = in.rip + in.len;
    rip = nextRip;

    uint16_t op = in.opcode;

    // --- 00..3D: de acht ALU-operaties in zes vormen ---
    // Het patroon is regelmatig: bits 5..3 kiezen de operatie (ADD..CMP),
    // bits 2..0 de vorm. Vormen 6 en 7 bestaan niet in 64-bit mode.
    if (op < 0x40 && (op & 7) <= 5) {
        int which = (op >> 3) & 7;
        int form = op & 7;
        bool store;
        switch (form) {
            case 0: { // r/m8, r8
                uint64_t a = readRm(in, 1), b = readReg8(in.regField, in.rex);
                uint64_t r = applyAlu(*this, which, a, b, 1, store);
                if (store) writeRm(in, 1, r);
                return;
            }
            case 1: { // r/m, r
                uint64_t a = readRm(in, sz), b = readGpr(in.regField, sz);
                uint64_t r = applyAlu(*this, which, a, b, sz, store);
                if (store) writeRm(in, sz, r);
                return;
            }
            case 2: { // r8, r/m8
                uint64_t a = readReg8(in.regField, in.rex), b = readRm(in, 1);
                uint64_t r = applyAlu(*this, which, a, b, 1, store);
                if (store) writeReg8(in.regField, in.rex, static_cast<uint8_t>(r));
                return;
            }
            case 3: { // r, r/m
                uint64_t a = readGpr(in.regField, sz), b = readRm(in, sz);
                uint64_t r = applyAlu(*this, which, a, b, sz, store);
                if (store) writeGpr(in.regField, sz, r);
                return;
            }
            case 4: { // AL, imm8
                uint64_t a = readGpr(RAX, 1), b = static_cast<uint64_t>(in.imm) & 0xFF;
                uint64_t r = applyAlu(*this, which, a, b, 1, store);
                if (store) writeGpr(RAX, 1, r);
                return;
            }
            default: { // eAX, immz
                uint64_t a = readGpr(RAX, sz), b = static_cast<uint64_t>(in.imm) & maskFor(sz);
                uint64_t r = applyAlu(*this, which, a, b, sz, store);
                if (store) writeGpr(RAX, sz, r);
                return;
            }
        }
    }

    switch (op) {
        // ---------------- PUSH / POP ----------------
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            push64(gpr[(op - 0x50) | (in.rexB ? 8 : 0)]);
            return;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            gpr[(op - 0x58) | (in.rexB ? 8 : 0)] = pop64();
            return;
        case 0x68: case 0x6A:
            push64(static_cast<uint64_t>(in.imm));
            return;
        case 0x8F: // POP r/m64
            writeRm(in, 8, pop64());
            return;
        case 0x9C: // PUSHFQ
            push64(flags.pack());
            return;
        case 0x9D: // POPFQ
            flags.unpack(pop64());
            return;

        // ---------------- MOV ----------------
        case 0x88: writeRm(in, 1, readReg8(in.regField, in.rex)); return;
        case 0x89: writeRm(in, sz, readGpr(in.regField, sz)); return;
        case 0x8A: writeReg8(in.regField, in.rex, static_cast<uint8_t>(readRm(in, 1))); return;
        case 0x8B: writeGpr(in.regField, sz, readRm(in, sz)); return;
        case 0xC6: writeRm(in, 1, static_cast<uint64_t>(in.imm) & 0xFF); return;
        case 0xC7: writeRm(in, sz, static_cast<uint64_t>(in.imm) & maskFor(sz)); return;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            writeReg8((op - 0xB0) | (in.rexB ? 8 : 0), in.rex, static_cast<uint8_t>(in.imm));
            return;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            writeGpr((op - 0xB8) | (in.rexB ? 8 : 0), sz,
                     static_cast<uint64_t>(in.imm) & maskFor(sz));
            return;
        case 0xA0: writeGpr(RAX, 1, mem.read8(static_cast<uint64_t>(in.imm))); return;
        case 0xA1: writeGpr(RAX, sz,
                            sz == 8 ? mem.read64(static_cast<uint64_t>(in.imm))
                                    : sz == 4 ? mem.read32(static_cast<uint64_t>(in.imm))
                                              : mem.read16(static_cast<uint64_t>(in.imm)));
            return;
        case 0xA2: mem.write8(static_cast<uint64_t>(in.imm), static_cast<uint8_t>(gpr[RAX]));
            return;
        case 0xA3:
            if (sz == 8) mem.write64(static_cast<uint64_t>(in.imm), gpr[RAX]);
            else if (sz == 4) mem.write32(static_cast<uint64_t>(in.imm), static_cast<uint32_t>(gpr[RAX]));
            else mem.write16(static_cast<uint64_t>(in.imm), static_cast<uint16_t>(gpr[RAX]));
            return;

        case 0x63: { // MOVSXD r64, r/m32
            uint64_t v = readRm(in, 4);
            writeGpr(in.regField, sz, static_cast<uint64_t>(static_cast<int64_t>(
                                          static_cast<int32_t>(static_cast<uint32_t>(v)))));
            return;
        }
        case 0x8D: { // LEA
            uint64_t ea;
            if (in.rmIsReg) throw UnsupportedInstruction(in, "LEA met register-operand");
            // LEA negeert segment-prefixes.
            Instr tmp = in;
            tmp.segment = 0;
            ea = effectiveAddress(tmp);
            writeGpr(in.regField, sz, ea & maskFor(sz));
            return;
        }

        // ---------------- XCHG / TEST ----------------
        case 0x86: {
            uint64_t a = readRm(in, 1), b = readReg8(in.regField, in.rex);
            writeRm(in, 1, b);
            writeReg8(in.regField, in.rex, static_cast<uint8_t>(a));
            return;
        }
        case 0x87: {
            uint64_t a = readRm(in, sz), b = readGpr(in.regField, sz);
            writeRm(in, sz, b);
            writeGpr(in.regField, sz, a);
            return;
        }
        case 0x84: setLogicFlags(readRm(in, 1) & readReg8(in.regField, in.rex), 1); return;
        case 0x85: setLogicFlags(readRm(in, sz) & readGpr(in.regField, sz), sz); return;
        case 0xA8: setLogicFlags(readGpr(RAX, 1) & (static_cast<uint64_t>(in.imm) & 0xFF), 1);
            return;
        case 0xA9: setLogicFlags(readGpr(RAX, sz) & (static_cast<uint64_t>(in.imm) & maskFor(sz)),
                                 sz);
            return;

        case 0x90: // NOP of XCHG rAX, r8-r15
            if (in.rexB) {
                uint64_t t = gpr[RAX];
                gpr[RAX] = gpr[R8];
                gpr[R8] = t;
            }
            return;
        case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97: {
            int r = (op - 0x90) | (in.rexB ? 8 : 0);
            uint64_t a = readGpr(RAX, sz), b = readGpr(r, sz);
            writeGpr(RAX, sz, b);
            writeGpr(r, sz, a);
            return;
        }

        // ---------------- Group 1: ALU met immediate ----------------
        case 0x80: case 0x81: case 0x83: {
            int which = (in.modrm >> 3) & 7;
            int size = (op == 0x80) ? 1 : sz;
            uint64_t a = readRm(in, size);
            uint64_t b = static_cast<uint64_t>(in.imm) & maskFor(size);
            bool store;
            uint64_t r = applyAlu(*this, which, a, b, size, store);
            if (store) writeRm(in, size, r);
            return;
        }

        // ---------------- Group 2: shifts ----------------
        case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            int which = (in.modrm >> 3) & 7;
            int size = (op == 0xC0 || op == 0xD0 || op == 0xD2) ? 1 : sz;
            uint64_t count;
            if (op == 0xC0 || op == 0xC1) count = static_cast<uint64_t>(in.imm) & 0xFF;
            else if (op == 0xD0 || op == 0xD1) count = 1;
            else count = gpr[RCX] & 0xFF;
            uint64_t v = readRm(in, size);
            uint64_t r = applyShift(*this, which, v, count, size);
            writeRm(in, size, r);
            return;
        }

        // ---------------- Group 3 ----------------
        case 0xF6: case 0xF7: {
            int size = (op == 0xF6) ? 1 : sz;
            int sub = (in.modrm >> 3) & 7;
            switch (sub) {
                case 0: case 1: // TEST r/m, imm
                    setLogicFlags(readRm(in, size) & (static_cast<uint64_t>(in.imm) & maskFor(size)),
                                  size);
                    return;
                case 2: // NOT
                    writeRm(in, size, ~readRm(in, size) & maskFor(size));
                    return;
                case 3: { // NEG
                    uint64_t a = readRm(in, size);
                    uint64_t r = (0 - a) & maskFor(size);
                    setSubFlags(0, a, r, size);
                    flags.cf = (a != 0);
                    writeRm(in, size, r);
                    return;
                }
                case 4: { // MUL (unsigned)
                    uint64_t a = readGpr(RAX, size), b = readRm(in, size);
                    if (size == 1) {
                        uint16_t r = static_cast<uint16_t>(a * b);
                        writeGpr(RAX, 2, r);
                        flags.cf = flags.of = ((r >> 8) != 0);
                    } else if (size == 2) {
                        uint32_t r = static_cast<uint32_t>(a * b);
                        writeGpr(RAX, 2, r & 0xFFFF);
                        writeGpr(RDX, 2, r >> 16);
                        flags.cf = flags.of = ((r >> 16) != 0);
                    } else if (size == 4) {
                        uint64_t r = a * b;
                        writeGpr(RAX, 4, r & 0xFFFFFFFF);
                        writeGpr(RDX, 4, r >> 32);
                        flags.cf = flags.of = ((r >> 32) != 0);
                    } else {
                        unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
                        gpr[RAX] = static_cast<uint64_t>(r);
                        gpr[RDX] = static_cast<uint64_t>(r >> 64);
                        flags.cf = flags.of = (gpr[RDX] != 0);
                    }
                    return;
                }
                case 5: { // IMUL (signed)
                    int64_t a = signExtend(readGpr(RAX, size), size);
                    int64_t b = signExtend(readRm(in, size), size);
                    if (size == 1) {
                        int16_t r = static_cast<int16_t>(a * b);
                        writeGpr(RAX, 2, static_cast<uint16_t>(r));
                        flags.cf = flags.of = (r != static_cast<int8_t>(r));
                    } else if (size == 2) {
                        int32_t r = static_cast<int32_t>(a * b);
                        writeGpr(RAX, 2, static_cast<uint16_t>(r));
                        writeGpr(RDX, 2, static_cast<uint16_t>(r >> 16));
                        flags.cf = flags.of = (r != static_cast<int16_t>(r));
                    } else if (size == 4) {
                        int64_t r = a * b;
                        writeGpr(RAX, 4, static_cast<uint32_t>(r));
                        writeGpr(RDX, 4, static_cast<uint32_t>(r >> 32));
                        flags.cf = flags.of = (r != static_cast<int32_t>(r));
                    } else {
                        __int128 r = static_cast<__int128>(a) * b;
                        gpr[RAX] = static_cast<uint64_t>(r);
                        gpr[RDX] = static_cast<uint64_t>(r >> 64);
                        flags.cf = flags.of = (r != static_cast<__int128>(static_cast<int64_t>(r)));
                    }
                    return;
                }
                case 6: { // DIV
                    uint64_t d = readRm(in, size);
                    if (d == 0) throw EmuError("deling door nul (DIV)");
                    if (size == 1) {
                        uint16_t n = static_cast<uint16_t>(readGpr(RAX, 2));
                        uint64_t q = (n / d) & 0xFF, r = (n % d) & 0xFF;
                        gpr[RAX] = (gpr[RAX] & ~0xFFFFull) | q | (r << 8);
                    } else if (size == 2) {
                        uint32_t n = (static_cast<uint32_t>(readGpr(RDX, 2)) << 16) |
                                     static_cast<uint32_t>(readGpr(RAX, 2));
                        writeGpr(RAX, 2, (n / d) & 0xFFFF);
                        writeGpr(RDX, 2, (n % d) & 0xFFFF);
                    } else if (size == 4) {
                        uint64_t n = (readGpr(RDX, 4) << 32) | readGpr(RAX, 4);
                        uint64_t q = n / d;
                        if (q > 0xFFFFFFFFull) throw EmuError("DIV overflow");
                        writeGpr(RAX, 4, q);
                        writeGpr(RDX, 4, n % d);
                    } else {
                        unsigned __int128 n =
                            (static_cast<unsigned __int128>(gpr[RDX]) << 64) | gpr[RAX];
                        unsigned __int128 q = n / d;
                        if (q > ~0ull) throw EmuError("DIV overflow");
                        gpr[RAX] = static_cast<uint64_t>(q);
                        gpr[RDX] = static_cast<uint64_t>(n % d);
                    }
                    return;
                }
                case 7: { // IDIV
                    int64_t d = signExtend(readRm(in, size), size);
                    if (d == 0) throw EmuError("deling door nul (IDIV)");
                    if (size == 1) {
                        int16_t n = static_cast<int16_t>(readGpr(RAX, 2));
                        int8_t q = static_cast<int8_t>(n / d);
                        int8_t r = static_cast<int8_t>(n % d);
                        gpr[RAX] = (gpr[RAX] & ~0xFFFFull) | (static_cast<uint8_t>(q)) |
                                   (static_cast<uint64_t>(static_cast<uint8_t>(r)) << 8);
                    } else if (size == 2) {
                        int32_t n = static_cast<int32_t>(
                            (static_cast<uint32_t>(readGpr(RDX, 2)) << 16) |
                            static_cast<uint32_t>(readGpr(RAX, 2)));
                        writeGpr(RAX, 2, static_cast<uint16_t>(n / d));
                        writeGpr(RDX, 2, static_cast<uint16_t>(n % d));
                    } else if (size == 4) {
                        int64_t n = static_cast<int64_t>((readGpr(RDX, 4) << 32) | readGpr(RAX, 4));
                        writeGpr(RAX, 4, static_cast<uint32_t>(n / d));
                        writeGpr(RDX, 4, static_cast<uint32_t>(n % d));
                    } else {
                        __int128 n = (static_cast<__int128>(gpr[RDX]) << 64) | gpr[RAX];
                        gpr[RAX] = static_cast<uint64_t>(n / d);
                        gpr[RDX] = static_cast<uint64_t>(n % d);
                    }
                    return;
                }
            }
            return;
        }

        // ---------------- Group 4/5 ----------------
        case 0xFE: {
            int sub = (in.modrm >> 3) & 7;
            uint64_t a = readRm(in, 1);
            if (sub == 0) {
                uint64_t r = (a + 1) & 0xFF;
                setIncFlags(a, r, 1);
                writeRm(in, 1, r);
            } else {
                uint64_t r = (a - 1) & 0xFF;
                setDecFlags(a, r, 1);
                writeRm(in, 1, r);
            }
            return;
        }
        case 0xFF: {
            int sub = (in.modrm >> 3) & 7;
            switch (sub) {
                case 0: {
                    uint64_t a = readRm(in, sz);
                    uint64_t r = (a + 1) & maskFor(sz);
                    setIncFlags(a, r, sz);
                    writeRm(in, sz, r);
                    return;
                }
                case 1: {
                    uint64_t a = readRm(in, sz);
                    uint64_t r = (a - 1) & maskFor(sz);
                    setDecFlags(a, r, sz);
                    writeRm(in, sz, r);
                    return;
                }
                case 2: { // CALL r/m64
                    uint64_t target = readRm(in, 8);
                    push64(nextRip);
                    rip = target;
                    return;
                }
                case 4: // JMP r/m64
                    rip = readRm(in, 8);
                    return;
                case 6: // PUSH r/m64
                    push64(readRm(in, 8));
                    return;
                default:
                    throw UnsupportedInstruction(in, strFormat("FF /%d (far call/jmp)", sub));
            }
        }

        // ---------------- INC/DEC (0x40-0x4F is REX in 64-bit) ----------------

        // ---------------- IMUL met immediate ----------------
        case 0x69: case 0x6B: {
            int64_t a = signExtend(readRm(in, sz), sz);
            int64_t b = in.imm;
            __int128 full = static_cast<__int128>(a) * b;
            uint64_t r = static_cast<uint64_t>(full) & maskFor(sz);
            writeGpr(in.regField, sz, r);
            flags.cf = flags.of = (full != static_cast<__int128>(signExtend(r, sz)));
            flags.zf = (r == 0);
            flags.sf = (r & msbMask(sz)) != 0;
            return;
        }

        // ---------------- sprongen ----------------
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            if (testCondition(static_cast<uint8_t>(op - 0x70)))
                rip = nextRip + static_cast<uint64_t>(in.imm);
            return;
        case 0xEB: rip = nextRip + static_cast<uint64_t>(in.imm); return;
        case 0xE9: rip = nextRip + static_cast<uint64_t>(in.imm); return;
        case 0xE8:
            push64(nextRip);
            rip = nextRip + static_cast<uint64_t>(in.imm);
            return;
        case 0xC3: rip = pop64(); return;
        case 0xC2: {
            uint64_t ret = pop64();
            gpr[RSP] += static_cast<uint16_t>(in.imm);
            rip = ret;
            return;
        }
        case 0xC9: // LEAVE
            gpr[RSP] = gpr[RBP];
            gpr[RBP] = pop64();
            return;
        case 0xE0: // LOOPNE
            if (--gpr[RCX] != 0 && !flags.zf) rip = nextRip + static_cast<uint64_t>(in.imm);
            return;
        case 0xE1: // LOOPE
            if (--gpr[RCX] != 0 && flags.zf) rip = nextRip + static_cast<uint64_t>(in.imm);
            return;
        case 0xE2: // LOOP
            if (--gpr[RCX] != 0) rip = nextRip + static_cast<uint64_t>(in.imm);
            return;
        case 0xE3: // JRCXZ
            if (gpr[RCX] == 0) rip = nextRip + static_cast<uint64_t>(in.imm);
            return;

        // ---------------- conversies ----------------
        case 0x98: // CBW / CWDE / CDQE
            if (sz == 2) writeGpr(RAX, 2, static_cast<uint16_t>(static_cast<int8_t>(gpr[RAX])));
            else if (sz == 4)
                writeGpr(RAX, 4, static_cast<uint32_t>(static_cast<int16_t>(gpr[RAX])));
            else gpr[RAX] = static_cast<uint64_t>(static_cast<int32_t>(gpr[RAX]));
            return;
        case 0x99: // CWD / CDQ / CQO
            if (sz == 2) writeGpr(RDX, 2, (gpr[RAX] & 0x8000) ? 0xFFFF : 0);
            else if (sz == 4) writeGpr(RDX, 4, (gpr[RAX] & 0x80000000ull) ? 0xFFFFFFFFull : 0);
            else gpr[RDX] = (gpr[RAX] & 0x8000000000000000ull) ? ~0ull : 0;
            return;
        case 0x9E: { // SAHF
            uint8_t ah = static_cast<uint8_t>((gpr[RAX] >> 8) & 0xFF);
            flags.cf = ah & 1;
            flags.pf = (ah >> 2) & 1;
            flags.af = (ah >> 4) & 1;
            flags.zf = (ah >> 6) & 1;
            flags.sf = (ah >> 7) & 1;
            return;
        }
        case 0x9F: { // LAHF
            uint8_t ah = static_cast<uint8_t>(flags.pack() & 0xFF) | 0x02;
            gpr[RAX] = (gpr[RAX] & ~0xFF00ull) | (static_cast<uint64_t>(ah) << 8);
            return;
        }

        // ---------------- string ----------------
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        case 0xAE: case 0xAF:
            executeString(in);
            return;

        // ---------------- flags & misc ----------------
        case 0xF5: flags.cf = !flags.cf; return;
        case 0xF8: flags.cf = false; return;
        case 0xF9: flags.cf = true; return;
        case 0xFA: flags.if_ = false; return;
        case 0xFB: flags.if_ = true; return;
        case 0xFC: flags.df = false; return;
        case 0xFD: flags.df = true; return;
        case 0xF4: // HLT
            halted = true;
            return;
        case 0xCC: // INT3
            throw EmuError(strFormat("INT3 (breakpoint) op 0x%llx - de gast is gecrasht of "
                                     "raakte in niet-geïnitialiseerde code",
                                     static_cast<unsigned long long>(in.rip)));
        case 0xCD:
            throw UnsupportedInstruction(in, strFormat("INT 0x%llx",
                                                       (unsigned long long)(in.imm & 0xFF)));
        case 0x9B: return; // FWAIT
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            if (executeX87(in)) return;
            throw UnsupportedInstruction(in, "x87 FPU-instructie buiten de ondersteunde subset");

        default:
            break;
    }

    if ((op & 0xFF00) == 0x0F00 || (op & 0xFF00) == 0x3800 || (op & 0xFF00) == 0x3A00) {
        executeTwoByte(in);
        return;
    }
    throw UnsupportedInstruction(in, "");
}

// ---------------------------------------------------------------------------
// 0F-map
// ---------------------------------------------------------------------------
void Cpu::executeTwoByte(const Instr& in) {
    const int sz = in.opSize;
    uint64_t nextRip = in.rip + in.len;
    uint8_t op = static_cast<uint8_t>(in.opcode & 0xFF);

    if ((in.opcode & 0xFF00) == 0x0F00) {
        switch (op) {
            case 0x01: { // groep: XGETBV etc.
                if (in.modrm == 0xD0) { // XGETBV
                    gpr[RAX] = 0x7; // x87 + SSE state
                    gpr[RDX] = 0;
                    return;
                }
                throw UnsupportedInstruction(in, "0F 01-groep (systeeminstructie)");
            }
            case 0x0B:
                throw EmuError(strFormat("UD2 op 0x%llx - de gast heeft bewust gecrasht "
                                         "(assertion/__fastfail)",
                                         static_cast<unsigned long long>(in.rip)));
            case 0x05:
                throw UnsupportedInstruction(in, "SYSCALL - Windows-code hoort dit niet te doen");
            case 0x0D: case 0x18: case 0x19: case 0x1A: case 0x1B:
            case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                return; // prefetch / hint-nop / multi-byte nop
            case 0x31: { // RDTSC - we tellen simpelweg instructies
                uint64_t tsc = instructionsExecuted * 3;
                gpr[RAX] = tsc & 0xFFFFFFFFull;
                gpr[RDX] = (tsc >> 32) & 0xFFFFFFFFull;
                return;
            }
            case 0xA2: { // CPUID
                uint32_t leaf = static_cast<uint32_t>(gpr[RAX]);
                uint32_t a = 0, b = 0, c = 0, d = 0;
                switch (leaf) {
                    case 0:
                        a = 1;               // max leaf: bewust laag
                        b = 0x756E6547;      // "Genu"
                        d = 0x49656E69;      // "ineI"
                        c = 0x6C65746E;      // "ntel"
                        break;
                    case 1:
                        a = 0x000306A9;      // family/model/stepping
                        b = 0x00000800;
                        // We melden SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT/CMPXCHG16B,
                        // maar NIET AVX/XSAVE: die implementeren we niet en dan
                        // kiest de CRT anders een AVX-pad dat we niet aankunnen.
                        c = (1u << 0) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 20) |
                            (1u << 23);
                        d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) |
                            (1u << 24) | (1u << 25) | (1u << 26);
                        break;
                    default:
                        break;
                }
                gpr[RAX] = a; gpr[RBX] = b; gpr[RCX] = c; gpr[RDX] = d;
                return;
            }
            // CMOVcc
            case 0x40: case 0x41: case 0x42: case 0x43:
            case 0x44: case 0x45: case 0x46: case 0x47:
            case 0x48: case 0x49: case 0x4A: case 0x4B:
            case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
                uint64_t v = readRm(in, sz);
                if (testCondition(static_cast<uint8_t>(op - 0x40)))
                    writeGpr(in.regField, sz, v);
                else if (sz == 4)
                    writeGpr(in.regField, 4, readGpr(in.regField, 4)); // zero-extend
                return;
            }
            // Jcc rel32
            case 0x80: case 0x81: case 0x82: case 0x83:
            case 0x84: case 0x85: case 0x86: case 0x87:
            case 0x88: case 0x89: case 0x8A: case 0x8B:
            case 0x8C: case 0x8D: case 0x8E: case 0x8F:
                if (testCondition(static_cast<uint8_t>(op - 0x80)))
                    rip = nextRip + static_cast<uint64_t>(in.imm);
                return;
            // SETcc
            case 0x90: case 0x91: case 0x92: case 0x93:
            case 0x94: case 0x95: case 0x96: case 0x97:
            case 0x98: case 0x99: case 0x9A: case 0x9B:
            case 0x9C: case 0x9D: case 0x9E: case 0x9F:
                writeRm(in, 1, testCondition(static_cast<uint8_t>(op - 0x90)) ? 1 : 0);
                return;

            case 0xAF: { // IMUL r, r/m
                int64_t a = signExtend(readGpr(in.regField, sz), sz);
                int64_t b = signExtend(readRm(in, sz), sz);
                __int128 full = static_cast<__int128>(a) * b;
                uint64_t r = static_cast<uint64_t>(full) & maskFor(sz);
                writeGpr(in.regField, sz, r);
                flags.cf = flags.of = (full != static_cast<__int128>(signExtend(r, sz)));
                flags.zf = (r == 0);
                flags.sf = (r & msbMask(sz)) != 0;
                return;
            }
            case 0xB6: writeGpr(in.regField, sz, readRm(in, 1)); return;           // MOVZX r, r/m8
            case 0xB7: writeGpr(in.regField, sz, readRm(in, 2)); return;           // MOVZX r, r/m16
            case 0xBE: writeGpr(in.regField, sz,
                                static_cast<uint64_t>(signExtend(readRm(in, 1), 1)) & maskFor(sz));
                return; // MOVSX
            case 0xBF: writeGpr(in.regField, sz,
                                static_cast<uint64_t>(signExtend(readRm(in, 2), 2)) & maskFor(sz));
                return;

            case 0xB8: { // POPCNT (F3-prefix)
                if (!in.pF3) throw UnsupportedInstruction(in, "0F B8 zonder F3 (JMPE)");
                uint64_t v = readRm(in, sz);
                uint64_t c = static_cast<uint64_t>(__builtin_popcountll(v & maskFor(sz)));
                writeGpr(in.regField, sz, c);
                flags.zf = (c == 0);
                flags.cf = flags.of = flags.sf = flags.af = flags.pf = false;
                return;
            }
            case 0xBC: { // BSF / TZCNT
                uint64_t v = readRm(in, sz) & maskFor(sz);
                if (in.pF3) { // TZCNT
                    uint64_t c = v ? static_cast<uint64_t>(__builtin_ctzll(v))
                                   : static_cast<uint64_t>(sz * 8);
                    writeGpr(in.regField, sz, c);
                    flags.cf = (v == 0);
                    flags.zf = (c == 0);
                } else {
                    flags.zf = (v == 0);
                    if (v) writeGpr(in.regField, sz, static_cast<uint64_t>(__builtin_ctzll(v)));
                }
                return;
            }
            case 0xBD: { // BSR / LZCNT
                uint64_t v = readRm(in, sz) & maskFor(sz);
                if (in.pF3) { // LZCNT
                    uint64_t c = v ? static_cast<uint64_t>(__builtin_clzll(v) - (64 - sz * 8))
                                   : static_cast<uint64_t>(sz * 8);
                    writeGpr(in.regField, sz, c);
                    flags.cf = (v == 0);
                    flags.zf = (c == 0);
                } else {
                    flags.zf = (v == 0);
                    if (v) writeGpr(in.regField, sz, static_cast<uint64_t>(63 - __builtin_clzll(v)));
                }
                return;
            }

            case 0xA3: case 0xAB: case 0xB3: case 0xBB: { // BT / BTS / BTR / BTC
                uint64_t bit = readGpr(in.regField, sz);
                uint64_t v;
                if (in.rmIsReg) {
                    bit &= (sz * 8 - 1);
                    v = readRm(in, sz);
                } else {
                    // Bij een geheugenoperand mag de bit-index buiten de operand wijzen.
                    int64_t sbit = signExtend(bit, sz);
                    uint64_t ea = effectiveAddress(in) + static_cast<uint64_t>(sbit >> 3);
                    uint8_t byteVal = mem.read8(ea);
                    uint64_t b = static_cast<uint64_t>(sbit) & 7;
                    flags.cf = (byteVal >> b) & 1;
                    uint8_t nv = byteVal;
                    if (op == 0xAB) nv |= static_cast<uint8_t>(1u << b);
                    else if (op == 0xB3) nv &= static_cast<uint8_t>(~(1u << b));
                    else if (op == 0xBB) nv ^= static_cast<uint8_t>(1u << b);
                    if (op != 0xA3) mem.write8(ea, nv);
                    return;
                }
                flags.cf = (v >> bit) & 1;
                if (op == 0xAB) v |= (1ull << bit);
                else if (op == 0xB3) v &= ~(1ull << bit);
                else if (op == 0xBB) v ^= (1ull << bit);
                if (op != 0xA3) writeRm(in, sz, v & maskFor(sz));
                return;
            }
            case 0xBA: { // BT/BTS/BTR/BTC met imm8
                int sub = (in.modrm >> 3) & 7;
                uint64_t bit = static_cast<uint64_t>(in.imm) & (sz * 8 - 1);
                uint64_t v = readRm(in, sz);
                flags.cf = (v >> bit) & 1;
                if (sub == 5) v |= (1ull << bit);
                else if (sub == 6) v &= ~(1ull << bit);
                else if (sub == 7) v ^= (1ull << bit);
                if (sub != 4) writeRm(in, sz, v & maskFor(sz));
                return;
            }

            case 0xA4: case 0xA5: { // SHLD
                uint64_t count = (op == 0xA4) ? (static_cast<uint64_t>(in.imm) & 0xFF)
                                              : (gpr[RCX] & 0xFF);
                count &= (sz == 8) ? 0x3F : 0x1F;
                if (count == 0) return;
                uint64_t dst = readRm(in, sz), src = readGpr(in.regField, sz);
                int bits = sz * 8;
                uint64_t r = (dst << count) | (src >> (bits - count));
                flags.cf = (dst >> (bits - count)) & 1;
                r &= maskFor(sz);
                writeRm(in, sz, r);
                flags.zf = (r == 0);
                flags.sf = (r & msbMask(sz)) != 0;
                return;
            }
            case 0xAC: case 0xAD: { // SHRD
                uint64_t count = (op == 0xAC) ? (static_cast<uint64_t>(in.imm) & 0xFF)
                                              : (gpr[RCX] & 0xFF);
                count &= (sz == 8) ? 0x3F : 0x1F;
                if (count == 0) return;
                uint64_t dst = readRm(in, sz), src = readGpr(in.regField, sz);
                int bits = sz * 8;
                uint64_t r = (dst >> count) | (src << (bits - count));
                flags.cf = (dst >> (count - 1)) & 1;
                r &= maskFor(sz);
                writeRm(in, sz, r);
                flags.zf = (r == 0);
                flags.sf = (r & msbMask(sz)) != 0;
                return;
            }

            case 0xB0: case 0xB1: { // CMPXCHG
                int size = (op == 0xB0) ? 1 : sz;
                uint64_t dst = readRm(in, size);
                uint64_t acc = readGpr(RAX, size);
                setSubFlags(acc, dst, (acc - dst) & maskFor(size), size);
                if (acc == dst) {
                    flags.zf = true;
                    writeRm(in, size, readGpr(in.regField, size));
                } else {
                    flags.zf = false;
                    writeGpr(RAX, size, dst);
                }
                return;
            }
            case 0xC0: case 0xC1: { // XADD
                int size = (op == 0xC0) ? 1 : sz;
                uint64_t a = readRm(in, size);
                uint64_t b = readRegOperand(in, size);
                uint64_t r = (a + b) & maskFor(size);
                setAddFlags(a, b, r, size);
                writeRegOperand(in, size, a);
                writeRm(in, size, r);
                return;
            }
            case 0xC3: // MOVNTI
                writeRm(in, sz, readGpr(in.regField, sz));
                return;
            case 0xC7: { // CMPXCHG16B / CMPXCHG8B
                int sub = (in.modrm >> 3) & 7;
                if (sub != 1) throw UnsupportedInstruction(in, "0F C7-groep");
                uint64_t ea = effectiveAddress(in);
                uint64_t lo = mem.read64(ea), hi = mem.read64(ea + 8);
                if (lo == gpr[RAX] && hi == gpr[RDX]) {
                    mem.write64(ea, gpr[RBX]);
                    mem.write64(ea + 8, gpr[RCX]);
                    flags.zf = true;
                } else {
                    gpr[RAX] = lo;
                    gpr[RDX] = hi;
                    flags.zf = false;
                }
                return;
            }
            case 0xC8: case 0xC9: case 0xCA: case 0xCB:
            case 0xCC: case 0xCD: case 0xCE: case 0xCF: { // BSWAP
                int r = (op - 0xC8) | (in.rexB ? 8 : 0);
                if (sz == 8) gpr[r] = __builtin_bswap64(gpr[r]);
                else writeGpr(r, 4, __builtin_bswap32(static_cast<uint32_t>(gpr[r])));
                return;
            }
            case 0xAE: { // fences / (f)xsave / stmxcsr
                int sub = (in.modrm >> 3) & 7;
                if (in.rmIsReg) return; // lfence/mfence/sfence
                if (sub == 3) { mem.write32(effectiveAddress(in), mxcsr); return; } // stmxcsr
                if (sub == 2) { mxcsr = mem.read32(effectiveAddress(in)); return; } // ldmxcsr
                return;
            }
            default:
                break;
        }
    }

    // Alles wat overblijft proberen we als SSE/SSE2.
    if (executeSse(in)) return;
    throw UnsupportedInstruction(in, "");
}

// ---------------------------------------------------------------------------
// Stap / run
// ---------------------------------------------------------------------------
void Cpu::step() {
    if (isHleAddress(rip)) {
        uint32_t id = static_cast<uint32_t>((rip - hleBase_) / kHleSlotSize);
        if (!hleSink_) throw EmuError("HLE-aanroep zonder geregistreerde Win32-laag");
        // De aanroeper heeft het returnadres al gepusht (CALL). De host-functie
        // krijgt de argumenten uit de registers volgens de Win64-conventie.
        hleSink_->onHostCall(*this, id);
        if (halted) return;
        rip = pop64();
        ++instructionsExecuted;
        return;
    }

    Instr in;
    decode(rip, in);
    if (traceEnabled && (traceLimit == 0 || instructionsExecuted < traceLimit)) {
        MACEMU_LOG_TRACE("%016llx  %-40s  rax=%llx rcx=%llx rdx=%llx rsp=%llx",
                         (unsigned long long)in.rip, disassemble(in).c_str(),
                         (unsigned long long)gpr[RAX], (unsigned long long)gpr[RCX],
                         (unsigned long long)gpr[RDX], (unsigned long long)gpr[RSP]);
    }
    execute(in);
    ++instructionsExecuted;
}

void Cpu::run(uint64_t maxInstructions) {
    uint64_t start = instructionsExecuted;
    while (!halted) {
        step();
        if (maxInstructions && instructionsExecuted - start >= maxInstructions)
            throw EmuError(strFormat("instructielimiet bereikt (%llu) - vermoedelijk een "
                                     "oneindige lus op 0x%llx",
                                     (unsigned long long)maxInstructions,
                                     (unsigned long long)rip));
    }
}

uint64_t Cpu::callGuest(uint64_t addr, const std::vector<uint64_t>& args,
                        uint64_t maxInstructions) {
    // Sentinel: een adres in het HLE-gebied dat we nooit als import uitdelen.
    if (!returnSentinel_) returnSentinel_ = hleBase_ + hleSize_ - kHleSlotSize;

    // Registers bewaren. De gast mag niet merken dat de host tussenbeide kwam;
    // Memory is gedeeld en wordt bewust niet teruggedraaid.
    struct Snapshot {
        uint64_t gpr[16];
        uint64_t rip;
        Xmm xmm[16];
        Flags flags;
    } saved;
    std::memcpy(saved.gpr, gpr, sizeof(gpr));
    std::memcpy(saved.xmm, xmm, sizeof(xmm));
    saved.rip = rip;
    saved.flags = flags;

    // Win64: eerste 4 integer-argumenten in RCX, RDX, R8, R9; daarna op de
    // stack. Altijd 32 bytes "shadow space" reserveren.
    size_t stackArgs = args.size() > 4 ? args.size() - 4 : 0;
    uint64_t rsp = gpr[RSP];
    rsp -= 32 + stackArgs * 8;
    rsp &= ~0xFull;
    rsp -= 8; // zodat RSP+8 na de CALL 16-byte aligned is
    gpr[RSP] = rsp;

    static const int kArgRegs[4] = {RCX, RDX, R8, R9};
    for (size_t i = 0; i < args.size() && i < 4; ++i) gpr[kArgRegs[i]] = args[i];
    for (size_t i = 4; i < args.size(); ++i) mem.write64(rsp + 8 + 32 + (i - 4) * 8, args[i]);

    push64(returnSentinel_);
    rip = addr;

    uint64_t startCount = instructionsExecuted;
    while (rip != returnSentinel_ && !halted) {
        step();
        if (instructionsExecuted - startCount > maxInstructions)
            throw EmuError("callGuest: instructielimiet bereikt (oneindige lus in de gast?)");
    }
    uint64_t result = gpr[RAX];

    std::memcpy(gpr, saved.gpr, sizeof(gpr));
    std::memcpy(xmm, saved.xmm, sizeof(xmm));
    rip = saved.rip;
    flags = saved.flags;
    return result;
}

std::string Cpu::dumpState() const {
    std::string s;
    s += strFormat("rip=%016llx  flags=%c%c%c%c%c%c\n", (unsigned long long)rip,
                   flags.cf ? 'C' : '-', flags.pf ? 'P' : '-', flags.af ? 'A' : '-',
                   flags.zf ? 'Z' : '-', flags.sf ? 'S' : '-', flags.of ? 'O' : '-');
    for (int i = 0; i < 16; i += 4) {
        s += "  ";
        for (int j = 0; j < 4; ++j)
            s += strFormat("%-4s=%016llx ", kReg64Names[i + j], (unsigned long long)gpr[i + j]);
        s += "\n";
    }
    return s;
}

std::string Cpu::dumpStack(int slots) const {
    std::string s = strFormat("stack @ %016llx\n", (unsigned long long)gpr[RSP]);
    for (int i = 0; i < slots; ++i) {
        uint64_t a = gpr[RSP] + i * 8;
        try {
            s += strFormat("  [rsp+%02x] %016llx\n", i * 8, (unsigned long long)mem.read64(a));
        } catch (const std::exception&) {
            s += strFormat("  [rsp+%02x] <niet leesbaar>\n", i * 8);
            break;
        }
    }
    return s;
}

} // namespace macemu
