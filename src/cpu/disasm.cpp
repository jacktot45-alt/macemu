// macemu - minimale disassembler, alleen bedoeld voor --trace-uitvoer.
// Geen volledige Intel-syntax; genoeg om te zien waar je zit.
#include "macemu/cpu.h"

namespace macemu {

namespace {

const char* regName(int reg, int size, bool hasRex) {
    switch (size) {
        case 1: return hasRex ? kReg8Names[reg] : (reg < 8 ? kReg8LegacyNames[reg] : kReg8Names[reg]);
        case 2: return kReg16Names[reg];
        case 4: return kReg32Names[reg];
        default: return kReg64Names[reg];
    }
}

std::string memOperand(const Instr& in) {
    std::string s = "[";
    bool first = true;
    if (in.ripRelative) {
        s += strFormat("rip%+lld", (long long)in.disp);
        s += strFormat("] (=0x%llx)",
                       (unsigned long long)(in.rip + in.len + (uint64_t)in.disp));
        return s;
    }
    if (in.segment == 1) { s = "fs:["; }
    else if (in.segment == 2) { s = "gs:["; }
    if (in.baseReg >= 0) { s += kReg64Names[in.baseReg]; first = false; }
    if (in.indexReg >= 0) {
        if (!first) s += "+";
        s += strFormat("%s*%u", kReg64Names[in.indexReg], in.scale);
        first = false;
    }
    if (in.disp || first) s += strFormat("%s0x%llx", first ? "" : (in.disp < 0 ? "-" : "+"),
                                         (unsigned long long)(in.disp < 0 ? -in.disp : in.disp));
    s += "]";
    return s;
}

std::string rmOperand(const Instr& in, int size) {
    if (in.rmIsReg) return regName(in.rmField, size, in.rex);
    return memOperand(in);
}

const char* aluName(int which) {
    static const char* n[8] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
    return n[which & 7];
}

const char* ccName(int cc) {
    static const char* n[16] = {"o", "no", "b", "ae", "e", "ne", "be", "a",
                                "s", "ns", "p", "np", "l", "ge", "le", "g"};
    return n[cc & 15];
}

const char* shiftName(int which) {
    static const char* n[8] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "shl", "sar"};
    return n[which & 7];
}

} // namespace

