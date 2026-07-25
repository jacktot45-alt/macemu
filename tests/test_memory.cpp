// Test van het geheugenmodel: pagina's, rechten, grensgevallen.
#include <string>

#include "macemu/memory.h"
#include "test_util.h"

using namespace macemu;

int main() {
    TEST("basis lezen/schrijven");
    {
        Memory m;
        m.map(0x1000, 0x1000, PROT_RW, "test");
        m.write64(0x1000, 0x0123456789ABCDEFull);
        CHECK_EQ(m.read64(0x1000), 0x0123456789ABCDEFull);
        CHECK_EQ(m.read32(0x1000), 0x89ABCDEFull);
        CHECK_EQ(m.read16(0x1000), 0xCDEFull);
        CHECK_EQ(m.read8(0x1000), 0xEFull);
        CHECK_EQ(m.read8(0x1007), 0x01ull);
    }

    TEST("toegang over een paginagrens");
    {
        Memory m;
        m.map(0x1000, 0x2000, PROT_RW, "test");
        // Een 64-bit waarde die precies over de grens 0x2000 heen loopt.
        m.write64(0x1FFC, 0xAABBCCDDEEFF0011ull);
        CHECK_EQ(m.read64(0x1FFC), 0xAABBCCDDEEFF0011ull);
        // little-endian: 11 00 FF EE | DD CC BB AA, dus de eerste byte van de
        // volgende pagina is 0xDD.
        CHECK_EQ(m.read8(0x2000), 0xDDull);
    }

    TEST("rechten worden afgedwongen");
    {
        Memory m;
        m.map(0x1000, 0x1000, PROT_R, "readonly");
        bool threw = false;
        try {
            m.write8(0x1000, 1);
        } catch (const MemoryFault&) {
            threw = true;
        }
        CHECK(threw);

        threw = false;
        try {
            uint8_t buf[4];
            m.fetch(0x1000, buf, 4); // geen X-recht
        } catch (const MemoryFault&) {
            threw = true;
        }
        CHECK(threw);
    }

    TEST("niet-gemapte pagina geeft een fout");
    {
        Memory m;
        bool threw = false;
        try {
            m.read8(0x50000);
        } catch (const MemoryFault& f) {
            threw = true;
            CHECK_EQ(f.address, 0x50000ull);
        }
        CHECK(threw);
    }

    TEST("blokken en strings");
    {
        Memory m;
        m.map(0x1000, 0x3000, PROT_RW, "test");
        std::string s = "hallo wereld";
        m.writeCString(0x1010, s);
        CHECK(m.readCString(0x1010) == s);
        m.writeWideString(0x1100, s);
        CHECK(m.readWideString(0x1100) == s);

        m.fill(0x2000, 0x5A, 0x1500); // over meerdere pagina's heen
        CHECK_EQ(m.read8(0x2000), 0x5Aull);
        CHECK_EQ(m.read8(0x34FF), 0x5Aull);
    }

    TEST("findFreeRegion vermijdt bestaande regio's");
    {
        Memory m;
        m.map(0x10000, 0x10000, PROT_RW, "a");
        m.map(0x30000, 0x10000, PROT_RW, "b");
        uint64_t free = m.findFreeRegion(0x8000, 0x10000, 0x10000);
        CHECK(free == 0x20000);
        CHECK(!m.isMapped(free));
    }

    TEST("unmap");
    {
        Memory m;
        m.map(0x1000, 0x1000, PROT_RW, "x");
        CHECK(m.isMapped(0x1000));
        m.unmap(0x1000, 0x1000);
        CHECK(!m.isMapped(0x1000));
    }

    return testing::summary("test_memory");
}
