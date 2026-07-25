// macemu - memory.h
//
// Het geheugenmodel van de gast. Bewust simpel: een sparse pagetabel van
// 4 KiB-pagina's in een hash map. Geen echte MMU, geen TLB, wel per-pagina
// rechten (R/W/X) zodat foute toegangen meteen een duidelijke fout geven in
// plaats van stilletjes door te lopen.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "macemu/util.h"

namespace macemu {

enum Prot : uint32_t {
    PROT_NONE_ = 0,
    PROT_R = 1,
    PROT_W = 2,
    PROT_X = 4,
    PROT_RW = PROT_R | PROT_W,
    PROT_RX = PROT_R | PROT_X,
    PROT_RWX = PROT_R | PROT_W | PROT_X,
};

// Wordt gegooid bij een ongeldige geheugentoegang van de gast.
class MemoryFault : public EmuError {
public:
    MemoryFault(uint64_t addr, size_t size, const char* kind)
        : EmuError(strFormat("geheugenfout: %s op 0x%llx (%zu bytes)", kind,
                             static_cast<unsigned long long>(addr), size)),
          address(addr), accessSize(size), kind(kind) {}
    uint64_t address;
    size_t accessSize;
    std::string kind;
};

class Memory {
public:
    static constexpr uint64_t kPageSize = 0x1000;
    static constexpr uint64_t kPageMask = kPageSize - 1;

    struct Region {
        uint64_t base;
        uint64_t size;
        uint32_t prot;
        std::string name;
        bool reserved; // true = alleen gereserveerd (MEM_RESERVE), niet gecommit
    };

    Memory();
    ~Memory();

    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    // Reserveert/commit een gebied. Adres wordt naar beneden afgerond op een
    // paginagrens, de grootte naar boven.
    void map(uint64_t addr, uint64_t size, uint32_t prot, const std::string& name,
             bool reserveOnly = false);
    void unmap(uint64_t addr, uint64_t size);
    void protect(uint64_t addr, uint64_t size, uint32_t prot);
    bool isMapped(uint64_t addr) const;

    // Zoekt een vrij gat van `size` bytes met de gevraagde alignment.
    uint64_t findFreeRegion(uint64_t size, uint64_t alignment = 0x10000,
                            uint64_t hintMin = 0x10000) const;

    // Lees/schrijf primitieven. Gooien MemoryFault bij problemen.
    uint8_t read8(uint64_t addr) const;
    uint16_t read16(uint64_t addr) const;
    uint32_t read32(uint64_t addr) const;
    uint64_t read64(uint64_t addr) const;
    void write8(uint64_t addr, uint8_t v);
    void write16(uint64_t addr, uint16_t v);
    void write32(uint64_t addr, uint32_t v);
    void write64(uint64_t addr, uint64_t v);

    void readBlock(uint64_t addr, void* dst, size_t len) const;
    void writeBlock(uint64_t addr, const void* src, size_t len);
    void fill(uint64_t addr, uint8_t value, size_t len);

    // Instructie-fetch: controleert het X-recht.
    void fetch(uint64_t addr, uint8_t* dst, size_t len) const;

    // Strings uit gastgeheugen (met een veiligheidslimiet).
    std::string readCString(uint64_t addr, size_t maxLen = 65536) const;
    std::string readWideString(uint64_t addr, size_t maxChars = 65536) const;
    void writeCString(uint64_t addr, const std::string& s); // incl. NUL
    void writeWideString(uint64_t addr, const std::string& s); // incl. NUL

    const std::map<uint64_t, Region>& regions() const { return regions_; }
    void dumpMap(std::string& out) const;

    uint64_t totalCommitted() const { return pages_.size() * kPageSize; }

private:
    struct Page {
        uint8_t* data = nullptr;
        uint32_t prot = 0;
    };

    Page* findPage(uint64_t addr) const;
    Page* pageForAccess(uint64_t addr, uint32_t need, size_t accessSize) const;

    // mutable: de cache is puur een versnelling, ook geldig in const-methodes.
    mutable std::unordered_map<uint64_t, Page> pages_;
    std::map<uint64_t, Region> regions_;
    mutable uint64_t lastPageKey_ = ~0ull;
    mutable Page* lastPage_ = nullptr;
};

} // namespace macemu