std::string disassemble(const Instr& in) {
    uint16_t op = in.opcode;
    int sz = in.opSize;

    std::string bytes;
    for (int i = 0; i < in.len && i < 8; ++i) bytes += strFormat("%02x", in.bytes[i]);
    while (bytes.size() < 16) bytes += ' ';

    auto out = [&](const std::string& text) { return bytes + " " + text; };

    if (op < 0x40 && (op & 7) <= 5) {
        int which = (op >> 3) & 7;
        switch (op & 7) {
            case 0: return out(strFormat("%s %s, %s", aluName(which), rmOperand(in, 1).c_str(),
                                         regName(in.regField, 1, in.rex)));
            case 1: return out(strFormat("%s %s, %s", aluName(which), rmOperand(in, sz).c_str(),
                                         regName(in.regField, sz, in.rex)));
            case 2: return out(strFormat("%s %s, %s", aluName(which),
                                         regName(in.regField, 1, in.rex), rmOperand(in, 1).c_str()));
            case 3: return out(strFormat("%s %s, %s", aluName(which),
                                         regName(in.regField, sz, in.rex),
                                         rmOperand(in, sz).c_str()));
            case 4: return out(strFormat("%s al, 0x%llx", aluName(which),
                                         (unsigned long long)(in.imm & 0xFF)));
            default: return out(strFormat("%s %s, 0x%llx", aluName(which),
                                          regName(RAX, sz, false), (unsigned long long)in.imm));
        }
    }

    switch (op) {
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            return out(strFormat("push %s", kReg64Names[(op - 0x50) | (in.rexB ? 8 : 0)]));
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            return out(strFormat("pop %s", kReg64Names[(op - 0x58) | (in.rexB ? 8 : 0)]));
        case 0x68: case 0x6A: return out(strFormat("push 0x%llx", (unsigned long long)in.imm));
        case 0x88: return out("mov " + rmOperand(in, 1) + ", " + regName(in.regField, 1, in.rex));
        case 0x89: return out("mov " + rmOperand(in, sz) + ", " + regName(in.regField, sz, in.rex));
        case 0x8A: return out(std::string("mov ") + regName(in.regField, 1, in.rex) + ", " +
                              rmOperand(in, 1));
        case 0x8B: return out(std::string("mov ") + regName(in.regField, sz, in.rex) + ", " +
                              rmOperand(in, sz));
        case 0x8D: return out(std::string("lea ") + regName(in.regField, sz, in.rex) + ", " +
                              memOperand(in));
        case 0x63: return out(std::string("movsxd ") + regName(in.regField, sz, in.rex) + ", " +
                              rmOperand(in, 4));
        case 0xC6: return out("mov " + rmOperand(in, 1) + strFormat(", 0x%llx",
                                                                    (unsigned long long)in.imm));
        case 0xC7: return out("mov " + rmOperand(in, sz) + strFormat(", 0x%llx",
                                                                     (unsigned long long)in.imm));
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            return out(strFormat("mov %s, 0x%llx",
                                 regName((op - 0xB8) | (in.rexB ? 8 : 0), sz, in.rex),
                                 (unsigned long long)in.imm));
        case 0x80: case 0x81: case 0x83:
            return out(strFormat("%s %s, 0x%llx", aluName((in.modrm >> 3) & 7),
                                 rmOperand(in, op == 0x80 ? 1 : sz).c_str(),
                                 (unsigned long long)in.imm));
        case 0x84: return out("test " + rmOperand(in, 1) + ", " + regName(in.regField, 1, in.rex));
        case 0x85: return out("test " + rmOperand(in, sz) + ", " + regName(in.regField, sz, in.rex));
        case 0xC0: case 0xC1:
            return out(strFormat("%s %s, 0x%llx", shiftName((in.modrm >> 3) & 7),
                                 rmOperand(in, op == 0xC0 ? 1 : sz).c_str(),
                                 (unsigned long long)in.imm));
        case 0xD0: case 0xD1:
            return out(strFormat("%s %s, 1", shiftName((in.modrm >> 3) & 7),
                                 rmOperand(in, op == 0xD0 ? 1 : sz).c_str()));
        case 0xD2: case 0xD3:
            return out(strFormat("%s %s, cl", shiftName((in.modrm >> 3) & 7),
                                 rmOperand(in, op == 0xD2 ? 1 : sz).c_str()));
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            return out(strFormat("j%s 0x%llx", ccName(op - 0x70),
                                 (unsigned long long)(in.rip + in.len + in.imm)));
        case 0xE8: return out(strFormat("call 0x%llx",
                                        (unsigned long long)(in.rip + in.len + in.imm)));
        case 0xE9: case 0xEB:
            return out(strFormat("jmp 0x%llx", (unsigned long long)(in.rip + in.len + in.imm)));
        case 0xC3: return out("ret");
        case 0xC2: return out(strFormat("ret 0x%llx", (unsigned long long)in.imm));
        case 0xC9: return out("leave");
        case 0xCC: return out("int3");
        case 0x90: return out("nop");
        case 0x98: return out(sz == 8 ? "cdqe" : "cwde");
        case 0x99: return out(sz == 8 ? "cqo" : "cdq");
        case 0xA4: return out(in.pF3 ? "rep movsb" : "movsb");
        case 0xA5: return out(in.pF3 ? "rep movsd" : "movsd");
        case 0xAA: return out(in.pF3 ? "rep stosb" : "stosb");
        case 0xAB: return out(in.pF3 ? "rep stosd" : "stosd");
        case 0xAE: return out(in.pF2 ? "repne scasb" : "scasb");
        case 0xF6: case 0xF7: {
            static const char* n[8] = {"test", "test", "not", "neg", "mul", "imul", "div", "idiv"};
            int sub = (in.modrm >> 3) & 7;
            std::string o = rmOperand(in, op == 0xF6 ? 1 : sz);
            if (sub <= 1) return out(strFormat("%s %s, 0x%llx", n[sub], o.c_str(),
                                               (unsigned long long)in.imm));
            return out(std::string(n[sub]) + " " + o);
        }
        case 0xFF: {
            static const char* n[8] = {"inc", "dec", "call", "callf", "jmp", "jmpf", "push", "?"};
            int sub = (in.modrm >> 3) & 7;
            return out(std::string(n[sub]) + " " + rmOperand(in, sub >= 2 ? 8 : sz));
        }
        default: break;
    }

    if ((op & 0xFF00) == 0x0F00) {
        uint8_t o = static_cast<uint8_t>(op & 0xFF);
        if (o >= 0x80 && o <= 0x8F)
            return out(strFormat("j%s 0x%llx", ccName(o - 0x80),
                                 (unsigned long long)(in.rip + in.len + in.imm)));
        if (o >= 0x90 && o <= 0x9F)
            return out(strFormat("set%s %s", ccName(o - 0x90), rmOperand(in, 1).c_str()));
        if (o >= 0x40 && o <= 0x4F)
            return out(strFormat("cmov%s %s, %s", ccName(o - 0x40),
                                 regName(in.regField, sz, in.rex), rmOperand(in, sz).c_str()));
        switch (o) {
            case 0xB6: return out(std::string("movzx ") + regName(in.regField, sz, in.rex) + ", " +
                                  rmOperand(in, 1));
            case 0xB7: return out(std::string("movzx ") + regName(in.regField, sz, in.rex) + ", " +
                                  rmOperand(in, 2));
            case 0xBE: return out(std::string("movsx ") + regName(in.regField, sz, in.rex) + ", " +
                                  rmOperand(in, 1));
            case 0xBF: return out(std::string("movsx ") + regName(in.regField, sz, in.rex) + ", " +
                                  rmOperand(in, 2));
            case 0xAF: return out(std::string("imul ") + regName(in.regField, sz, in.rex) + ", " +
                                  rmOperand(in, sz));
            case 0x1F: return out("nop " + rmOperand(in, sz));
            case 0x05: return out("syscall");
            case 0x0B: return out("ud2");
            case 0xA2: return out("cpuid");
            case 0x31: return out("rdtsc");
            case 0x10: return out(strFormat("mov%s xmm%u, %s",
                                            in.pF3 ? "ss" : in.pF2 ? "sd" : "ups", in.regField,
                                            rmOperand(in, 16).c_str()));
            case 0x11: return out(strFormat("mov%s %s, xmm%u",
                                            in.pF3 ? "ss" : in.pF2 ? "sd" : "ups",
                                            rmOperand(in, 16).c_str(), in.regField));
            case 0x6F: return out(strFormat("movdq%c xmm%u, %s", in.p66 ? 'a' : 'u', in.regField,
                                            rmOperand(in, 16).c_str()));
            case 0x7F: return out(strFormat("movdq%c %s, xmm%u", in.p66 ? 'a' : 'u',
                                            rmOperand(in, 16).c_str(), in.regField));
            case 0xEF: return out(strFormat("pxor xmm%u, %s", in.regField,
                                            rmOperand(in, 16).c_str()));
            case 0x74: return out(strFormat("pcmpeqb xmm%u, %s", in.regField,
                                            rmOperand(in, 16).c_str()));
            case 0xD7: return out(strFormat("pmovmskb %s, xmm%u", regName(in.regField, 4, in.rex),
                                            in.rmField));
            default: break;
        }
        return out(strFormat("(0f %02x) %s", o, in.hasModrm ? rmOperand(in, sz).c_str() : ""));
    }

    return out(strFormat("(opcode 0x%x)", op));
}

} // namespace macemu
