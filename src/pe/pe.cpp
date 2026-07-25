#include "macemu/pe.h"

#include <algorithm>
#include <cstring>

namespace macemu {
namespace pe {

namespace {

template <typename T>
T rd(const std::vector<uint8_t>& d, size_t off) {
    if (off + sizeof(T) > d.size())
        throw EmuError(strFormat("PE: lezen buiten bestand op offset 0x%zx", off));
    T v;
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return s;
}

} // namespace

PeFile PeFile::parseFile(const std::string& p) {
    return parse(readWholeFile(p), p);
}

PeFile PeFile::parse(const std::vector<uint8_t>& data, const std::string& p) {
    PeFile pe;
    pe.raw = data;
    pe.path = p;

    if (data.size() < 0x40) throw EmuError("PE: bestand te klein voor een DOS-header");
    if (rd<uint16_t>(data, 0) != kDosMagic)
        throw EmuError("PE: geen 'MZ'-signatuur - dit is geen Windows-executable");

    uint32_t peOff = rd<uint32_t>(data, 0x3C);
    if (peOff + 24 > data.size()) throw EmuError("PE: e_lfanew wijst buiten het bestand");
    if (rd<uint32_t>(data, peOff) != kNtSignature)
        throw EmuError("PE: geen 'PE\\0\\0'-signatuur op e_lfanew");

    size_t fileHdr = peOff + 4;
    pe.machine = rd<uint16_t>(data, fileHdr + 0);
    pe.numberOfSections = rd<uint16_t>(data, fileHdr + 2);
    pe.timeDateStamp = rd<uint32_t>(data, fileHdr + 4);
    uint16_t sizeOfOptional = rd<uint16_t>(data, fileHdr + 16);
    pe.characteristics = rd<uint16_t>(data, fileHdr + 18);

    size_t opt = fileHdr + 20;
    if (sizeOfOptional == 0) throw EmuError("PE: object file zonder optional header (geen .exe)");
    pe.optionalMagic = rd<uint16_t>(data, opt + 0);
    pe.is64 = (pe.optionalMagic == kOptMagicPe32Plus);
    if (pe.optionalMagic != kOptMagicPe32 && pe.optionalMagic != kOptMagicPe32Plus)
        throw EmuError(strFormat("PE: onbekende optional header magic 0x%x", pe.optionalMagic));

    pe.entryPointRva = rd<uint32_t>(data, opt + 16);
    uint32_t numDirs;
    if (pe.is64) {
        pe.imageBase = rd<uint64_t>(data, opt + 24);
        pe.sectionAlignment = rd<uint32_t>(data, opt + 32);
        pe.fileAlignment = rd<uint32_t>(data, opt + 36);
        pe.majorSubsystemVersion = rd<uint16_t>(data, opt + 48);
        pe.minorSubsystemVersion = rd<uint16_t>(data, opt + 50);
        pe.sizeOfImage = rd<uint32_t>(data, opt + 56);
        pe.sizeOfHeaders = rd<uint32_t>(data, opt + 60);
        pe.subsystem = rd<uint16_t>(data, opt + 68);
        pe.dllCharacteristics = rd<uint16_t>(data, opt + 70);
        pe.sizeOfStackReserve = rd<uint64_t>(data, opt + 72);
        pe.sizeOfStackCommit = rd<uint64_t>(data, opt + 80);
        pe.sizeOfHeapReserve = rd<uint64_t>(data, opt + 88);
        pe.sizeOfHeapCommit = rd<uint64_t>(data, opt + 96);
        numDirs = rd<uint32_t>(data, opt + 108);
        for (int i = 0; i < 16 && static_cast<uint32_t>(i) < numDirs; ++i) {
            pe.dataDirs[i].rva = rd<uint32_t>(data, opt + 112 + i * 8);
            pe.dataDirs[i].size = rd<uint32_t>(data, opt + 116 + i * 8);
        }
    } else {
        pe.imageBase = rd<uint32_t>(data, opt + 28);
        pe.sectionAlignment = rd<uint32_t>(data, opt + 32);
        pe.fileAlignment = rd<uint32_t>(data, opt + 36);
        pe.majorSubsystemVersion = rd<uint16_t>(data, opt + 48);
        pe.minorSubsystemVersion = rd<uint16_t>(data, opt + 50);
        pe.sizeOfImage = rd<uint32_t>(data, opt + 56);
        pe.sizeOfHeaders = rd<uint32_t>(data, opt + 60);
        pe.subsystem = rd<uint16_t>(data, opt + 68);
        pe.dllCharacteristics = rd<uint16_t>(data, opt + 70);
        pe.sizeOfStackReserve = rd<uint32_t>(data, opt + 72);
        pe.sizeOfStackCommit = rd<uint32_t>(data, opt + 76);
        pe.sizeOfHeapReserve = rd<uint32_t>(data, opt + 80);
        pe.sizeOfHeapCommit = rd<uint32_t>(data, opt + 84);
        numDirs = rd<uint32_t>(data, opt + 92);
        for (int i = 0; i < 16 && static_cast<uint32_t>(i) < numDirs; ++i) {
            pe.dataDirs[i].rva = rd<uint32_t>(data, opt + 96 + i * 8);
            pe.dataDirs[i].size = rd<uint32_t>(data, opt + 100 + i * 8);
        }
    }

    pe.parseSections(opt + sizeOfOptional);
    pe.parseImports();
    pe.parseDelayImports();
    pe.parseExports();
    pe.parseRelocations();
    pe.parseClr();
    pe.parseTls();
    return pe;
}

void PeFile::parseSections(size_t secTableOff) {
    sections.reserve(numberOfSections);
    for (uint16_t i = 0; i < numberOfSections; ++i) {
        size_t off = secTableOff + i * 40;
        if (off + 40 > raw.size()) throw EmuError("PE: sectietabel loopt buiten het bestand");
        Section s;
        char nameBuf[9] = {0};
        std::memcpy(nameBuf, raw.data() + off, 8);
        s.name = nameBuf;
        s.virtualSize = rd<uint32_t>(raw, off + 8);
        s.virtualAddress = rd<uint32_t>(raw, off + 12);
        s.rawSize = rd<uint32_t>(raw, off + 16);
        s.rawPointer = rd<uint32_t>(raw, off + 20);
        s.characteristics = rd<uint32_t>(raw, off + 36);
        sections.push_back(s);
    }
}

const Section* PeFile::sectionForRva(uint32_t rva) const {
    for (const auto& s : sections) {
        uint32_t size = s.virtualSize ? s.virtualSize : s.rawSize;
        if (rva >= s.virtualAddress && rva < s.virtualAddress + size) return &s;
    }
    return nullptr;
}

uint64_t PeFile::rvaToOffset(uint32_t rva) const {
    if (rva < sizeOfHeaders && rva < raw.size()) return rva;
    const Section* s = sectionForRva(rva);
    if (!s) return UINT64_MAX;
    uint32_t delta = rva - s->virtualAddress;
    if (delta >= s->rawSize) return UINT64_MAX; // in .bss-achtig gebied
    return static_cast<uint64_t>(s->rawPointer) + delta;
}

bool PeFile::readAtRva(uint32_t rva, void* dst, size_t len) const {
    uint64_t off = rvaToOffset(rva);
    if (off == UINT64_MAX || off + len > raw.size()) return false;
    std::memcpy(dst, raw.data() + off, len);
    return true;
}

std::string PeFile::readCStringAtRva(uint32_t rva, size_t maxLen) const {
    uint64_t off = rvaToOffset(rva);
    if (off == UINT64_MAX) return {};
    std::string s;
    for (size_t i = 0; i < maxLen && off + i < raw.size(); ++i) {
        char c = static_cast<char>(raw[off + i]);
        if (!c) break;
        s += c;
    }
    return s;
}

void PeFile::parseImports() {
    const DataDirectory& dir = dataDirs[kDirImport];
    if (dir.rva == 0 || dir.size == 0) return;

    for (uint32_t i = 0;; ++i) {
        uint32_t descRva = dir.rva + i * 20;
        uint32_t oft = 0, name = 0, firstThunk = 0;
        uint32_t timeStamp = 0, forwarder = 0;
        if (!readAtRva(descRva + 0, &oft, 4)) break;
        if (!readAtRva(descRva + 4, &timeStamp, 4)) break;
        if (!readAtRva(descRva + 8, &forwarder, 4)) break;
        if (!readAtRva(descRva + 12, &name, 4)) break;
        if (!readAtRva(descRva + 16, &firstThunk, 4)) break;
        if (oft == 0 && name == 0 && firstThunk == 0) break;

        ImportedModule m;
        m.dllName = readCStringAtRva(name);
        if (m.dllName.empty()) m.dllName = strFormat("<onbekend #%u>", i);

        uint32_t lookupRva = oft ? oft : firstThunk;
        if (lookupRva == 0) {
            imports.push_back(m);
            continue;
        }

        size_t thunkSize = is64 ? 8 : 4;
        for (uint32_t k = 0;; ++k) {
            uint64_t entry = 0;
            if (!readAtRva(static_cast<uint32_t>(lookupRva + k * thunkSize), &entry, thunkSize))
                break;
            if (entry == 0) break;
            ImportedSymbol sym;
            sym.iatRva = firstThunk + k * thunkSize;
            uint64_t ordinalFlag = is64 ? 0x8000000000000000ull : 0x80000000ull;
            if (entry & ordinalFlag) {
                sym.byOrdinal = true;
                sym.ordinal = static_cast<uint16_t>(entry & 0xFFFF);
            } else {
                uint32_t hintNameRva = static_cast<uint32_t>(entry & 0x7FFFFFFF);
                uint16_t hint = 0;
                readAtRva(hintNameRva, &hint, 2);
                sym.hint = hint;
                sym.name = readCStringAtRva(hintNameRva + 2);
            }
            m.symbols.push_back(sym);
            if (k > 100000) break; // vangnet tegen kapotte bestanden
        }
        imports.push_back(m);
        if (i > 4096) break;
    }
}

void PeFile::parseDelayImports() {
    const DataDirectory& dir = dataDirs[kDirDelayImport];
    if (dir.rva == 0 || dir.size == 0) return;

    for (uint32_t i = 0;; ++i) {
        uint32_t off = dir.rva + i * 32;
        uint32_t attrs = 0, nameRva = 0, iat = 0, intRva = 0;
        if (!readAtRva(off + 0, &attrs, 4)) break;
        if (!readAtRva(off + 4, &nameRva, 4)) break;
        if (!readAtRva(off + 12, &iat, 4)) break;
        if (!readAtRva(off + 16, &intRva, 4)) break;
        if (nameRva == 0 && iat == 0) break;

        ImportedModule m;
        m.delayLoaded = true;
        m.dllName = readCStringAtRva(nameRva);
        if (m.dllName.empty()) break;

        size_t thunkSize = is64 ? 8 : 4;
        if (intRva) {
            for (uint32_t k = 0;; ++k) {
                uint64_t entry = 0;
                if (!readAtRva(static_cast<uint32_t>(intRva + k * thunkSize), &entry, thunkSize))
                    break;
                if (entry == 0) break;
                ImportedSymbol sym;
                sym.iatRva = iat + k * thunkSize;
                uint64_t ordinalFlag = is64 ? 0x8000000000000000ull : 0x80000000ull;
                if (entry & ordinalFlag) {
                    sym.byOrdinal = true;
                    sym.ordinal = static_cast<uint16_t>(entry & 0xFFFF);
                } else {
                    sym.name = readCStringAtRva(static_cast<uint32_t>((entry & 0x7FFFFFFF) + 2));
                }
                m.symbols.push_back(sym);
                if (k > 100000) break;
            }
        }
        imports.push_back(m);
        if (i > 4096) break;
    }
}

void PeFile::parseExports() {
    const DataDirectory& dir = dataDirs[kDirExport];
    if (dir.rva == 0 || dir.size == 0) return;

    uint32_t base = 0, numFuncs = 0, numNames = 0;
    uint32_t addrRva = 0, nameRva = 0, ordRva = 0;
    if (!readAtRva(dir.rva + 16, &base, 4)) return;
    if (!readAtRva(dir.rva + 20, &numFuncs, 4)) return;
    if (!readAtRva(dir.rva + 24, &numNames, 4)) return;
    if (!readAtRva(dir.rva + 28, &addrRva, 4)) return;
    if (!readAtRva(dir.rva + 32, &nameRva, 4)) return;
    if (!readAtRva(dir.rva + 36, &ordRva, 4)) return;
    if (numFuncs > 1000000 || numNames > 1000000) return;

    std::vector<uint32_t> funcRvas(numFuncs, 0);
    for (uint32_t i = 0; i < numFuncs; ++i) readAtRva(addrRva + i * 4, &funcRvas[i], 4);

    std::vector<std::string> names(numFuncs);
    for (uint32_t i = 0; i < numNames; ++i) {
        uint32_t nRva = 0;
        uint16_t ord = 0;
        if (!readAtRva(nameRva + i * 4, &nRva, 4)) break;
        if (!readAtRva(ordRva + i * 2, &ord, 2)) break;
        if (ord < numFuncs) names[ord] = readCStringAtRva(nRva);
    }

    for (uint32_t i = 0; i < numFuncs; ++i) {
        if (funcRvas[i] == 0) continue;
        ExportedSymbol e;
        e.ordinal = static_cast<uint16_t>(base + i);
        e.rva = funcRvas[i];
        e.name = names[i];
        // Ligt de RVA binnen de export directory zelf, dan is het een forwarder.
        if (funcRvas[i] >= dir.rva && funcRvas[i] < dir.rva + dir.size)
            e.forwarder = readCStringAtRva(funcRvas[i]);
        exports.push_back(e);
    }
}

void PeFile::parseRelocations() {
    const DataDirectory& dir = dataDirs[kDirBaseReloc];
    if (dir.rva == 0 || dir.size == 0) return;

    uint32_t pos = 0;
    while (pos + 8 <= dir.size) {
        uint32_t pageRva = 0, blockSize = 0;
        if (!readAtRva(dir.rva + pos, &pageRva, 4)) break;
        if (!readAtRva(dir.rva + pos + 4, &blockSize, 4)) break;
        if (blockSize < 8 || blockSize > dir.size - pos) break;
        uint32_t count = (blockSize - 8) / 2;
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t entry = 0;
            if (!readAtRva(dir.rva + pos + 8 + i * 2, &entry, 2)) break;
            uint16_t type = entry >> 12;
            if (type == kRelocAbsolute) continue;
            relocations.push_back(Relocation{pageRva + (entry & 0xFFF), type});
        }
        pos += blockSize;
    }
}

void PeFile::parseClr() {
    const DataDirectory& dir = dataDirs[kDirComDescriptor];
    if (dir.rva == 0 || dir.size == 0) return;
    hasClrHeader = true;
    readAtRva(dir.rva + 4, &clrMajorRuntime, 2);
    readAtRva(dir.rva + 6, &clrMinorRuntime, 2);
    readAtRva(dir.rva + 16, &clrFlags, 4);
}

void PeFile::parseTls() {
    const DataDirectory& dir = dataDirs[kDirTls];
    if (dir.rva == 0 || dir.size == 0) return;
    hasTls = true;
    if (is64)
        readAtRva(dir.rva + 24, &tlsCallbacksVa, 8);
    else {
        uint32_t v = 0;
        readAtRva(dir.rva + 12, &v, 4);
        tlsCallbacksVa = v;
    }
}

std::string PeFile::machineName() const {
    switch (machine) {
        case kMachineI386: return "x86 (32-bit, i386)";
        case kMachineAmd64: return "x86-64 (64-bit, AMD64)";
        case kMachineArm64: return "ARM64 (AArch64)";
        case kMachineArmNT: return "ARM (Thumb-2)";
        case kMachineIA64: return "Itanium (IA-64)";
        default: return strFormat("onbekend (0x%04x)", machine);
    }
}

std::string PeFile::subsystemName() const {
    switch (subsystem) {
        case kSubsystemNative: return "Native (driver/ntdll)";
        case kSubsystemWindowsGui: return "Windows GUI";
        case kSubsystemWindowsCui: return "Windows Console (CUI)";
        case 5: return "OS/2 CUI";
        case 7: return "POSIX CUI";
        case 9: return "Windows CE GUI";
        case 10: return "EFI Application";
        case 16: return "Windows Boot Application";
        default: return strFormat("onbekend (%u)", subsystem);
    }
}

std::vector<std::string> PeFile::importedDllNames() const {
    std::vector<std::string> out;
    for (const auto& m : imports) out.push_back(toLower(m.dllName));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace pe
} // namespace macemu
