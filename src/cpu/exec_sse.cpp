// macemu - SSE/SSE2-subset (Fase 1)
//
// Waarom dit nodig is: 64-bit MSVC-code gebruikt SSE2 voor *alles* met
// floating point, en de CRT gebruikt SSE2 ook voor strlen/memset/memcmp
// (pcmpeqb + pmovmskb). Zonder deze instructies kom je geen enkele echte
// Windows-executable door het opstarten heen.
//
// Niet geïmplementeerd (bewust): AVX/AVX2/AVX-512. De CPUID-uitvoer meldt die
// features dan ook niet, zodat de gast een SSE2-pad kiest.
#include <cmath>
#include <cstring>

#include "macemu/cpu.h"

namespace macemu {

namespace {

inline float bitsToF32(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
inline uint32_t f32ToBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline double bitsToF64(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }
inline uint64_t f64ToBits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

inline uint8_t xbyte(const Xmm& x, int i) {
    return static_cast<uint8_t>(((i < 8) ? (x.lo >> (i * 8)) : (x.hi >> ((i - 8) * 8))) & 0xFF);
}
inline void setByte(Xmm& x, int i, uint8_t v) {
    if (i < 8) x.lo = (x.lo & ~(0xFFull << (i * 8))) | (static_cast<uint64_t>(v) << (i * 8));
    else x.hi = (x.hi & ~(0xFFull << ((i - 8) * 8))) | (static_cast<uint64_t>(v) << ((i - 8) * 8));
}
inline uint16_t xword(const Xmm& x, int i) {
    return static_cast<uint16_t>(((i < 4) ? (x.lo >> (i * 16)) : (x.hi >> ((i - 4) * 16))) & 0xFFFF);
}
inline void setWord(Xmm& x, int i, uint16_t v) {
    if (i < 4) x.lo = (x.lo & ~(0xFFFFull << (i * 16))) | (static_cast<uint64_t>(v) << (i * 16));
    else x.hi = (x.hi & ~(0xFFFFull << ((i - 4) * 16))) |
                (static_cast<uint64_t>(v) << ((i - 4) * 16));
}
inline uint32_t xdword(const Xmm& x, int i) {
    return static_cast<uint32_t>(((i < 2) ? (x.lo >> (i * 32)) : (x.hi >> ((i - 2) * 32))) &
                                 0xFFFFFFFF);
}
inline void setDword(Xmm& x, int i, uint32_t v) {
    if (i < 2) x.lo = (x.lo & ~(0xFFFFFFFFull << (i * 32))) | (static_cast<uint64_t>(v) << (i * 32));
    else x.hi = (x.hi & ~(0xFFFFFFFFull << ((i - 2) * 32))) |
                (static_cast<uint64_t>(v) << ((i - 2) * 32));
}
inline uint64_t xqword(const Xmm& x, int i) { return i == 0 ? x.lo : x.hi; }
inline void setQword(Xmm& x, int i, uint64_t v) { (i == 0 ? x.lo : x.hi) = v; }

} // namespace

Xmm Cpu::readXmmRm(const Instr& in) const {
    if (in.rmIsReg) return xmm[in.rmField];
    uint64_t ea = effectiveAddress(in);
    Xmm v;
    v.lo = mem.read64(ea);
    v.hi = mem.read64(ea + 8);
    return v;
}

void Cpu::writeXmmRm(const Instr& in, const Xmm& v) {
    if (in.rmIsReg) {
        xmm[in.rmField] = v;
        return;
    }
    uint64_t ea = effectiveAddress(in);
    mem.write64(ea, v.lo);
    mem.write64(ea + 8, v.hi);
}

bool Cpu::executeSse(const Instr& in) {
    if ((in.opcode & 0xFF00) == 0x3800) {
        uint8_t o = static_cast<uint8_t>(in.opcode & 0xFF);
        Xmm& d = xmm[in.regField];
        Xmm s = readXmmRm(in);
        switch (o) {
            case 0x00: { // PSHUFB
                Xmm r = d;
                for (int i = 0; i < 16; ++i) {
                    uint8_t sel = xbyte(s, i);
                    setByte(r, i, (sel & 0x80) ? 0 : xbyte(d, sel & 0x0F));
                }
                d = r;
                return true;
            }
            case 0x17: { // PTEST
                flags.zf = ((d.lo & s.lo) | (d.hi & s.hi)) == 0;
                flags.cf = ((~d.lo & s.lo) | (~d.hi & s.hi)) == 0;
                flags.of = flags.af = flags.sf = flags.pf = false;
                return true;
            }
            case 0x29: { // PCMPEQQ
                d.lo = (d.lo == s.lo) ? ~0ull : 0;
                d.hi = (d.hi == s.hi) ? ~0ull : 0;
                return true;
            }
            default:
                return false;
        }
    }
    if ((in.opcode & 0xFF00) != 0x0F00) return false;

    uint8_t op = static_cast<uint8_t>(in.opcode & 0xFF);
    const bool p66 = in.p66, pF3 = in.pF3, pF2 = in.pF2;

    switch (op) {
        // ------------------------------------------------------- verplaatsen
        case 0x10: { // MOVUPS / MOVUPD / MOVSS / MOVSD (load)
            Xmm& d = xmm[in.regField];
            if (pF3) {
                if (in.rmIsReg) setDword(d, 0, xdword(xmm[in.rmField], 0));
                else { d.lo = mem.read32(effectiveAddress(in)); d.hi = 0; }
            } else if (pF2) {
                if (in.rmIsReg) d.lo = xmm[in.rmField].lo;
                else { d.lo = mem.read64(effectiveAddress(in)); d.hi = 0; }
            } else {
                d = readXmmRm(in);
            }
            return true;
        }
        case 0x11: { // store-variant
            const Xmm& s = xmm[in.regField];
            if (pF3) {
                if (in.rmIsReg) setDword(xmm[in.rmField], 0, xdword(s, 0));
                else mem.write32(effectiveAddress(in), xdword(s, 0));
            } else if (pF2) {
                if (in.rmIsReg) xmm[in.rmField].lo = s.lo;
                else mem.write64(effectiveAddress(in), s.lo);
            } else {
                writeXmmRm(in, s);
            }
            return true;
        }
        case 0x12: { // MOVLPS/MOVLPD load (of MOVHLPS bij reg-operand)
            Xmm& d = xmm[in.regField];
            if (in.rmIsReg) d.lo = xmm[in.rmField].hi; // MOVHLPS
            else d.lo = mem.read64(effectiveAddress(in));
            return true;
        }
        case 0x13: // MOVLPS store
            mem.write64(effectiveAddress(in), xmm[in.regField].lo);
            return true;
        case 0x16: { // MOVHPS load (of MOVLHPS)
            Xmm& d = xmm[in.regField];
            if (in.rmIsReg) d.hi = xmm[in.rmField].lo;
            else d.hi = mem.read64(effectiveAddress(in));
            return true;
        }
        case 0x17: // MOVHPS store
            mem.write64(effectiveAddress(in), xmm[in.regField].hi);
            return true;
        case 0x28: xmm[in.regField] = readXmmRm(in); return true;          // MOVAPS/MOVAPD
        case 0x29: writeXmmRm(in, xmm[in.regField]); return true;
        case 0x6F: xmm[in.regField] = readXmmRm(in); return true;          // MOVDQA/MOVDQU
        case 0x7F: writeXmmRm(in, xmm[in.regField]); return true;

        case 0x6E: { // MOVD/MOVQ xmm, r/m
            Xmm& d = xmm[in.regField];
            if (in.rexW) { d.lo = readRm(in, 8); d.hi = 0; }
            else { d.lo = readRm(in, 4); d.hi = 0; }
            return true;
        }
        case 0x7E: { // MOVQ xmm,xmm/m64 (F3) of MOVD/MOVQ r/m, xmm
            if (pF3) {
                Xmm& d = xmm[in.regField];
                if (in.rmIsReg) d.lo = xmm[in.rmField].lo;
                else d.lo = mem.read64(effectiveAddress(in));
                d.hi = 0;
                return true;
            }
            const Xmm& s = xmm[in.regField];
            if (in.rexW) writeRm(in, 8, s.lo);
            else writeRm(in, 4, xdword(s, 0));
            return true;
        }
        case 0xD6: { // MOVQ xmm/m64, xmm
            const Xmm& s = xmm[in.regField];
            if (in.rmIsReg) { xmm[in.rmField].lo = s.lo; xmm[in.rmField].hi = 0; }
            else mem.write64(effectiveAddress(in), s.lo);
            return true;
        }
        case 0x2B: writeXmmRm(in, xmm[in.regField]); return true; // MOVNTPS

        // ----------------------------------------------------------- logisch
        case 0x54: { Xmm s = readXmmRm(in); xmm[in.regField].lo &= s.lo;
                     xmm[in.regField].hi &= s.hi; return true; } // ANDPS/PD
        case 0x55: { Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
                     d.lo = ~d.lo & s.lo; d.hi = ~d.hi & s.hi; return true; } // ANDNPS
        case 0x56: { Xmm s = readXmmRm(in); xmm[in.regField].lo |= s.lo;
                     xmm[in.regField].hi |= s.hi; return true; } // ORPS
        case 0x57: { Xmm s = readXmmRm(in); xmm[in.regField].lo ^= s.lo;
                     xmm[in.regField].hi ^= s.hi; return true; } // XORPS
        case 0xDB: { Xmm s = readXmmRm(in); xmm[in.regField].lo &= s.lo;
                     xmm[in.regField].hi &= s.hi; return true; } // PAND
        case 0xDF: { Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
                     d.lo = ~d.lo & s.lo; d.hi = ~d.hi & s.hi; return true; } // PANDN
        case 0xEB: { Xmm s = readXmmRm(in); xmm[in.regField].lo |= s.lo;
                     xmm[in.regField].hi |= s.hi; return true; } // POR
        case 0xEF: { Xmm s = readXmmRm(in); xmm[in.regField].lo ^= s.lo;
                     xmm[in.regField].hi ^= s.hi; return true; } // PXOR

        // ------------------------------------------------------ vergelijking
        case 0x74: { // PCMPEQB
            Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 16; ++i) setByte(d, i, xbyte(d, i) == xbyte(s, i) ? 0xFF : 0x00);
            return true;
        }
        case 0x75: { // PCMPEQW
            Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 8; ++i) setWord(d, i, xword(d, i) == xword(s, i) ? 0xFFFF : 0);
            return true;
        }
        case 0x76: { // PCMPEQD
            Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 4; ++i)
                setDword(d, i, xdword(d, i) == xdword(s, i) ? 0xFFFFFFFFu : 0);
            return true;
        }
        case 0x64: { // PCMPGTB (signed)
            Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 16; ++i)
                setByte(d, i, static_cast<int8_t>(xbyte(d, i)) > static_cast<int8_t>(xbyte(s, i))
                                  ? 0xFF : 0x00);
            return true;
        }
        case 0x65: { // PCMPGTW
            Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 8; ++i)
                setWord(d, i, static_cast<int16_t>(xword(d, i)) > static_cast<int16_t>(xword(s, i))
                                  ? 0xFFFF : 0);
            return true;
        }
        case 0x66: { // PCMPGTD
            Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 4; ++i)
                setDword(d, i,
                         static_cast<int32_t>(xdword(d, i)) > static_cast<int32_t>(xdword(s, i))
                             ? 0xFFFFFFFFu : 0);
            return true;
        }
        case 0xD7: { // PMOVMSKB
            const Xmm& s = xmm[in.rmField];
            uint32_t m = 0;
            for (int i = 0; i < 16; ++i)
                if (xbyte(s, i) & 0x80) m |= (1u << i);
            writeGpr(in.regField, 8, m);
            return true;
        }
        case 0x50: { // MOVMSKPS / MOVMSKPD
            const Xmm& s = xmm[in.rmField];
            uint32_t m = 0;
            if (p66) {
                if (s.lo >> 63) m |= 1;
                if (s.hi >> 63) m |= 2;
            } else {
                for (int i = 0; i < 4; ++i)
                    if (xdword(s, i) & 0x80000000u) m |= (1u << i);
            }
            writeGpr(in.regField, 8, m);
            return true;
        }
        case 0xDA: { // PMINUB
            Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 16; ++i)
                setByte(d, i, xbyte(d, i) < xbyte(s, i) ? xbyte(d, i) : xbyte(s, i));
            return true;
        }
        case 0xDE: { // PMAXUB
            Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 16; ++i)
                setByte(d, i, xbyte(d, i) > xbyte(s, i) ? xbyte(d, i) : xbyte(s, i));
            return true;
        }

        // ------------------------------------------------- integer-rekenwerk
        case 0xFC: { Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 16; ++i) setByte(d, i, static_cast<uint8_t>(xbyte(d, i) + xbyte(s, i)));
            return true; } // PADDB
        case 0xFD: { Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 8; ++i) setWord(d, i, static_cast<uint16_t>(xword(d, i) + xword(s, i)));
            return true; } // PADDW
        case 0xFE: { Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 4; ++i) setDword(d, i, xdword(d, i) + xdword(s, i));
            return true; } // PADDD
        case 0xD4: { Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            d.lo += s.lo; d.hi += s.hi; return true; } // PADDQ
        case 0xF8: { Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 16; ++i) setByte(d, i, static_cast<uint8_t>(xbyte(d, i) - xbyte(s, i)));
            return true; } // PSUBB
        case 0xF9: { Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 8; ++i) setWord(d, i, static_cast<uint16_t>(xword(d, i) - xword(s, i)));
            return true; } // PSUBW
        case 0xFA: { Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            for (int i = 0; i < 4; ++i) setDword(d, i, xdword(d, i) - xdword(s, i));
            return true; } // PSUBD
        case 0xFB: { Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            d.lo -= s.lo; d.hi -= s.hi; return true; } // PSUBQ

        // ------------------------------------------------------- unpack/shuf
        case 0x60: { // PUNPCKLBW
            Xmm s = readXmmRm(in); Xmm d = xmm[in.regField]; Xmm r{};
            for (int i = 0; i < 8; ++i) {
                setByte(r, i * 2, xbyte(d, i));
                setByte(r, i * 2 + 1, xbyte(s, i));
            }
            xmm[in.regField] = r;
            return true;
        }
        case 0x61: { // PUNPCKLWD
            Xmm s = readXmmRm(in); Xmm d = xmm[in.regField]; Xmm r{};
            for (int i = 0; i < 4; ++i) {
                setWord(r, i * 2, xword(d, i));
                setWord(r, i * 2 + 1, xword(s, i));
            }
            xmm[in.regField] = r;
            return true;
        }
        case 0x62: { // PUNPCKLDQ
            Xmm s = readXmmRm(in); Xmm d = xmm[in.regField]; Xmm r{};
            setDword(r, 0, xdword(d, 0)); setDword(r, 1, xdword(s, 0));
            setDword(r, 2, xdword(d, 1)); setDword(r, 3, xdword(s, 1));
            xmm[in.regField] = r;
            return true;
        }
        case 0x6C: { // PUNPCKLQDQ
            Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            d.hi = s.lo;
            return true;
        }
        case 0x6D: { // PUNPCKHQDQ
            Xmm s = readXmmRm(in); Xmm& d = xmm[in.regField];
            d.lo = d.hi; d.hi = s.hi;
            return true;
        }
        case 0x14: { // UNPCKLPS / UNPCKLPD
            Xmm s = readXmmRm(in); Xmm d = xmm[in.regField]; Xmm r{};
            if (p66) { r.lo = d.lo; r.hi = s.lo; }
            else {
                setDword(r, 0, xdword(d, 0)); setDword(r, 1, xdword(s, 0));
                setDword(r, 2, xdword(d, 1)); setDword(r, 3, xdword(s, 1));
            }
            xmm[in.regField] = r;
            return true;
        }
        case 0x70: { // PSHUFD / PSHUFLW / PSHUFHW
            Xmm s = readXmmRm(in);
            Xmm r = s;
            uint8_t sel = static_cast<uint8_t>(in.imm);
            if (p66) {
                for (int i = 0; i < 4; ++i) setDword(r, i, xdword(s, (sel >> (i * 2)) & 3));
            } else if (pF2) { // PSHUFLW
                for (int i = 0; i < 4; ++i) setWord(r, i, xword(s, (sel >> (i * 2)) & 3));
            } else if (pF3) { // PSHUFHW
                for (int i = 0; i < 4; ++i) setWord(r, 4 + i, xword(s, 4 + ((sel >> (i * 2)) & 3)));
            } else {
                for (int i = 0; i < 4; ++i) setDword(r, i, xdword(s, (sel >> (i * 2)) & 3));
            }
            xmm[in.regField] = r;
            return true;
        }
        case 0xC6: { // SHUFPS / SHUFPD
            Xmm s = readXmmRm(in); Xmm d = xmm[in.regField]; Xmm r{};
            uint8_t sel = static_cast<uint8_t>(in.imm);
            if (p66) {
                r.lo = (sel & 1) ? d.hi : d.lo;
                r.hi = (sel & 2) ? s.hi : s.lo;
            } else {
                setDword(r, 0, xdword(d, sel & 3));
                setDword(r, 1, xdword(d, (sel >> 2) & 3));
                setDword(r, 2, xdword(s, (sel >> 4) & 3));
                setDword(r, 3, xdword(s, (sel >> 6) & 3));
            }
            xmm[in.regField] = r;
            return true;
        }

        // ------------------------------------------------------ shift-groepen
        case 0x71: case 0x72: case 0x73: {
            int sub = (in.modrm >> 3) & 7;
            uint64_t cnt = static_cast<uint64_t>(in.imm) & 0xFF;
            Xmm& d = xmm[in.rmField];
            if (op == 0x73 && (sub == 7 || sub == 3)) { // PSLLDQ / PSRLDQ (bytes!)
                Xmm r{};
                int n = static_cast<int>(cnt);
                if (n > 16) n = 16;
                for (int i = 0; i < 16; ++i) {
                    if (sub == 7) { // links schuiven
                        if (i >= n) setByte(r, i, xbyte(d, i - n));
                    } else {        // rechts schuiven
                        if (i + n < 16) setByte(r, i, xbyte(d, i + n));
                    }
                }
                d = r;
                return true;
            }
            if (op == 0x71) { // words
                for (int i = 0; i < 8; ++i) {
                    uint16_t v = xword(d, i);
                    if (sub == 2) v = cnt >= 16 ? 0 : static_cast<uint16_t>(v >> cnt);
                    else if (sub == 4) {
                        int16_t sv = static_cast<int16_t>(v);
                        v = static_cast<uint16_t>(sv >> (cnt >= 16 ? 15 : cnt));
                    } else if (sub == 6) v = cnt >= 16 ? 0 : static_cast<uint16_t>(v << cnt);
                    setWord(d, i, v);
                }
                return true;
            }
            if (op == 0x72) { // dwords
                for (int i = 0; i < 4; ++i) {
                    uint32_t v = xdword(d, i);
                    if (sub == 2) v = cnt >= 32 ? 0 : (v >> cnt);
                    else if (sub == 4) {
                        int32_t sv = static_cast<int32_t>(v);
                        v = static_cast<uint32_t>(sv >> (cnt >= 32 ? 31 : cnt));
                    } else if (sub == 6) v = cnt >= 32 ? 0 : (v << cnt);
                    setDword(d, i, v);
                }
                return true;
            }
            // op == 0x73: qwords
            for (int i = 0; i < 2; ++i) {
                uint64_t v = xqword(d, i);
                if (sub == 2) v = cnt >= 64 ? 0 : (v >> cnt);
                else if (sub == 6) v = cnt >= 64 ? 0 : (v << cnt);
                setQword(d, i, v);
            }
            return true;
        }

        // -------------------------------------------------------- floats
        case 0x51: case 0x58: case 0x59: case 0x5C:
        case 0x5D: case 0x5E: case 0x5F: {
            Xmm s = readXmmRm(in);
            Xmm& d = xmm[in.regField];
            auto binop = [&](double a, double b) -> double {
                switch (op) {
                    case 0x51: return std::sqrt(b);
                    case 0x58: return a + b;
                    case 0x59: return a * b;
                    case 0x5C: return a - b;
                    case 0x5D: return a < b ? a : b;
                    case 0x5E: return a / b;
                    default: return a > b ? a : b;
                }
            };
            if (pF3) { // scalar single
                float r = static_cast<float>(binop(bitsToF32(xdword(d, 0)),
                                                   bitsToF32(xdword(s, 0))));
                setDword(d, 0, f32ToBits(r));
            } else if (pF2) { // scalar double
                d.lo = f64ToBits(binop(bitsToF64(d.lo), bitsToF64(s.lo)));
            } else if (p66) { // packed double
                d.lo = f64ToBits(binop(bitsToF64(d.lo), bitsToF64(s.lo)));
                d.hi = f64ToBits(binop(bitsToF64(d.hi), bitsToF64(s.hi)));
            } else { // packed single
                for (int i = 0; i < 4; ++i)
                    setDword(d, i, f32ToBits(static_cast<float>(
                                       binop(bitsToF32(xdword(d, i)), bitsToF32(xdword(s, i))))));
            }
            return true;
        }
        case 0x2E: case 0x2F: { // UCOMISS/COMISS (+66 = ...SD)
            Xmm s = readXmmRm(in);
            const Xmm& d = xmm[in.regField];
            double a, b;
            if (p66) { a = bitsToF64(d.lo); b = bitsToF64(s.lo); }
            else { a = bitsToF32(xdword(d, 0)); b = bitsToF32(xdword(s, 0)); }
            flags.of = flags.af = flags.sf = false;
            if (std::isnan(a) || std::isnan(b)) { flags.zf = flags.pf = flags.cf = true; }
            else {
                flags.pf = false;
                flags.zf = (a == b);
                flags.cf = (a < b);
            }
            return true;
        }
        case 0x2A: { // CVTSI2SS / CVTSI2SD
            Xmm& d = xmm[in.regField];
            int64_t v = in.rexW ? static_cast<int64_t>(readRm(in, 8))
                                : static_cast<int32_t>(readRm(in, 4));
            if (pF2) d.lo = f64ToBits(static_cast<double>(v));
            else setDword(d, 0, f32ToBits(static_cast<float>(v)));
            return true;
        }
        case 0x2C: case 0x2D: { // CVTTSS2SI / CVTSS2SI (+F2 = SD)
            Xmm s = readXmmRm(in);
            double v = pF2 ? bitsToF64(s.lo) : static_cast<double>(bitsToF32(xdword(s, 0)));
            int64_t r = (op == 0x2C) ? static_cast<int64_t>(v)
                                     : static_cast<int64_t>(std::nearbyint(v));
            writeGpr(in.regField, in.rexW ? 8 : 4, static_cast<uint64_t>(r));
            return true;
        }
        case 0x5A: { // CVTSS2SD / CVTSD2SS
            Xmm s = readXmmRm(in);
            Xmm& d = xmm[in.regField];
            if (pF3) d.lo = f64ToBits(static_cast<double>(bitsToF32(xdword(s, 0))));
            else if (pF2) setDword(d, 0, f32ToBits(static_cast<float>(bitsToF64(s.lo))));
            else return false;
            return true;
        }
        case 0x5B: { // CVTDQ2PS
            Xmm s = readXmmRm(in);
            Xmm& d = xmm[in.regField];
            for (int i = 0; i < 4; ++i)
                setDword(d, i, f32ToBits(static_cast<float>(static_cast<int32_t>(xdword(s, i)))));
            return true;
        }
        case 0xE6: { // CVTDQ2PD / CVTPD2DQ / CVTTPD2DQ
            Xmm s = readXmmRm(in);
            Xmm& d = xmm[in.regField];
            if (pF3) { // CVTDQ2PD
                double a = static_cast<int32_t>(xdword(s, 0));
                double b = static_cast<int32_t>(xdword(s, 1));
                d.lo = f64ToBits(a);
                d.hi = f64ToBits(b);
            } else {
                setDword(d, 0, static_cast<uint32_t>(static_cast<int32_t>(bitsToF64(s.lo))));
                setDword(d, 1, static_cast<uint32_t>(static_cast<int32_t>(bitsToF64(s.hi))));
                d.hi = 0;
            }
            return true;
        }
        case 0xC2: { // CMPPS/CMPSS/CMPSD
            Xmm s = readXmmRm(in);
            Xmm& d = xmm[in.regField];
            uint8_t pred = static_cast<uint8_t>(in.imm) & 7;
            auto cmp = [&](double a, double b) -> bool {
                switch (pred) {
                    case 0: return a == b;
                    case 1: return a < b;
                    case 2: return a <= b;
                    case 3: return std::isnan(a) || std::isnan(b);
                    case 4: return a != b;
                    case 5: return !(a < b);
                    case 6: return !(a <= b);
                    default: return !(std::isnan(a) || std::isnan(b));
                }
            };
            if (pF3) setDword(d, 0, cmp(bitsToF32(xdword(d, 0)), bitsToF32(xdword(s, 0)))
                                        ? 0xFFFFFFFFu : 0);
            else if (pF2) d.lo = cmp(bitsToF64(d.lo), bitsToF64(s.lo)) ? ~0ull : 0;
            else if (p66) {
                d.lo = cmp(bitsToF64(d.lo), bitsToF64(s.lo)) ? ~0ull : 0;
                d.hi = cmp(bitsToF64(d.hi), bitsToF64(s.hi)) ? ~0ull : 0;
            } else {
                for (int i = 0; i < 4; ++i)
                    setDword(d, i, cmp(bitsToF32(xdword(d, i)), bitsToF32(xdword(s, i)))
                                       ? 0xFFFFFFFFu : 0);
            }
            return true;
        }

        default:
            return false;
    }
}

} // namespace macemu
