// Fase 1-test: de CPU-core met handgeschreven machinecode.
//
// Elke test is een klein blokje x86-64 dat eindigt op HLT. Zo kun je de
// interpreter valideren zonder ook maar één .exe nodig te hebben.
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "macemu/cpu.h"
#include "macemu/memory.h"
#include "test_util.h"

using namespace macemu;

namespace {

constexpr uint64_t kCodeBase = 0x1000;
constexpr uint64_t kDataBase = 0x10000;
constexpr uint64_t kStackTop = 0x30000;

struct Machine {
    Memory mem;
    Cpu cpu;

    Machine() : cpu(mem) {
        mem.map(kCodeBase, 0x2000, PROT_RWX, "code");
        mem.map(kDataBase, 0x10000, PROT_RW, "data");
        mem.map(kStackTop - 0x10000, 0x10000, PROT_RW, "stack");
        cpu.gpr[RSP] = kStackTop - 0x100;
        cpu.rip = kCodeBase;
    }

    void load(std::initializer_list<uint8_t> bytes, uint64_t at = kCodeBase) {
        std::vector<uint8_t> v(bytes);
        mem.writeBlock(at, v.data(), v.size());
    }

    void run(uint64_t maxInstr = 10000) {
        cpu.rip = kCodeBase;
        cpu.halted = false;
        cpu.run(maxInstr);
    }
};

void testMovAndRegisterSizes() {
    TEST("mov + registergroottes");
    Machine m;
    // mov rax, 0x1122334455667788 ; hlt
    m.load({0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xF4});
    m.run();
    CHECK_EQ(m.cpu.gpr[RAX], 0x1122334455667788ull);

    // 32-bit schrijven moet de bovenste 32 bits wissen (klassieke valkuil).
    Machine m2;
    m2.load({0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // mov rax, -1
             0xB8, 0x01, 0x00, 0x00, 0x00,                               // mov eax, 1
             0xF4});
    m2.run();
    CHECK_EQ(m2.cpu.gpr[RAX], 1ull);

    // 16-bit en 8-bit schrijven laten de rest juist staan.
    Machine m3;
    m3.load({0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // mov rax, -1
             0x66, 0xB8, 0x34, 0x12,                                     // mov ax, 0x1234
             0xF4});
    m3.run();
    CHECK_EQ(m3.cpu.gpr[RAX], 0xFFFFFFFFFFFF1234ull);

    // ah/al zonder REX
    Machine m4;
    m4.load({0x31, 0xC0,       // xor eax, eax
             0xB4, 0x12,       // mov ah, 0x12
             0xB0, 0x34,       // mov al, 0x34
             0xF4});
    m4.run();
    CHECK_EQ(m4.cpu.gpr[RAX], 0x1234ull);
}

void testArithmeticFlags() {
    TEST("rekenen + flags");
    Machine m;
    m.load({0xB8, 0x05, 0x00, 0x00, 0x00, // mov eax, 5
            0xB9, 0x07, 0x00, 0x00, 0x00, // mov ecx, 7
            0x01, 0xC8,                   // add eax, ecx
            0xF4});
    m.run();
    CHECK_EQ(m.cpu.gpr[RAX], 12ull);
    CHECK(!m.cpu.flags.zf);
    CHECK(!m.cpu.flags.cf);

    // signed overflow: 0x7FFFFFFF + 1
    Machine m2;
    m2.load({0xB8, 0xFF, 0xFF, 0xFF, 0x7F, // mov eax, 0x7FFFFFFF
             0x83, 0xC0, 0x01,             // add eax, 1
             0xF4});
    m2.run();
    CHECK_EQ(m2.cpu.gpr[RAX], 0x80000000ull);
    CHECK(m2.cpu.flags.of);
    CHECK(m2.cpu.flags.sf);
    CHECK(!m2.cpu.flags.cf);

    // unsigned carry: 0xFFFFFFFF + 1
    Machine m3;
    m3.load({0xB8, 0xFF, 0xFF, 0xFF, 0xFF, // mov eax, -1
             0x83, 0xC0, 0x01,             // add eax, 1
             0xF4});
    m3.run();
    CHECK_EQ(m3.cpu.gpr[RAX], 0ull);
    CHECK(m3.cpu.flags.cf);
    CHECK(m3.cpu.flags.zf);
    CHECK(!m3.cpu.flags.of);

    // sub + zf
    Machine m4;
    m4.load({0xB8, 0x0A, 0x00, 0x00, 0x00, // mov eax, 10
             0x83, 0xE8, 0x0A,             // sub eax, 10
             0xF4});
    m4.run();
    CHECK(m4.cpu.flags.zf);

    // 64-bit optellen met carry over de 32-bit grens
    Machine m5;
    m5.load({0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, // mov rax,0xFFFFFFFF
             0x48, 0x83, 0xC0, 0x01,                                     // add rax, 1
             0xF4});
    m5.run();
    CHECK_EQ(m5.cpu.gpr[RAX], 0x100000000ull);
    CHECK(!m5.cpu.flags.cf);
}

void testLogicShiftsAndBits() {
    TEST("logica, shifts en bits");
    Machine m;
    m.load({0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
            0xC1, 0xE0, 0x04,             // shl eax, 4
            0xF4});
    m.run();
    CHECK_EQ(m.cpu.gpr[RAX], 0x10ull);

    Machine m2;
    m2.load({0xB8, 0x00, 0x00, 0x00, 0x80, // mov eax, 0x80000000
             0xC1, 0xF8, 0x04,             // sar eax, 4
             0xF4});
    m2.run();
    CHECK_EQ(m2.cpu.gpr[RAX], 0xF8000000ull);

    Machine m3;
    m3.load({0xB8, 0x00, 0x00, 0x00, 0x80, // mov eax, 0x80000000
             0xC1, 0xE8, 0x04,             // shr eax, 4
             0xF4});
    m3.run();
    CHECK_EQ(m3.cpu.gpr[RAX], 0x08000000ull);

    // bts eax, 5
    Machine m4;
    m4.load({0x31, 0xC0,                   // xor eax, eax
             0x0F, 0xBA, 0xE8, 0x05,       // bts eax, 5
             0xF4});
    m4.run();
    CHECK_EQ(m4.cpu.gpr[RAX], 0x20ull);
    CHECK(!m4.cpu.flags.cf);

    // bsf/bsr
    Machine m5;
    m5.load({0xB8, 0x00, 0x01, 0x00, 0x00, // mov eax, 0x100
             0x0F, 0xBC, 0xC8,             // bsf ecx, eax
             0x0F, 0xBD, 0xD0,             // bsr edx, eax
             0xF4});
    m5.run();
    CHECK_EQ(m5.cpu.gpr[RCX], 8ull);
    CHECK_EQ(m5.cpu.gpr[RDX], 8ull);

    // bswap
    Machine m6;
    m6.load({0xB8, 0x78, 0x56, 0x34, 0x12, // mov eax, 0x12345678
             0x0F, 0xC8,                   // bswap eax
             0xF4});
    m6.run();
    CHECK_EQ(m6.cpu.gpr[RAX], 0x78563412ull);
}

void testMulDiv() {
    TEST("imul / mul / div / idiv");
    Machine m;
    m.load({0xB8, 0x07, 0x00, 0x00, 0x00, // mov eax, 7
            0xB9, 0x06, 0x00, 0x00, 0x00, // mov ecx, 6
            0x0F, 0xAF, 0xC1,             // imul eax, ecx
            0xF4});
    m.run();
    CHECK_EQ(m.cpu.gpr[RAX], 42ull);

    Machine m2;
    m2.load({0xB8, 0x64, 0x00, 0x00, 0x00, // mov eax, 100
             0x31, 0xD2,                   // xor edx, edx
             0xB9, 0x07, 0x00, 0x00, 0x00, // mov ecx, 7
             0xF7, 0xF1,                   // div ecx
             0xF4});
    m2.run();
    CHECK_EQ(m2.cpu.gpr[RAX], 14ull); // quotient
    CHECK_EQ(m2.cpu.gpr[RDX], 2ull);  // rest

    // idiv met negatief getal: -100 / 7 = -14 rest -2
    Machine m3;
    m3.load({0xB8, 0x9C, 0xFF, 0xFF, 0xFF, // mov eax, -100
             0x99,                         // cdq
             0xB9, 0x07, 0x00, 0x00, 0x00, // mov ecx, 7
             0xF7, 0xF9,                   // idiv ecx
             0xF4});
    m3.run();
    CHECK_EQ(static_cast<int32_t>(m3.cpu.gpr[RAX]), -14);
    CHECK_EQ(static_cast<int32_t>(m3.cpu.gpr[RDX]), -2);

    // 64-bit mul
    Machine m4;
    m4.load({0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // mov rax,0x100000000
             0x48, 0xB9, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rcx,2
             0x48, 0xF7, 0xE1,                                           // mul rcx
             0xF4});
    m4.run();
    CHECK_EQ(m4.cpu.gpr[RAX], 0x200000000ull);
    CHECK_EQ(m4.cpu.gpr[RDX], 0ull);
}

void testControlFlow() {
    TEST("sprongen, call/ret, stack");
    // call -> functie zet eax=99 en returnt
    Machine m;
    m.load({
        0xE8, 0x06, 0x00, 0x00, 0x00, // call +6  (naar 0x100B)
        0x83, 0xC0, 0x01,             // add eax, 1
        0xF4,                         // hlt
        0x90, 0x90,                   // nop nop (opvulling)
        0xB8, 0x63, 0x00, 0x00, 0x00, // mov eax, 99
        0xC3,                         // ret
    });
    m.run();
    CHECK_EQ(m.cpu.gpr[RAX], 100ull);

    // conditionele sprong
    Machine m2;
    m2.load({0xB8, 0x05, 0x00, 0x00, 0x00, // mov eax, 5
             0x83, 0xF8, 0x05,             // cmp eax, 5
             0x74, 0x05,                   // je +5
             0xB8, 0x00, 0x00, 0x00, 0x00, // mov eax, 0   (overgeslagen)
             0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
             0xF4});
    m2.run();
    CHECK_EQ(m2.cpu.gpr[RAX], 1ull);

    // push/pop
    Machine m3;
    m3.load({0x48, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00, // mov rax,0xDEADBEEF
             0x50,                                                       // push rax
             0x5B,                                                       // pop rbx
             0xF4});
    uint64_t rspBefore = m3.cpu.gpr[RSP];
    m3.run();
    CHECK_EQ(m3.cpu.gpr[RBX], 0xDEADBEEFull);
    CHECK_EQ(m3.cpu.gpr[RSP], rspBefore);

    // setcc + cmovcc
    Machine m4;
    m4.load({0x31, 0xC0,                   // xor eax, eax
             0x83, 0xF8, 0x00,             // cmp eax, 0
             0x0F, 0x94, 0xC3,             // sete bl
             0xB9, 0x2A, 0x00, 0x00, 0x00, // mov ecx, 42
             0x0F, 0x44, 0xC1,             // cmove eax, ecx
             0xF4});
    m4.run();
    CHECK_EQ(m4.cpu.gpr[RBX] & 0xFF, 1ull);
    CHECK_EQ(m4.cpu.gpr[RAX], 42ull);

    // loop
    Machine m5;
    m5.load({0xB9, 0x05, 0x00, 0x00, 0x00, // mov ecx, 5
             0x31, 0xC0,                   // xor eax, eax
             0x83, 0xC0, 0x02,             // add eax, 2     <- lus
             0xE2, 0xFB,                   // loop -5
             0xF4});
    m5.run();
    CHECK_EQ(m5.cpu.gpr[RAX], 10ull);
}

void testMemoryOperands() {
    TEST("geheugenoperanden, lea, SIB");
    Machine m;
    m.load({0x48, 0xB8, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, 0x10000
            0x48, 0xB9, 0x21, 0x43, 0x65, 0x87, 0x00, 0x00, 0x00, 0x00, // mov rcx, 0x87654321
            0x48, 0x89, 0x08,                                           // mov [rax], rcx
            0x48, 0x8B, 0x10,                                           // mov rdx, [rax]
            0xF4});
    m.run();
    CHECK_EQ(m.cpu.gpr[RDX], 0x87654321ull);
    CHECK_EQ(m.mem.read64(kDataBase), 0x87654321ull);

    // SIB: mov eax, [rbx + rcx*4 + 8]
    Machine m2;
    m2.mem.write32(kDataBase + 8 + 2 * 4, 0xCAFEBABE);
    m2.load({0x48, 0xBB, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rbx, 0x10000
             0x48, 0xB9, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rcx, 2
             0x8B, 0x44, 0x8B, 0x08,                                     // mov eax,[rbx+rcx*4+8]
             0xF4});
    m2.run();
    CHECK_EQ(m2.cpu.gpr[RAX], 0xCAFEBABEull);

    // RIP-relatieve lea
    Machine m3;
    // lea rax, [rip+8] ; hlt  -> instructie is 7 bytes, dus 0x1007 + 8 = 0x100F
    m3.load({0x48, 0x8D, 0x05, 0x08, 0x00, 0x00, 0x00, 0xF4});
    m3.run();
    CHECK_EQ(m3.cpu.gpr[RAX], kCodeBase + 0x0Full);

    // movzx / movsx
    Machine m4;
    m4.load({0xB9, 0xFF, 0x00, 0x00, 0x00, // mov ecx, 0xFF
             0x0F, 0xB6, 0xC1,             // movzx eax, cl
             0x0F, 0xBE, 0xD1,             // movsx edx, cl
             0xF4});
    m4.run();
    CHECK_EQ(m4.cpu.gpr[RAX], 0xFFull);
    CHECK_EQ(m4.cpu.gpr[RDX], 0xFFFFFFFFull);
}

void testStringOps() {
    TEST("string-instructies met REP");
    Machine m;
    // rep stosb: vul 16 bytes met 0xAB vanaf 0x10000
    m.load({0x48, 0xBF, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rdi, 0x10000
            0xB8, 0xAB, 0x00, 0x00, 0x00,                               // mov eax, 0xAB
            0x48, 0xB9, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rcx, 16
            0xFC,                                                       // cld
            0xF3, 0xAA,                                                 // rep stosb
            0xF4});
    m.run();
    for (int i = 0; i < 16; ++i) CHECK_EQ(m.mem.read8(kDataBase + i), 0xABull);
    CHECK_EQ(m.cpu.gpr[RCX], 0ull);
    CHECK_EQ(m.cpu.gpr[RDI], kDataBase + 16);

    // rep movsb
    Machine m2;
    for (int i = 0; i < 8; ++i) m2.mem.write8(kDataBase + i, static_cast<uint8_t>(i + 1));
    m2.load({0x48, 0xBE, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rsi, 0x10000
             0x48, 0xBF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rdi, 0x10100
             0x48, 0xB9, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rcx, 8
             0xFC, 0xF3, 0xA4,                                           // cld; rep movsb
             0xF4});
    m2.run();
    for (int i = 0; i < 8; ++i)
        CHECK_EQ(m2.mem.read8(kDataBase + 0x100 + i), static_cast<uint64_t>(i + 1));

    // repne scasb: zoek de 0 in "hello\0"
    Machine m3;
    const char* s = "hello";
    for (int i = 0; i < 6; ++i) m3.mem.write8(kDataBase + i, static_cast<uint8_t>(s[i]));
    m3.load({0x48, 0xBF, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rdi, 0x10000
             0x31, 0xC0,                                                 // xor eax, eax
             0x48, 0xB9, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rcx, 100
             0xFC, 0xF2, 0xAE,                                           // cld; repne scasb
             0xF4});
    m3.run();
    // rdi wijst één byte voorbij de gevonden NUL.
    CHECK_EQ(m3.cpu.gpr[RDI], kDataBase + 6);
}

void testSse() {
    TEST("SSE2-subset");
    Machine m;
    // pxor xmm0, xmm0 -> alles nul
    m.load({0x66, 0x0F, 0xEF, 0xC0, 0xF4});
    m.cpu.xmm[0].lo = 0x1111;
    m.cpu.xmm[0].hi = 0x2222;
    m.run();
    CHECK_EQ(m.cpu.xmm[0].lo, 0ull);
    CHECK_EQ(m.cpu.xmm[0].hi, 0ull);

    // movdqu xmm0, [rax] ; pcmpeqb xmm0, xmm1 ; pmovmskb ecx, xmm0
    // Dit is precies het patroon dat de MSVC-CRT voor strlen gebruikt.
    Machine m2;
    for (int i = 0; i < 16; ++i) m2.mem.write8(kDataBase + i, i == 5 ? 0 : 'x');
    m2.load({0x48, 0xB8, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, 0x10000
             0xF3, 0x0F, 0x6F, 0x00,                                     // movdqu xmm0,[rax]
             0x66, 0x0F, 0xEF, 0xC9,                                     // pxor xmm1, xmm1
             0x66, 0x0F, 0x74, 0xC1,                                     // pcmpeqb xmm0, xmm1
             0x66, 0x0F, 0xD7, 0xC8,                                     // pmovmskb ecx, xmm0
             0xF4});
    m2.run();
    CHECK_EQ(m2.cpu.gpr[RCX], 1ull << 5);

    // movsd + addsd (scalar double)
    Machine m3;
    double a = 1.5, b = 2.25;
    uint64_t ab, bb;
    std::memcpy(&ab, &a, 8);
    std::memcpy(&bb, &b, 8);
    m3.mem.write64(kDataBase, ab);
    m3.mem.write64(kDataBase + 8, bb);
    m3.load({0x48, 0xB8, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, 0x10000
             0xF2, 0x0F, 0x10, 0x00,                                     // movsd xmm0,[rax]
             0xF2, 0x0F, 0x10, 0x48, 0x08,                               // movsd xmm1,[rax+8]
             0xF2, 0x0F, 0x58, 0xC1,                                     // addsd xmm0, xmm1
             0xF4});
    m3.run();
    double result;
    std::memcpy(&result, &m3.cpu.xmm[0].lo, 8);
    CHECK(result == 3.75);

    // ucomisd zet ZF bij gelijkheid
    Machine m4;
    m4.mem.write64(kDataBase, ab);
    m4.mem.write64(kDataBase + 8, ab);
    m4.load({0x48, 0xB8, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
             0xF2, 0x0F, 0x10, 0x00,       // movsd xmm0,[rax]
             0xF2, 0x0F, 0x10, 0x48, 0x08, // movsd xmm1,[rax+8]
             0x66, 0x0F, 0x2E, 0xC1,       // ucomisd xmm0, xmm1
             0xF4});
    m4.run();
    CHECK(m4.cpu.flags.zf);
    CHECK(!m4.cpu.flags.cf);
}

void testCallGuestAndHle() {
    TEST("callGuest (host roept gastcode aan)");
    Machine m;
    // De HLE-regio moet bestaan voor de return-sentinel van callGuest.
    m.mem.map(0x40000, 0x1000, PROT_RX, "hle");
    m.cpu.setHleRegion(0x40000, 0x1000, nullptr);

    // functie: lea eax, [rcx + rdx] ; ret
    m.load({0x8D, 0x04, 0x11, 0xC3}, kCodeBase + 0x100);

    uint64_t before = m.cpu.gpr[RCX];
    uint64_t r = m.cpu.callGuest(kCodeBase + 0x100, {20, 22});
    CHECK_EQ(r, 42ull);
    // De registers van de aanroeper moeten ongemoeid zijn gebleven.
    CHECK_EQ(m.cpu.gpr[RCX], before);
}

void testUnsupportedInstructionIsClear() {
    TEST("onbekende instructie geeft een bruikbare fout");
    Machine m;
    m.load({0x0F, 0x0B}); // ud2
    bool threw = false;
    try {
        m.run();
    } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("UD2") != std::string::npos);
    }
    CHECK(threw);
}

} // namespace

int main() {
    testMovAndRegisterSizes();
    testArithmeticFlags();
    testLogicShiftsAndBits();
    testMulDiv();
    testControlFlow();
    testMemoryOperands();
    testStringOps();
    testSse();
    testCallGuestAndHle();
    testUnsupportedInstructionIsClear();
    return testing::summary("test_cpu");
}
