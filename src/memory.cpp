#include "macemu/memory.h"

#include <cstring>

namespace macemu {

Memory::Memory() = default;

Memory::~Memory() {
    for (auto& kv : pages_) delete[] kv.second.data;
    pages_.clear();
}

void Memory::map(uint64_t addr, uint64_t size, uint32_t prot, const std::string& name,
                 bool reserveOnly) {
    if (size == 0) return;
    uint64_t start = addr & ~kPageMask;
    uint64_t end = (addr + size + kPageMask) & ~kPageMask;

    regions_[start] = Region{start, end - start, prot, name, reserveOnly};
    lastPageKey_ = ~0ull;
    lastPage_ = nullptr;

    if (reserveOnly) return;

    for (uint64_t p = start; p < end; p += kPageSize) {
        auto it = pages_.find(p);
        if (it == pages_.end()) {
            Page pg;
            pg.data = new uint8_t[kPageSize];
            std::memset(pg.data, 0, kPageSize);
            pg.prot = prot;
            pages_.emplace(p, pg);
        } else {
            it->second.prot = prot;
        }
    }
}

void Memory::unmap(uint64_t addr, uint64_t size) {
    uint64_t start = addr & ~kPageMask;
    uint64_t end = (addr + size + kPageMask) & ~kPageMask;
    for (uint64_t p = start; p < end; p += kPageSize) {
        auto it = pages_.find(p);
        if (it != pages_.end()) {
            delete[] it->second.data;
            pages_.erase(it);
        }
    }
    auto it = regions_.find(start);
    if (it != regions_.end() && it->second.size <= end - start) regions_.erase(it);
    lastPageKey_ = ~0ull;
    lastPage_ = nullptr;
}

void Memory::protect(uint64_t addr, uint64_t size, uint32_t prot) {
    uint64_t start = addr & ~kPageMask;
    uint64_t end = (addr + size + kPageMask) & ~kPageMask;
    for (uint64_t p = start; p < end; p += kPageSize) {
        auto it = pages_.find(p);
        if (it != pages_.end()) it->second.prot = prot;
    }
    // Regio's die volledig binnen het bereik vallen krijgen ook de nieuwe
    // rechten, zodat dumpMap() de waarheid laat zien. Regio's die maar deels
    // overlappen laten we staan; die worden per pagina wel correct afgedwongen.
    for (auto& kv : regions_) {
        Region& r = kv.second;
        if (r.base >= start && r.base + r.size <= end) r.prot = prot;
    }
}

bool Memory::isMapped(uint64_t addr) const {
    return pages_.find(addr & ~kPageMask) != pages_.end();
}

uint64_t Memory::findFreeRegion(uint64_t size, uint64_t alignment, uint64_t hintMin) const {
    if (alignment == 0) alignment = kPageSize;
    uint64_t candidate = (hintMin + alignment - 1) & ~(alignment - 1);
    uint64_t need = (size + kPageMask) & ~kPageMask;

    for (const auto& kv : regions_) {
        const Region& r = kv.second;
        if (r.base + r.size <= candidate) continue;
        if (candidate + need <= r.base) return candidate;
        candidate = (r.base + r.size + alignment - 1) & ~(alignment - 1);
    }
    return candidate;
}

Memory::Page* Memory::findPage(uint64_t addr) const {
    uint64_t key = addr & ~kPageMask;
    if (key == lastPageKey_ && lastPage_) return lastPage_;
    auto it = pages_.find(key);
    if (it == pages_.end()) return nullptr;
    lastPageKey_ = key;
    lastPage_ = &it->second;
    return lastPage_;
}

Memory::Page* Memory::pageForAccess(uint64_t addr, uint32_t need, size_t accessSize) const {
    Page* pg = findPage(addr);
    if (!pg) throw MemoryFault(addr, accessSize, "niet-gemapte pagina");
    if ((pg->prot & need) != need) {
        const char* what = (need & PROT_X) ? "geen uitvoerrecht"
                           : (need & PROT_W) ? "geen schrijfrecht"
                                             : "geen leesrecht";
        throw MemoryFault(addr, accessSize, what);
    }
    return pg;
}

// Toegangen die een paginagrens kruisen worden byte-voor-byte afgehandeld.
// Dat is traag maar zeldzaam; de snelle route is de aligned/binnen-pagina-case.
#define MACEMU_RW_IMPL(TYPE, NAME)                                                       \
    TYPE Memory::read##NAME(uint64_t addr) const {                                       \
        constexpr size_t sz = sizeof(TYPE);                                              \
        uint64_t off = addr & kPageMask;                                                 \
        if (off + sz <= kPageSize) {                                                     \
            Page* pg = pageForAccess(addr, PROT_R, sz);                                  \
            TYPE v;                                                                      \
            std::memcpy(&v, pg->data + off, sz);                                         \
            return v;                                                                    \
        }                                                                                \
        TYPE v = 0;                                                                      \
        for (size_t i = 0; i < sz; ++i)                                                  \
            v |= static_cast<TYPE>(static_cast<TYPE>(read8(addr + i)) << (8 * i));       \
        return v;                                                                        \
    }                                                                                    \
    void Memory::write##NAME(uint64_t addr, TYPE v) {                                    \
        constexpr size_t sz = sizeof(TYPE);                                              \
        uint64_t off = addr & kPageMask;                                                 \
        if (off + sz <= kPageSize) {                                                     \
            Page* pg = pageForAccess(addr, PROT_W, sz);                                  \
            std::memcpy(pg->data + off, &v, sz);                                         \
            return;                                                                      \
        }                                                                                \
        for (size_t i = 0; i < sz; ++i)                                                  \
            write8(addr + i, static_cast<uint8_t>(v >> (8 * i)));                        \
    }

