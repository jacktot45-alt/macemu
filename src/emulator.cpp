#include "macemu/emulator.h"

#include <cstdio>

#include "macemu/feasibility.h"

namespace macemu {

Emulator::Emulator(const EmulatorOptions& opts) : opts_(opts), cpu_(mem_) {
    win32_.reset(new Win32(*this));
    cpu_.traceEnabled = opts_.trace;
    cpu_.traceLimit = opts_.traceLimit;
}

Emulator::~Emulator() = default;

uint64_t Emulator::allocScratch(uint64_t size, uint64_t align) {
    if (align == 0) align = 1;
    scratchCursor_ = (scratchCursor_ + align - 1) & ~(align - 1);
    uint64_t addr = scratchCursor_;
    scratchCursor_ += size;

    if (scratchCursor_ > scratchCommitted_) {
        uint64_t need = ((scratchCursor_ - scratchCommitted_) + 0xFFFF) & ~0xFFFFull;
        mem_.map(scratchCommitted_, need, PROT_RW, "macemu scratch");
        scratchCommitted_ += need;
    }
    return addr;
}

uint64_t Emulator::internCString(const std::string& s) {
    auto it = internedA_.find(s);
    if (it != internedA_.end()) return it->second;
    uint64_t addr = allocScratch(s.size() + 1, 8);
    mem_.writeCString(addr, s);
    internedA_[s] = addr;
    return addr;
}

uint64_t Emulator::internWideString(const std::string& s) {
    auto it = internedW_.find(s);
    if (it != internedW_.end()) return it->second;
    std::vector<uint16_t> w = utf8ToUtf16(s);
    uint64_t addr = allocScratch((w.size() + 1) * 2, 8);
    mem_.writeWideString(addr, s);
    internedW_[s] = addr;
    return addr;
}

int Emulator::execute() {
    // Returnadres voor het entry point: slot 0 van de HLE-tabel sluit het
    // proces netjes af als main() gewoon 'ret' doet.
    cpu_.push64(cpu_.hleSlotAddress(0));

    try {
        cpu_.run(opts_.maxInstructions);
    } catch (const std::exception& e) {
        reportCrash(e);
        return -1;
    }
    return static_cast<int>(cpu_.exitCode);
}

int Emulator::run() {
    try {
        pe::PeFile probe = pe::PeFile::parseFile(opts_.exePath);
        FeasibilityReport rep = assessFeasibility(probe);
        if (opts_.showAnalysis) {
            std::printf("Fase 0-oordeel voor %s: %s (%zu imports uit %zu DLL's)\n",
                        opts_.exePath.c_str(), rep.verdictText().c_str(), rep.totalImports,
                        rep.deps.size());
            for (const auto& b : rep.blockers) std::printf("  !! %s\n", b.c_str());
            std::fflush(stdout); // anders loopt dit door de stderr-logging heen
        }
        if (rep.verdict == Verdict::Blocked && !opts_.force) {
            std::fprintf(stderr,
                         "\nGestopt: Fase 0 zegt dat dit bestand niet haalbaar is met een\n"
                         "handgeschreven Win32-laag. Draai `macemu-peinfo %s` voor het volledige\n"
                         "rapport, of gebruik --force om het toch te proberen.\n",
                         opts_.exePath.c_str());
            return 3;
        }

        load();

        if (opts_.dryRun) {
            std::string map;
            mem_.dumpMap(map);
            std::printf("\nGeheugenkaart na laden:\n%s\n", map.c_str());
            std::printf("Entry point: 0x%llx\n", (unsigned long long)image_.entryPoint);
            if (!image_.unresolvedImports.empty()) {
                std::printf("\nNog niet geïmplementeerde imports (%zu):\n",
                            image_.unresolvedImports.size());
                std::string lastDll;
                for (const auto& u : image_.unresolvedImports) {
                    if (u.first != lastDll) {
                        std::printf("  %s\n", u.first.c_str());
                        lastDll = u.first;
                    }
                    std::printf("      %s\n", u.second.c_str());
                }
                std::printf(
                    "\nDit is de Fase 3-werklijst. Je hoeft ze niet allemaal te doen: begin\n"
                    "met draaien en implementeer telkens de functie waar de emulator op stopt.\n");
            }
            return 0;
        }

        int rc = execute();

        MACEMU_LOG_INFO("gestopt na %llu instructies, exit-code %d",
                        (unsigned long long)cpu_.instructionsExecuted, rc);
        if (g_logLevel >= LogLevel::Debug) {
            std::string usage;
            win32_->dumpApiUsage(usage);
            std::fprintf(stderr, "%s", usage.c_str());
        }
        if (!opts_.screenshotPath.empty() && !win32_->saveScreenshot(opts_.screenshotPath))
            MACEMU_LOG_WARN("geen venster om een screenshot van te maken");
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nfout: %s\n", e.what());
        return 1;
    }
}

void Emulator::reportCrash(const std::exception& e) {
    std::fprintf(stderr, "\n=== De geëmuleerde applicatie is gestopt ===\n");
    std::fprintf(stderr, "reden: %s\n\n", e.what());
    std::fprintf(stderr, "%s\n", cpu_.dumpState().c_str());
    std::fprintf(stderr, "%s\n", cpu_.dumpStack(6).c_str());

    // Bytes rond RIP: handig om te zien welke instructie ontbreekt.
    try {
        uint8_t buf[16];
        mem_.fetch(cpu_.rip, buf, sizeof(buf));
        std::fprintf(stderr, "bytes op rip:\n%s\n",
                     hexDump(buf, sizeof(buf), cpu_.rip).c_str());
    } catch (const std::exception&) {
    }

    if (cpu_.isHleAddress(cpu_.rip)) {
        uint32_t id = static_cast<uint32_t>((cpu_.rip - cpu_.hleBase()) / Cpu::kHleSlotSize);
        if (id < win32_->slots().size()) {
            const ApiEntry& s = win32_->slots()[id];
            std::fprintf(stderr, "de gast stond in de Win32-stub voor %s!%s\n", s.dll.c_str(),
                         s.name.c_str());
        }
    }

    std::string map;
    mem_.dumpMap(map);
    std::fprintf(stderr, "geheugenkaart:\n%s\n", map.c_str());

    std::fprintf(stderr,
                 "Tip: draai met --trace (en eventueel --trace-limit N) om te zien welke\n"
                 "instructies hieraan voorafgingen, of met --dry-run om alleen te laden.\n");
}

} // namespace macemu
