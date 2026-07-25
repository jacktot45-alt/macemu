// macemu - pe.h
//
// PE/COFF-parser. Wordt gebruikt door twee dingen:
//   1. macemu-peinfo (Fase 0: haalbaarheidscheck, puur lezen/rapporteren)
//   2. de loader (Fase 2: secties mappen, imports resolven)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "macemu/util.h"

namespace macemu {
namespace pe {

// --- constanten uit winnt.h -------------------------------------------------
constexpr uint16_t kDosMagic = 0x5A4D;      // "MZ"
constexpr uint32_t kNtSignature = 0x00004550; // "PE\0\0"

constexpr uint16_t kMachineI386 = 0x014c;
constexpr uint16_t kMachineAmd64 = 0x8664;
constexpr uint16_t kMachineArm64 = 0xAA64;
constexpr uint16_t kMachineArmNT = 0x01c4;
constexpr uint16_t kMachineIA64 = 0x0200;

constexpr uint16_t kOptMagicPe32 = 0x10b;
constexpr uint16_t kOptMagicPe32Plus = 0x20b;

constexpr uint16_t kSubsystemNative = 1;
constexpr uint16_t kSubsystemWindowsGui = 2;
constexpr uint16_t kSubsystemWindowsCui = 3;

constexpr int kDirExport = 0;
constexpr int kDirImport = 1;
constexpr int kDirResource = 2;
constexpr int kDirException = 3;
constexpr int kDirSecurity = 4;
constexpr int kDirBaseReloc = 5;
constexpr int kDirDebug = 6;
constexpr int kDirTls = 9;
constexpr int kDirLoadConfig = 10;
constexpr int kDirBoundImport = 11;
constexpr int kDirIat = 12;
constexpr int kDirDelayImport = 13;
constexpr int kDirComDescriptor = 14; // .NET / CLR header

constexpr uint32_t kSecCodeFlag = 0x00000020;
constexpr uint32_t kSecInitData = 0x00000040;
constexpr uint32_t kSecUninitData = 0x00000080;
constexpr uint32_t kSecMemExecute = 0x20000000;
constexpr uint32_t kSecMemRead = 0x40000000;
constexpr uint32_t kSecMemWrite = 0x80000000;

constexpr uint16_t kRelocAbsolute = 0;
constexpr uint16_t kRelocHigh = 1;
constexpr uint16_t kRelocLow = 2;
constexpr uint16_t kRelocHighLow = 3;
constexpr uint16_t kRelocHighAdj = 4;
constexpr uint16_t kRelocDir64 = 10;

// --- geparste structuren ----------------------------------------------------
struct Section {
    std::string name;
    uint32_t virtualSize = 0;
    uint32_t virtualAddress = 0; // RVA
    uint32_t rawSize = 0;
    uint32_t rawPointer = 0;
    uint32_t characteristics = 0;
};

struct ImportedSymbol {
    std::string name;   // leeg als by-ordinal
    uint16_t ordinal = 0;
    bool byOrdinal = false;
    uint64_t iatRva = 0; // waar de resolved adres geschreven moet worden
    uint64_t hint = 0;
};

struct ImportedModule {
    std::string dllName;
    std::vector<ImportedSymbol> symbols;
    bool delayLoaded = false;
};

struct ExportedSymbol {
    std::string name;
    uint16_t ordinal = 0;
    uint32_t rva = 0;
    std::string forwarder; // niet leeg = "OTHERDLL.FuncName"
};

struct DataDirectory {
    uint32_t rva = 0;
    uint32_t size = 0;
};

struct Relocation {
    uint64_t rva;
    uint16_t type;
};

class PeFile {
public:
    // Gooit EmuError als het bestand geen geldige PE is.
    static PeFile parse(const std::vector<uint8_t>& data, const std::string& path = "");
    static PeFile parseFile(const std::string& path);

    // --- headers ---
    std::string path;
    uint16_t machine = 0;
    uint16_t numberOfSections = 0;
    uint32_t timeDateStamp = 0;
    uint16_t characteristics = 0;
    uint16_t optionalMagic = 0;
    bool is64 = false;
    uint32_t entryPointRva = 0;
    uint64_t imageBase = 0;
    uint32_t sectionAlignment = 0;
    uint32_t fileAlignment = 0;
    uint32_t sizeOfImage = 0;
    uint32_t sizeOfHeaders = 0;
    uint16_t subsystem = 0;
    uint16_t dllCharacteristics = 0;
    uint64_t sizeOfStackReserve = 0;
    uint64_t sizeOfStackCommit = 0;
    uint64_t sizeOfHeapReserve = 0;
    uint64_t sizeOfHeapCommit = 0;
    uint16_t majorSubsystemVersion = 0;
    uint16_t minorSubsystemVersion = 0;
    DataDirectory dataDirs[16];

    std::vector<Section> sections;
    std::vector<ImportedModule> imports;
    std::vector<ExportedSymbol> exports;
    std::vector<Relocation> relocations;
    std::vector<uint8_t> raw;

    // .NET/CLR
    bool hasClrHeader = false;
    uint32_t clrFlags = 0;
    uint16_t clrMajorRuntime = 0;
    uint16_t clrMinorRuntime = 0;

    // TLS
    bool hasTls = false;
    uint64_t tlsCallbacksVa = 0;

    // --- helpers ---
    bool hasRelocations() const { return dataDirs[kDirBaseReloc].size > 0; }
    bool isDll() const { return (characteristics & 0x2000) != 0; }
    bool isAslr() const { return (dllCharacteristics & 0x0040) != 0; }
    const Section* sectionForRva(uint32_t rva) const;
    // Vertaalt een RVA naar een offset in `raw`; UINT64_MAX als dat niet kan.
    uint64_t rvaToOffset(uint32_t rva) const;
    // Leest bytes op een RVA uit de bestandsinhoud (niet uit gastgeheugen).
    bool readAtRva(uint32_t rva, void* dst, size_t len) const;
    std::string readCStringAtRva(uint32_t rva, size_t maxLen = 4096) const;

    std::string machineName() const;
    std::string subsystemName() const;

    // Alle DLL-namen (imports + delay imports), lowercase.
    std::vector<std::string> importedDllNames() const;

private:
    void parseSections(size_t secTableOff);
    void parseImports();
    void parseDelayImports();
    void parseExports();
    void parseRelocations();
    void parseClr();
    void parseTls();
};

} // namespace pe
} // namespace macemu