uint8_t Memory::read8(uint64_t addr) const {
    Page* pg = pageForAccess(addr, PROT_R, 1);
    return pg->data[addr & kPageMask];
}

void Memory::write8(uint64_t addr, uint8_t v) {
    Page* pg = pageForAccess(addr, PROT_W, 1);
    pg->data[addr & kPageMask] = v;
}

MACEMU_RW_IMPL(uint16_t, 16)
MACEMU_RW_IMPL(uint32_t, 32)
MACEMU_RW_IMPL(uint64_t, 64)

#undef MACEMU_RW_IMPL

void Memory::readBlock(uint64_t addr, void* dst, size_t len) const {
    uint8_t* out = static_cast<uint8_t*>(dst);
    while (len > 0) {
        uint64_t off = addr & kPageMask;
        size_t chunk = static_cast<size_t>(kPageSize - off);
        if (chunk > len) chunk = len;
        Page* pg = pageForAccess(addr, PROT_R, chunk);
        std::memcpy(out, pg->data + off, chunk);
        out += chunk;
        addr += chunk;
        len -= chunk;
    }
}

void Memory::writeBlock(uint64_t addr, const void* src, size_t len) {
    const uint8_t* in = static_cast<const uint8_t*>(src);
    while (len > 0) {
        uint64_t off = addr & kPageMask;
        size_t chunk = static_cast<size_t>(kPageSize - off);
        if (chunk > len) chunk = len;
        Page* pg = pageForAccess(addr, PROT_W, chunk);
        std::memcpy(pg->data + off, in, chunk);
        in += chunk;
        addr += chunk;
        len -= chunk;
    }
}

void Memory::fill(uint64_t addr, uint8_t value, size_t len) {
    while (len > 0) {
        uint64_t off = addr & kPageMask;
        size_t chunk = static_cast<size_t>(kPageSize - off);
        if (chunk > len) chunk = len;
        Page* pg = pageForAccess(addr, PROT_W, chunk);
        std::memset(pg->data + off, value, chunk);
        addr += chunk;
        len -= chunk;
    }
}

void Memory::fetch(uint64_t addr, uint8_t* dst, size_t len) const {
    // Bij een fetch aan het einde van een pagina mag de volgende pagina
    // ontbreken; we leveren dan minder bytes. De decoder merkt dat vanzelf.
    size_t done = 0;
    while (done < len) {
        uint64_t a = addr + done;
        Page* pg = findPage(a);
        if (!pg) {
            if (done == 0) throw MemoryFault(a, len, "instructie-fetch uit niet-gemapt geheugen");
            std::memset(dst + done, 0, len - done);
            return;
        }
        if ((pg->prot & PROT_X) == 0)
            throw MemoryFault(a, len, "instructie-fetch zonder uitvoerrecht");
        uint64_t off = a & kPageMask;
        size_t chunk = static_cast<size_t>(kPageSize - off);
        if (chunk > len - done) chunk = len - done;
        std::memcpy(dst + done, pg->data + off, chunk);
        done += chunk;
    }
}

std::string Memory::readCString(uint64_t addr, size_t maxLen) const {
    std::string s;
    for (size_t i = 0; i < maxLen; ++i) {
        uint8_t c = read8(addr + i);
        if (c == 0) break;
        s += static_cast<char>(c);
    }
    return s;
}

std::string Memory::readWideString(uint64_t addr, size_t maxChars) const {
    std::vector<uint16_t> w;
    for (size_t i = 0; i < maxChars; ++i) {
        uint16_t c = read16(addr + i * 2);
        if (c == 0) break;
        w.push_back(c);
    }
    return utf16ToUtf8(w);
}

void Memory::writeCString(uint64_t addr, const std::string& s) {
    writeBlock(addr, s.data(), s.size());
    write8(addr + s.size(), 0);
}

void Memory::writeWideString(uint64_t addr, const std::string& s) {
    std::vector<uint16_t> w = utf8ToUtf16(s);
    for (size_t i = 0; i < w.size(); ++i) write16(addr + i * 2, w[i]);
    write16(addr + w.size() * 2, 0);
}

void Memory::dumpMap(std::string& out) const {
    out += "  adres                  grootte     rechten  naam\n";
    for (const auto& kv : regions_) {
        const Region& r = kv.second;
        char prot[4] = {
            static_cast<char>((r.prot & PROT_R) ? 'r' : '-'),
            static_cast<char>((r.prot & PROT_W) ? 'w' : '-'),
            static_cast<char>((r.prot & PROT_X) ? 'x' : '-'),
            0,
        };
        out += strFormat("  0x%012llx-0x%012llx  %8llu  %s      %s%s\n",
                         static_cast<unsigned long long>(r.base),
                         static_cast<unsigned long long>(r.base + r.size),
                         static_cast<unsigned long long>(r.size), prot, r.name.c_str(),
                         r.reserved ? " (reserved)" : "");
    }
}

} // namespace macemu
