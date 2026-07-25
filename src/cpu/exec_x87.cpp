// macemu - x87 FPU-subset (Fase 1, uitbreiding)
//
// Waarom dit nodig is: ook 64-bit code die zelf geen floating point gebruikt
// komt x87 tegen. De MinGW-w64 CRT doet bij het opstarten een FNINIT, en
// `long double` is op Windows/x86-64 nog steeds 80-bit x87.
//
// BELANGRIJKE AFWIJKING: de registerstack is hier `double` (64-bit), niet de
// echte 80-bit extended precision. Laden en opslaan van m80fp converteert
// heen en weer, dus het *formaat* klopt, maar tussenberekeningen hebben 53 in
// plaats van 64 bits mantisse. Voor normale programma's onzichtbaar; voor code
// die bewust op 80-bit precisie leunt niet. Echte 80-bit vraagt software-
// emulatie van extended precision - dat is een apart project.
#include <cmath>
#include <cstring>

#include "macemu/cpu.h"

namespace macemu {

namespace {

// Status word bits
constexpr uint16_t SW_C0 = 1u << 8;
constexpr uint16_t SW_C1 = 1u << 9;
constexpr uint16_t SW_C2 = 1u << 10;
constexpr uint16_t SW_C3 = 1u << 14;

// --- 80-bit extended <-> double ------------------------------------------
double f80ToDouble(const uint8_t b[10]) {
    uint64_t mant;
    uint16_t se;
    std::memcpy(&mant, b, 8);
    std::memcpy(&se, b + 8, 2);
    int sign = (se >> 15) & 1;
    int exp = se & 0x7FFF;

    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7FFF) {
        if (mant << 1) return std::nan("");
        return sign ? -INFINITY : INFINITY;
    }
    // waarde = mant * 2^(exp - 16383 - 63), waarbij bit 63 de expliciete
    // integer-bit is.
    double v = std::ldexp(static_cast<double>(mant), exp - 16383 - 63);
    return sign ? -v : v;
}

void doubleToF80(double d, uint8_t out[10]) {
    uint64_t bits;
    std::memcpy(&bits, &d, 8);
    uint64_t sign = bits >> 63;
    int exp = static_cast<int>((bits >> 52) & 0x7FF);
    uint64_t frac = bits & 0xFFFFFFFFFFFFFull;

    uint16_t se;
    uint64_t mant;

    if (exp == 0 && frac == 0) {            // nul
        se = static_cast<uint16_t>(sign << 15);
        mant = 0;
    } else if (exp == 0x7FF) {              // inf / NaN
        se = static_cast<uint16_t>((sign << 15) | 0x7FFF);
        mant = frac ? ((1ull << 63) | (frac << 11)) : (1ull << 63);
    } else if (exp == 0) {                  // subnormaal: normaliseren
        int shift = 0;
        while (!(frac & (1ull << 52))) {
            frac <<= 1;
            ++shift;
        }
        int e = 1 - shift - 1023 + 16383;
        se = static_cast<uint16_t>((sign << 15) | (e & 0x7FFF));
        mant = (frac << 11) | (1ull << 63);
    } else {                                 // normaal
        int e = exp - 1023 + 16383;
        se = static_cast<uint16_t>((sign << 15) | (e & 0x7FFF));
        mant = (1ull << 63) | (frac << 11);
    }
    std::memcpy(out, &mant, 8);
    std::memcpy(out + 8, &se, 2);
}

} // namespace

// ---------------------------------------------------------------------------
// Stack-beheer
// ---------------------------------------------------------------------------
void Cpu::fpuInit() {
    for (int i = 0; i < 8; ++i) {
        fpuStack[i] = 0.0;
        fpuTagValid[i] = false;
    }
    fpuTop = 0;
    fpuControl = 0x037F;
    fpuStatus = 0;
}

void Cpu::fpuPush(double v) {
    fpuTop = (fpuTop - 1) & 7;
    fpuStack[fpuTop] = v;
    fpuTagValid[fpuTop] = true;
}

double Cpu::fpuPop() {
    double v = fpuStack[fpuTop];
    fpuTagValid[fpuTop] = false;
    fpuTop = (fpuTop + 1) & 7;
    return v;
}

