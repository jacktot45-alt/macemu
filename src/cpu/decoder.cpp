// macemu - x86-64 instructie-decoder (Fase 1)
//
// Verwerkt: legacy prefixes, REX, 1/2/3-byte opcodes, ModRM, SIB, displacement
// en immediates. De decoder kent geen betekenis van instructies - dat doet
// exec.cpp. Deze scheiding maakt het uitbreiden een kwestie van "case erbij".
#include <cstring>

#include "macemu/cpu.h"

namespace macemu {

const char* const kReg64Names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                     "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
const char* const kReg32Names[16] = {"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                                     "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
const char* const kReg16Names[16] = {"ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
                                     "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
const char* const kReg8Names[16] = {"al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
                                    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
const char* const kReg8LegacyNames[8] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};

UnsupportedInstruction::UnsupportedInstruction(const Instr& in, const std::string& detail)
    : EmuError(strFormat("niet-ondersteunde instructie op 0x%llx: opcode 0x%x%s%s",
                         static_cast<unsigned long long>(in.rip), in.opcode,
                         detail.empty() ? "" : " - ", detail.c_str())),
      instr(in) {}

namespace {

// Immediate-codes.
enum ImmCode : uint8_t {
    IMM_NONE = 0,
    IMM_8 = 1,
    IMM_16 = 2,
    IMM_Z = 3,   // 2 bytes bij 16-bit opsize, anders 4 (sign-extended naar 64)
    IMM_V = 4,   // volgt de operandgrootte (2/4/8)
    IMM_MOFFS = 5, // absoluut adres, volgt de adresgrootte
    IMM_16_8 = 6,  // enter: imm16 gevolgd door imm8
};

struct OpProps {
    bool modrm;
    uint8_t imm;
    bool default64; // in long mode standaard 64-bit operand
};

OpProps oneByteProps(uint8_t op) {
    switch (op) {
        // ALU-groepen 00..3F
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
            return {true, IMM_NONE, false};
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C:
            return {false, IMM_8, false};
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D:
            return {false, IMM_Z, false};

        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            return {false, IMM_NONE, true};

        case 0x63: return {true, IMM_NONE, false};  // movsxd
        case 0x68: return {false, IMM_Z, true};     // push imm32
        case 0x69: return {true, IMM_Z, false};     // imul r,r/m,imm32
        case 0x6A: return {false, IMM_8, true};     // push imm8
        case 0x6B: return {true, IMM_8, false};     // imul r,r/m,imm8

        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            return {false, IMM_8, true};

        case 0x80: return {true, IMM_8, false};
        case 0x81: return {true, IMM_Z, false};
        case 0x83: return {true, IMM_8, false};
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E:
            return {true, IMM_NONE, false};
        case 0x8F: return {true, IMM_NONE, true};

        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9B: case 0x9E: case 0x9F:
            return {false, IMM_NONE, false};
        case 0x9C: case 0x9D: return {false, IMM_NONE, true};

        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
            return {false, IMM_MOFFS, false};
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
            return {false, IMM_NONE, false};
        case 0xA8: return {false, IMM_8, false};
        case 0xA9: return {false, IMM_Z, false};
        case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            return {false, IMM_NONE, false};

        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            return {false, IMM_8, false};
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            return {false, IMM_V, false};

        case 0xC0: case 0xC1: return {true, IMM_8, false};
        case 0xC2: return {false, IMM_16, true};
        case 0xC3: return {false, IMM_NONE, true};
        case 0xC6: return {true, IMM_8, false};
        case 0xC7: return {true, IMM_Z, false};
        case 0xC8: return {false, IMM_16_8, true};
        case 0xC9: return {false, IMM_NONE, true};
        case 0xCC: return {false, IMM_NONE, false};
        case 0xCD: return {false, IMM_8, false};

        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
            return {true, IMM_NONE, false};
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            return {true, IMM_NONE, false}; // x87

        case 0xE0: case 0xE1: case 0xE2: case 0xE3:
            return {false, IMM_8, true};
        case 0xE8: case 0xE9: return {false, IMM_Z, true};
        case 0xEB: return {false, IMM_8, true};

        case 0xF4: case 0xF5: case 0xF8: case 0xF9:
        case 0xFA: case 0xFB: case 0xFC: case 0xFD:
            return {false, IMM_NONE, false};
        case 0xF6: case 0xF7: return {true, IMM_NONE, false}; // imm hangt van /reg af
        case 0xFE: return {true, IMM_NONE, false};
        case 0xFF: return {true, IMM_NONE, false};            // default64 per /reg

        default: return {false, IMM_NONE, false};
    }
}

OpProps twoByteProps(uint8_t op) {
    switch (op) {
        case 0x00: case 0x01: return {true, IMM_NONE, false};
        case 0x05: case 0x06: case 0x0B: return {false, IMM_NONE, false}; // syscall/clts/ud2
        case 0x0D: return {true, IMM_NONE, false};
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
            return {true, IMM_NONE, false}; // hints / multi-byte nop
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
            return {false, IMM_NONE, false};
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            return {true, IMM_NONE, false}; // cmovcc
        case 0x70: case 0xC2: case 0xC4: case 0xC5: case 0xC6:
        case 0x71: case 0x72: case 0x73:
            return {true, IMM_8, false}; // SSE met imm8
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            return {false, IMM_Z, true}; // jcc rel32
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            return {true, IMM_NONE, false}; // setcc
        case 0xA0: case 0xA1: case 0xA8: case 0xA9:
            return {false, IMM_NONE, true}; // push/pop fs/gs
        case 0xA2: return {false, IMM_NONE, false}; // cpuid
        case 0xA4: case 0xAC: return {true, IMM_8, false}; // shld/shrd imm8
        case 0xBA: return {true, IMM_8, false};            // bt-groep
        case 0xA3: case 0xA5: case 0xAB: case 0xAD: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB3: case 0xB6: case 0xB7:
        case 0xB8: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        case 0xC0: case 0xC1: case 0xC3: case 0xC7:
            return {true, IMM_NONE, false};
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            return {false, IMM_NONE, false}; // bswap
        default:
            // Alles wat overblijft in de 0F-map is SSE/MMX met ModRM.
            return {true, IMM_NONE, false};
    }
}

} // namespace

size_t Cpu::decode(uint64_t addr, Instr& in) const {
    in = Instr();
    in.rip = addr;

    uint8_t buf[16];
    mem.fetch(addr, buf, sizeof(buf));
    std::memcpy(in.bytes, buf, sizeof(buf));

    size_t p = 0;
    bool opSize16 = false;
    bool addrSize32 = false;

    // --- legacy prefixes ---
    for (;;) {
        uint8_t b = buf[p];
        if (b == 0x66) { opSize16 = true; in.p66 = true; ++p; }
        else if (b == 0x67) { addrSize32 = true; ++p; }
        else if (b == 0xF0) { in.lock = true; ++p; }
        else if (b == 0xF2) { in.pF2 = true; ++p; }
        else if (b == 0xF3) { in.pF3 = true; ++p; }
        else if (b == 0x64) { in.segment = 1; ++p; }
        else if (b == 0x65) { in.segment = 2; ++p; }
        else if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) { ++p; } // genegeerd in 64-bit
        else break;
        if (p >= 15) throw EmuError("decoder: te veel prefixes");
    }

    // --- REX (moet direct voor de opcode staan) ---
    if ((buf[p] & 0xF0) == 0x40) {
        uint8_t r = buf[p++];
        in.rex = true;
        in.rexW = (r & 8) != 0;
        in.rexR = (r & 4) != 0;
        in.rexX = (r & 2) != 0;
        in.rexB = (r & 1) != 0;
    }

    // --- opcode ---
    OpProps props;
    uint8_t op = buf[p++];
    if (op == 0x0F) {
        uint8_t op2 = buf[p++];
        if (op2 == 0x38) {
            in.opcode = 0x3800 | buf[p++];
            props = {true, IMM_NONE, false};
        } else if (op2 == 0x3A) {
            in.opcode = 0x3A00 | buf[p++];
            props = {true, IMM_8, false};
        } else {
            in.opcode = static_cast<uint16_t>(0x0F00 | op2);
            props = twoByteProps(op2);
        }
    } else {
        in.opcode = op;
        props = oneByteProps(op);
    }

    // --- operand-/adresgrootte ---
    in.addrSize = addrSize32 ? 4 : 8;
    if (in.rexW) in.opSize = 8;
    else if (opSize16) in.opSize = 2;
    else if (props.default64) in.opSize = 8;
    else in.opSize = 4;

    // --- ModRM/SIB/displacement ---
    if (props.modrm) {
        in.hasModrm = true;
        in.modrm = buf[p++];
        in.mod = in.modrm >> 6;
        in.regField = static_cast<uint8_t>(((in.modrm >> 3) & 7) | (in.rexR ? 8 : 0));
        uint8_t rm = in.modrm & 7;

        if (in.mod == 3) {
            in.rmIsReg = true;
            in.rmField = static_cast<uint8_t>(rm | (in.rexB ? 8 : 0));
        } else {
            in.rmIsReg = false;
            if (rm == 4) {
                // SIB
                uint8_t sib = buf[p++];
                uint8_t ss = sib >> 6;
                uint8_t idx = static_cast<uint8_t>(((sib >> 3) & 7) | (in.rexX ? 8 : 0));
                uint8_t base = static_cast<uint8_t>((sib & 7) | (in.rexB ? 8 : 0));
                in.scale = static_cast<uint8_t>(1u << ss);
                in.indexReg = (idx == 4) ? -1 : idx; // index 4 = geen index
                if ((sib & 7) == 5 && in.mod == 0) {
                    in.baseReg = -1;
                    in.disp = static_cast<int32_t>(buf[p] | (buf[p + 1] << 8) |
                                                   (buf[p + 2] << 16) | (buf[p + 3] << 24));
                    p += 4;
                } else {
                    in.baseReg = base;
                }
            } else if (rm == 5 && in.mod == 0) {
                // RIP-relatief
                in.ripRelative = true;
                in.baseReg = -1;
                in.indexReg = -1;
                in.disp = static_cast<int32_t>(buf[p] | (buf[p + 1] << 8) | (buf[p + 2] << 16) |
                                               (buf[p + 3] << 24));
                p += 4;
            } else {
                in.baseReg = rm | (in.rexB ? 8 : 0);
                in.indexReg = -1;
            }

            if (in.mod == 1) {
                in.disp += static_cast<int8_t>(buf[p]);
                p += 1;
            } else if (in.mod == 2) {
                in.disp += static_cast<int32_t>(buf[p] | (buf[p + 1] << 8) | (buf[p + 2] << 16) |
                                                (buf[p + 3] << 24));
                p += 4;
            }
        }

        // Opcodes waarvan de immediate-grootte van /reg afhangt.
        if (in.opcode == 0xF6 && ((in.modrm >> 3) & 7) <= 1) props.imm = IMM_8;
        if (in.opcode == 0xF7 && ((in.modrm >> 3) & 7) <= 1) props.imm = IMM_Z;
        // FF /2 /3 /4 /5 /6 zijn standaard 64-bit (call/jmp/push).
        if (in.opcode == 0xFF) {
            uint8_t sub = (in.modrm >> 3) & 7;
            if (sub >= 2 && sub <= 6 && !in.rexW && !opSize16) in.opSize = 8;
        }
    }

    // --- immediate ---
    auto rd32 = [&](size_t o) {
        return static_cast<uint32_t>(buf[o] | (buf[o + 1] << 8) | (buf[o + 2] << 16) |
                                     (static_cast<uint32_t>(buf[o + 3]) << 24));
    };
    switch (props.imm) {
        case IMM_NONE: break;
        case IMM_8:
            in.imm = static_cast<int8_t>(buf[p]);
            in.immSize = 1;
            p += 1;
            break;
        case IMM_16:
            in.imm = static_cast<int16_t>(buf[p] | (buf[p + 1] << 8));
            in.immSize = 2;
            p += 2;
            break;
        case IMM_16_8:
            in.imm = static_cast<uint16_t>(buf[p] | (buf[p + 1] << 8));
            in.imm |= static_cast<int64_t>(buf[p + 2]) << 32;
            in.immSize = 3;
            p += 3;
            break;
        case IMM_Z:
            if (in.opSize == 2) {
                in.imm = static_cast<int16_t>(buf[p] | (buf[p + 1] << 8));
                in.immSize = 2;
                p += 2;
            } else {
                in.imm = static_cast<int32_t>(rd32(p));
                in.immSize = 4;
                p += 4;
            }
            break;
        case IMM_V:
            if (in.opSize == 8) {
                uint64_t v = 0;
                for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(buf[p + i]) << (8 * i);
                in.imm = static_cast<int64_t>(v);
                in.immSize = 8;
                p += 8;
            } else if (in.opSize == 2) {
                in.imm = static_cast<int16_t>(buf[p] | (buf[p + 1] << 8));
                in.immSize = 2;
                p += 2;
            } else {
                in.imm = static_cast<int32_t>(rd32(p));
                in.immSize = 4;
                p += 4;
            }
            break;
        case IMM_MOFFS: {
            uint64_t v = 0;
            int n = in.addrSize;
            for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(buf[p + i]) << (8 * i);
            in.imm = static_cast<int64_t>(v);
            in.immSize = static_cast<uint8_t>(n);
            p += n;
            break;
        }
    }

    if (p > 15) throw EmuError(strFormat("decoder: instructie langer dan 15 bytes op 0x%llx",
                                         static_cast<unsigned long long>(addr)));
    in.len = static_cast<uint8_t>(p);
    return p;
}

uint64_t Cpu::effectiveAddress(const Instr& in) const {
    uint64_t ea = 0;
    if (in.ripRelative) {
        ea = in.rip + in.len + static_cast<uint64_t>(in.disp);
    } else {
        if (in.baseReg >= 0) ea += gpr[in.baseReg];
        if (in.indexReg >= 0) ea += gpr[in.indexReg] * in.scale;
        ea += static_cast<uint64_t>(in.disp);
    }
    if (in.addrSize == 4) ea &= 0xFFFFFFFFull;
    if (in.segment == 1) ea += fsBase;
    else if (in.segment == 2) ea += gsBase;
    return ea;
}

} // namespace macemu
