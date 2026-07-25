// macemu - emulator.h
//
// Bindt alles samen: geheugen, CPU, PE-loader en de Win32-laag.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "macemu/cpu.h"
#include "macemu/memory.h"
#include "macemu/pe.h"
#include "macemu/win32.h"

namespace macemu {

struct EmulatorOptions {
    std::string exePath;
    std::vector<std::string> guestArgs;
    bool trace = false;
    uint64_t traceLimit = 0;
    bool dryRun = false;       // laden + imports resolven, niet uitvoeren
    bool headless = false;     // geen SDL-venster
    bool stubUnknown = false;  // onbekende imports geven 0 terug i.p.v. crashen
    bool force = false;        // doorgaan ondanks een Fase 0-blokkade
    bool showAnalysis = true;
    uint64_t maxInstructions = 0;
    std::string screenshotPath;
    // Hoe lang een headless GUI-app mag draaien voordat we het venster sluiten.
    double headlessSeconds = 1.5;
    uint64_t stackSize = 1 * 1024 * 1024;
    uint64_t heapSize = 64 * 1024 * 1024;
};

struct LoadedImage {
    pe::PeFile pe;
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t entryPoint = 0;
    std::string name;
    std::vector<std::pair<std::string, std::string>> unresolvedImports;
};

class Emulator {
public:
    explicit Emulator(const EmulatorOptions& opts);
    ~Emulator();

    // Laadt de executable (Fase 2). Gooit EmuError als dat niet lukt.
    void load();
    // Voert uit tot het proces stopt. Geeft de exit-code van de gast terug.
    int execute();
    // load() + execute() met foutafhandeling en rapportage.
    int run();

    Memory& memory() { return mem_; }
    Cpu& cpu() { return cpu_; }
    Win32& win32() { return *win32_; }
    LoadedImage& image() { return image_; }
    const EmulatorOptions& options() const { return opts_; }

    // Layout van de gastadresruimte.
    static constexpr uint64_t kStackTop = 0x000000C000000000ull;
    static constexpr uint64_t kHeapBase = 0x0000000200000000ull;
    static constexpr uint64_t kHleBase = 0x00007FFE00000000ull;
    static constexpr uint64_t kHleSize = 0x100000ull; // 65536 slots van 16 bytes
    static constexpr uint64_t kTebBase = 0x000000C000100000ull;
    static constexpr uint64_t kScratchBase = 0x0000000100000000ull;

    // Kleine scratch-allocator voor host-side structuren in gastgeheugen
    // (command line, environment, module-namen, ...).
    uint64_t allocScratch(uint64_t size, uint64_t align = 16);
    uint64_t internCString(const std::string& s);
    uint64_t internWideString(const std::string& s);

    void reportCrash(const std::exception& e);

private:
    void mapSections();
    void applyRelocations();
    void resolveImports();
    void finalizeProtections();
    void setupStack();

    EmulatorOptions opts_;
    Memory mem_;
    Cpu cpu_;
    std::unique_ptr<Win32> win32_;
    LoadedImage image_;
    uint64_t scratchCursor_ = kScratchBase;
    uint64_t scratchCommitted_ = kScratchBase;
    std::map<std::string, uint64_t> internedA_;
    std::map<std::string, uint64_t> internedW_;
};

} // namespace macemu