void Cpu::fpuSetCompareFlags(double a, double b) {
    fpuStatus &= static_cast<uint16_t>(~(SW_C0 | SW_C2 | SW_C3));
    if (std::isnan(a) || std::isnan(b)) fpuStatus |= SW_C0 | SW_C2 | SW_C3;
    else if (a > b) { /* alle drie 0 */ }
    else if (a < b) fpuStatus |= SW_C0;
    else fpuStatus |= SW_C3;
}

// ---------------------------------------------------------------------------
// De uitvoerder
// ---------------------------------------------------------------------------
bool Cpu::executeX87(const Instr& in) {
    uint8_t op = static_cast<uint8_t>(in.opcode);
    if (op < 0xD8 || op > 0xDF) return false;

    const uint8_t modrm = in.modrm;
    const int sub = (modrm >> 3) & 7;
    const int sti = modrm & 7;

    // Afronden volgens de RC-bits van het control word.
    auto roundToInt = [&](double v) -> int64_t {
        switch ((fpuControl >> 10) & 3) {
            case 1: return static_cast<int64_t>(std::floor(v));
            case 2: return static_cast<int64_t>(std::ceil(v));
            case 3: return static_cast<int64_t>(std::trunc(v));
            default: return static_cast<int64_t>(std::rint(v)); // nearest-even
        }
    };

    // --- rekenkundige kern, gedeeld door de geheugen- en registervormen ---
    auto arith = [&](int which, double a, double b) -> double {
        switch (which) {
            case 0: return a + b;   // ADD
            case 1: return a * b;   // MUL
            case 4: return a - b;   // SUB
            case 5: return b - a;   // SUBR
            case 6: return a / b;   // DIV
            case 7: return b / a;   // DIVR
            default: return a;
        }
    };

    // =======================================================================
    // Geheugenvormen (mod != 3)
    // =======================================================================
    if (!in.rmIsReg) {
        uint64_t ea = effectiveAddress(in);

        auto loadF32 = [&]() { float f; uint32_t b = mem.read32(ea); std::memcpy(&f, &b, 4); return static_cast<double>(f); };
        auto loadF64 = [&]() { double d; uint64_t b = mem.read64(ea); std::memcpy(&d, &b, 8); return d; };
        auto storeF32 = [&](double v) { float f = static_cast<float>(v); uint32_t b; std::memcpy(&b, &f, 4); mem.write32(ea, b); };
        auto storeF64 = [&](double v) { uint64_t b; std::memcpy(&b, &v, 8); mem.write64(ea, b); };

        switch (op) {
            case 0xD8: { // m32fp aritmetiek met ST(0)
                double m = loadF32();
                if (sub == 2 || sub == 3) { // FCOM / FCOMP
                    fpuSetCompareFlags(st(0), m);
                    if (sub == 3) fpuPop();
                } else {
                    st(0) = arith(sub, st(0), m);
                }
                return true;
            }
            case 0xDC: { // m64fp aritmetiek met ST(0)
                double m = loadF64();
                if (sub == 2 || sub == 3) {
                    fpuSetCompareFlags(st(0), m);
                    if (sub == 3) fpuPop();
                } else {
                    st(0) = arith(sub, st(0), m);
                }
                return true;
            }
            case 0xD9:
                switch (sub) {
                    case 0: fpuPush(loadF32()); return true;                 // FLD m32fp
                    case 2: storeF32(st(0)); return true;                    // FST m32fp
                    case 3: storeF32(fpuPop()); return true;                 // FSTP m32fp
                    case 4: return true;                                     // FLDENV (genegeerd)
                    case 5: fpuControl = mem.read16(ea); return true;        // FLDCW
                    case 6:                                                  // FNSTENV
                        mem.fill(ea, 0, 28);
                        mem.write16(ea, fpuControl);
                        mem.write16(ea + 4, fpuStatus);
                        return true;
                    case 7: mem.write16(ea, fpuControl); return true;        // FNSTCW
                    default: return false;
                }
            case 0xDB:
                switch (sub) {
                    case 0: fpuPush(static_cast<double>(static_cast<int32_t>(mem.read32(ea))));
                        return true;                                         // FILD m32int
                    case 1: mem.write32(ea, static_cast<uint32_t>(static_cast<int32_t>(
                                std::trunc(fpuPop()))));
                        return true;                                         // FISTTP m32int
                    case 2: mem.write32(ea, static_cast<uint32_t>(
                                static_cast<int32_t>(roundToInt(st(0)))));
                        return true;                                         // FIST m32int
                    case 3: mem.write32(ea, static_cast<uint32_t>(
                                static_cast<int32_t>(roundToInt(fpuPop()))));
                        return true;                                         // FISTP m32int
                    case 5: {                                                // FLD m80fp
                        uint8_t b[10];
                        mem.readBlock(ea, b, 10);
                        fpuPush(f80ToDouble(b));
                        return true;
                    }
                    case 7: {                                                // FSTP m80fp
                        uint8_t b[10];
                        doubleToF80(fpuPop(), b);
                        mem.writeBlock(ea, b, 10);
                        return true;
                    }
                    default: return false;
                }
            case 0xDD:
                switch (sub) {
                    case 0: fpuPush(loadF64()); return true;                 // FLD m64fp
                    case 1: mem.write64(ea, static_cast<uint64_t>(
                                static_cast<int64_t>(std::trunc(fpuPop()))));
                        return true;                                         // FISTTP m64int
                    case 2: storeF64(st(0)); return true;                    // FST m64fp
                    case 3: storeF64(fpuPop()); return true;                 // FSTP m64fp
                    case 4: return true;                                     // FRSTOR (genegeerd)
                    case 6: mem.fill(ea, 0, 108); return true;               // FNSAVE
                    case 7:                                                  // FNSTSW m16
                        mem.write16(ea, static_cast<uint16_t>(
                            (fpuStatus & ~0x3800) | ((fpuTop & 7) << 11)));
                        return true;
                    default: return false;
                }
            case 0xDA: { // m32int aritmetiek
                double m = static_cast<double>(static_cast<int32_t>(mem.read32(ea)));
                if (sub == 2 || sub == 3) {
                    fpuSetCompareFlags(st(0), m);
                    if (sub == 3) fpuPop();
                } else {
                    st(0) = arith(sub, st(0), m);
                }
                return true;
            }
            case 0xDE: { // m16int aritmetiek
                double m = static_cast<double>(static_cast<int16_t>(mem.read16(ea)));
                if (sub == 2 || sub == 3) {
                    fpuSetCompareFlags(st(0), m);
                    if (sub == 3) fpuPop();
                } else {
                    st(0) = arith(sub, st(0), m);
                }
                return true;
            }
            case 0xDF:
                switch (sub) {
                    case 0: fpuPush(static_cast<double>(static_cast<int16_t>(mem.read16(ea))));
                        return true;                                         // FILD m16int
                    case 1: mem.write16(ea, static_cast<uint16_t>(
                                static_cast<int16_t>(std::trunc(fpuPop()))));
                        return true;                                         // FISTTP m16int
                    case 2: mem.write16(ea, static_cast<uint16_t>(
                                static_cast<int16_t>(roundToInt(st(0)))));
                        return true;                                         // FIST m16int
                    case 3: mem.write16(ea, static_cast<uint16_t>(
                                static_cast<int16_t>(roundToInt(fpuPop()))));
                        return true;                                         // FISTP m16int
                    case 5: fpuPush(static_cast<double>(
                                static_cast<int64_t>(mem.read64(ea))));
                        return true;                                         // FILD m64int
                    case 7: mem.write64(ea, static_cast<uint64_t>(roundToInt(fpuPop())));
                        return true;                                         // FISTP m64int
                    default: return false;
                }
            default:
                return false;
        }
    }

    // =======================================================================
    // Registervormen (mod == 3): de hele ModRM-byte selecteert de instructie
    // =======================================================================
    switch (op) {
        case 0xD8: { // ST(0) = ST(0) op ST(i)
            double b = stValue(sti);
            switch (modrm & 0xF8) {
                case 0xC0: st(0) = st(0) + b; return true;  // FADD
                case 0xC8: st(0) = st(0) * b; return true;  // FMUL
                case 0xD0: fpuSetCompareFlags(st(0), b); return true;            // FCOM
                case 0xD8: fpuSetCompareFlags(st(0), b); fpuPop(); return true;  // FCOMP
                case 0xE0: st(0) = st(0) - b; return true;  // FSUB
                case 0xE8: st(0) = b - st(0); return true;  // FSUBR
                case 0xF0: st(0) = st(0) / b; return true;  // FDIV
                case 0xF8: st(0) = b / st(0); return true;  // FDIVR
                default: return false;
            }
        }
        case 0xD9: {
            if ((modrm & 0xF8) == 0xC0) { fpuPush(stValue(sti)); return true; }   // FLD ST(i)
            if ((modrm & 0xF8) == 0xC8) {                                         // FXCH ST(i)
                double t = st(0);
                st(0) = st(sti);
                st(sti) = t;
                return true;
            }
            switch (modrm) {
                case 0xD0: return true;                                  // FNOP
                case 0xE0: st(0) = -st(0); return true;                  // FCHS
                case 0xE1: st(0) = std::fabs(st(0)); return true;        // FABS
                case 0xE4: fpuSetCompareFlags(st(0), 0.0); return true;  // FTST
                case 0xE5:                                               // FXAM
                    fpuStatus &= static_cast<uint16_t>(~(SW_C0 | SW_C1 | SW_C2 | SW_C3));
                    if (std::signbit(st(0))) fpuStatus |= SW_C1;
                    if (std::isnan(st(0))) fpuStatus |= SW_C0;
                    else if (std::isinf(st(0))) fpuStatus |= SW_C0 | SW_C2;
                    else if (st(0) == 0.0) fpuStatus |= SW_C3;
                    else fpuStatus |= SW_C2;
                    return true;
                case 0xE8: fpuPush(1.0); return true;                              // FLD1
                case 0xE9: fpuPush(3.321928094887362348); return true;             // FLDL2T
                case 0xEA: fpuPush(1.442695040888963407); return true;             // FLDL2E
                case 0xEB: fpuPush(3.141592653589793239); return true;             // FLDPI
                case 0xEC: fpuPush(0.301029995663981195); return true;             // FLDLG2
                case 0xED: fpuPush(0.693147180559945309); return true;             // FLDLN2
                case 0xEE: fpuPush(0.0); return true;                              // FLDZ
                case 0xF0: st(0) = std::exp2(st(0)) - 1.0; return true;            // F2XM1
                case 0xF1: { double y = fpuPop(); st(0) = y * std::log2(st(0)); return true; } // FYL2X
                case 0xF2: { double t = std::tan(st(0)); st(0) = t; fpuPush(1.0);
                             fpuStatus &= static_cast<uint16_t>(~SW_C2); return true; }        // FPTAN
                case 0xF3: { double x = fpuPop(); st(0) = std::atan2(x, st(0)); return true; } // FPATAN
                case 0xF8: { double b = stValue(1); st(0) = std::fmod(st(0), b);
                             fpuStatus &= static_cast<uint16_t>(~SW_C2); return true; }        // FPREM
                case 0xF9: { double y = fpuPop(); st(0) = y * std::log2(st(0) + 1.0); return true; } // FYL2XP1
                case 0xFA: st(0) = std::sqrt(st(0)); return true;                  // FSQRT
                case 0xFB: { double s = std::sin(st(0)), c = std::cos(st(0));
                             st(0) = s; fpuPush(c); return true; }                 // FSINCOS
                case 0xFC: st(0) = static_cast<double>(roundToInt(st(0))); return true; // FRNDINT
                case 0xFD: { double e = stValue(1);
                             st(0) = std::ldexp(st(0), static_cast<int>(std::trunc(e)));
                             return true; }                                        // FSCALE
                case 0xFE: st(0) = std::sin(st(0)); return true;                   // FSIN
                case 0xFF: st(0) = std::cos(st(0)); return true;                   // FCOS
                default: return false;
            }
        }
        case 0xDA: {
            if (modrm == 0xE9) { // FUCOMPP
                fpuSetCompareFlags(st(0), stValue(1));
                fpuPop();
                fpuPop();
                return true;
            }
            // FCMOVB/FCMOVE/FCMOVBE/FCMOVU
            bool cond = false;
            switch (modrm & 0xF8) {
                case 0xC0: cond = flags.cf; break;
                case 0xC8: cond = flags.zf; break;
                case 0xD0: cond = flags.cf || flags.zf; break;
                case 0xD8: cond = flags.pf; break;
                default: return false;
            }
            if (cond) st(0) = stValue(sti);
            return true;
        }
        case 0xDB: {
            if (modrm == 0xE2) { fpuStatus &= 0x7F00; return true; }  // FNCLEX
            if (modrm == 0xE3) { fpuInit(); return true; }            // FNINIT
            if ((modrm & 0xF8) == 0xE8 || (modrm & 0xF8) == 0xF0) {   // FUCOMI / FCOMI
                double a = st(0), b = stValue(sti);
                flags.of = flags.af = flags.sf = false;
                if (std::isnan(a) || std::isnan(b)) { flags.zf = flags.pf = flags.cf = true; }
                else { flags.pf = false; flags.zf = (a == b); flags.cf = (a < b); }
                return true;
            }
            // FCMOVNB/FCMOVNE/FCMOVNBE/FCMOVNU
            bool cond = false;
            switch (modrm & 0xF8) {
                case 0xC0: cond = !flags.cf; break;
                case 0xC8: cond = !flags.zf; break;
                case 0xD0: cond = !flags.cf && !flags.zf; break;
                case 0xD8: cond = !flags.pf; break;
                default: return false;
            }
            if (cond) st(0) = stValue(sti);
            return true;
        }
        case 0xDC: { // ST(i) = ST(i) op ST(0)  -- let op de omgekeerde mnemonics
            switch (modrm & 0xF8) {
                case 0xC0: st(sti) = st(sti) + st(0); return true;  // FADD
                case 0xC8: st(sti) = st(sti) * st(0); return true;  // FMUL
                case 0xE0: st(sti) = st(0) - st(sti); return true;  // FSUBR
                case 0xE8: st(sti) = st(sti) - st(0); return true;  // FSUB
                case 0xF0: st(sti) = st(0) / st(sti); return true;  // FDIVR
                case 0xF8: st(sti) = st(sti) / st(0); return true;  // FDIV
                default: return false;
            }
        }
        case 0xDD: {
            switch (modrm & 0xF8) {
                case 0xC0: fpuTagValid[(fpuTop + sti) & 7] = false; return true;   // FFREE
                case 0xD0: st(sti) = st(0); return true;                           // FST ST(i)
                case 0xD8: st(sti) = st(0); fpuPop(); return true;                 // FSTP ST(i)
                case 0xE0: fpuSetCompareFlags(st(0), stValue(sti)); return true;   // FUCOM
                case 0xE8: fpuSetCompareFlags(st(0), stValue(sti)); fpuPop();
                    return true;                                                    // FUCOMP
                default: return false;
            }
        }
        case 0xDE: {
            if (modrm == 0xD9) { // FCOMPP
                fpuSetCompareFlags(st(0), stValue(1));
                fpuPop();
                fpuPop();
                return true;
            }
            switch (modrm & 0xF8) {
                case 0xC0: st(sti) = st(sti) + st(0); fpuPop(); return true;  // FADDP
                case 0xC8: st(sti) = st(sti) * st(0); fpuPop(); return true;  // FMULP
                case 0xE0: st(sti) = st(0) - st(sti); fpuPop(); return true;  // FSUBRP
                case 0xE8: st(sti) = st(sti) - st(0); fpuPop(); return true;  // FSUBP
                case 0xF0: st(sti) = st(0) / st(sti); fpuPop(); return true;  // FDIVRP
                case 0xF8: st(sti) = st(sti) / st(0); fpuPop(); return true;  // FDIVP
                default: return false;
            }
        }
        case 0xDF: {
            if (modrm == 0xE0) { // FNSTSW AX
                writeGpr(RAX, 2, static_cast<uint16_t>((fpuStatus & ~0x3800) |
                                                       ((fpuTop & 7) << 11)));
                return true;
            }
            if ((modrm & 0xF8) == 0xE8 || (modrm & 0xF8) == 0xF0) { // FUCOMIP / FCOMIP
                double a = st(0), b = stValue(sti);
                flags.of = flags.af = flags.sf = false;
                if (std::isnan(a) || std::isnan(b)) { flags.zf = flags.pf = flags.cf = true; }
                else { flags.pf = false; flags.zf = (a == b); flags.cf = (a < b); }
                fpuPop();
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

} // namespace macemu
