// macemu - PE-loader (Fase 2)
//
// Doet wat de Windows-loader doet, maar dan het minimum:
//   1. headers + secties in het gastgeheugen zetten
//   2. relocations toepassen als we niet op de voorkeursbase kunnen laden
//   3. de import address table vullen met HLE-adressen
//   4. stack, TEB/PEB en command line opzetten
//   5. TLS-callbacks draaien en daarna naar het entry point springen
#include "macemu/emulator.h"
#include "macemu/feasibility.h"

namespace macemu {

namespace {

uint32_t protFromSection(uint32_t chars) {
    uint32_t p = 0;
    if (chars & pe::kSecMemRead) p |= PROT_R;
    if (chars & pe::kSecMemWrite) p |= PROT_W;
    if (chars & pe::kSecMemExecute) p |= PROT_X;
    if (p == 0) p = PROT_R;
    return p;
}

} // namespace

void Emulator::mapSections() {
    const pe::PeFile& p = image_.pe;

    uint64_t base = p.imageBase;
    if (base == 0) base = p.is64 ? 0x140000000ull : 0x400000ull;

    // Botst de voorkeursbase met iets? Zoek dan een vrij gat (en reloceer).
    bool collides = false;
    for (uint64_t a = base; a < base + p.sizeOfImage; a += Memory::kPageSize) {
        if (mem_.isMapped(a)) { collides = true; break; }
    }
    if (collides) {
        if (!p.hasRelocations())
            throw EmuError("image kan niet op zijn voorkeursbase geladen worden en heeft geen "
                           "relocations");
        base = mem_.findFreeRegion(p.sizeOfImage, 0x10000, 0x10000000ull);
        MACEMU_LOG_WARN("image verplaatst naar 0x%llx", (unsigned long long)base);
    }

    image_.base = base;
    image_.size = p.sizeOfImage;
    image_.entryPoint = base + p.entryPointRva;

    // Headers: de CRT en sommige APIs lezen de DOS/PE-header uit het image.
    uint64_t hdrSize = p.sizeOfHeaders ? p.sizeOfHeaders : 0x1000;
    mem_.map(base, hdrSize, PROT_RW, image_.name + " (headers)");
    size_t copy = std::min<size_t>(hdrSize, p.raw.size());
    mem_.writeBlock(base, p.raw.data(), copy);
    mem_.protect(base, hdrSize, PROT_R);

    for (const auto& s : p.sections) {
        uint64_t va = base + s.virtualAddress;
        uint64_t vsize = s.virtualSize ? s.virtualSize : s.rawSize;
        if (vsize == 0) continue;
        // Eerst schrijfbaar mappen zodat we de inhoud kunnen kopiëren.
        mem_.map(va, vsize, PROT_RW, image_.name + " " + s.name);
        if (s.rawSize > 0 && s.rawPointer < p.raw.size()) {
            uint64_t n = std::min<uint64_t>(s.rawSize, p.raw.size() - s.rawPointer);
            n = std::min<uint64_t>(n, vsize);
            mem_.writeBlock(va, p.raw.data() + s.rawPointer, static_cast<size_t>(n));
        }
        MACEMU_LOG_DEBUG("sectie %-8s -> 0x%llx (%llu bytes, %s%s%s)", s.name.c_str(),
                         (unsigned long long)va, (unsigned long long)vsize,
                         (s.characteristics & pe::kSecMemRead) ? "r" : "-",
                         (s.characteristics & pe::kSecMemWrite) ? "w" : "-",
                         (s.characteristics & pe::kSecMemExecute) ? "x" : "-");
    }
}

void Emulator::applyRelocations() {
    const pe::PeFile& p = image_.pe;
    if (image_.base == p.imageBase) return;
    if (p.relocations.empty()) return;

    int64_t delta = static_cast<int64_t>(image_.base) - static_cast<int64_t>(p.imageBase);
    size_t applied = 0;
    for (const auto& r : p.relocations) {
        uint64_t va = image_.base + r.rva;
        switch (r.type) {
            case pe::kRelocDir64:
                mem_.write64(va, mem_.read64(va) + static_cast<uint64_t>(delta));
                ++applied;
                break;
            case pe::kRelocHighLow:
                mem_.write32(va, mem_.read32(va) + static_cast<uint32_t>(delta));
                ++applied;
                break;
            default:
                MACEMU_LOG_WARN("relocation-type %u nog niet ondersteund", r.type);
                break;
        }
    }
    MACEMU_LOG_INFO("%zu relocations toegepast (delta 0x%llx)", applied,
                    (unsigned long long)delta);
}

void Emulator::resolveImports() {
    const pe::PeFile& p = image_.pe;
    size_t resolved = 0, stubbed = 0;

    for (const auto& mod : p.imports) {
        for (const auto& sym : mod.symbols) {
            uint64_t slotAddr = win32_->resolveImport(mod.dllName, sym.name, sym.ordinal,
                                                      sym.byOrdinal);
            uint64_t iatVa = image_.base + sym.iatRva;
            mem_.write64(iatVa, slotAddr);
            if (win32_->hasApi(mod.dllName, sym.name)) ++resolved;
            else {
                ++stubbed;
                image_.unresolvedImports.emplace_back(mod.dllName, sym.byOrdinal
                                                                       ? strFormat("#%u", sym.ordinal)
                                                                       : sym.name);
            }
        }
    }
    MACEMU_LOG_INFO("imports: %zu geïmplementeerd, %zu nog niet (worden pas een probleem als de "
                    "gast ze echt aanroept)", resolved, stubbed);
}

void Emulator::finalizeProtections() {
    // Tijdens het laden staat alles op RW zodat we kunnen schrijven. Nu pas
    // krijgen de secties hun echte rechten - dat is precies wat een fout als
    // "schrijven naar .text" meteen zichtbaar maakt.
    const pe::PeFile& p = image_.pe;
    uint32_t iatRva = p.dataDirs[pe::kDirIat].rva;
    uint32_t iatSize = p.dataDirs[pe::kDirIat].size;

    for (const auto& s : p.sections) {
        uint64_t va = image_.base + s.virtualAddress;
        uint64_t vsize = s.virtualSize ? s.virtualSize : s.rawSize;
        if (vsize == 0) continue;
        uint32_t prot = protFromSection(s.characteristics);
        mem_.protect(va, vsize, prot);
    }
    // De IAT moet schrijfbaar blijven: delay-load en GetProcAddress-patches
    // schrijven daar tijdens het draaien in.
    if (iatRva && iatSize)
        mem_.protect(image_.base + iatRva, iatSize, PROT_RW);
    // Idem voor de import-directory zelf (sommige runtimes patchen thunks).
    if (p.dataDirs[pe::kDirImport].rva)
        mem_.protect(image_.base + p.dataDirs[pe::kDirImport].rva,
                     p.dataDirs[pe::kDirImport].size ? p.dataDirs[pe::kDirImport].size : 0x1000,
                     PROT_RW);
}

void Emulator::setupStack() {
    uint64_t stackSize = opts_.stackSize;
    if (image_.pe.sizeOfStackReserve > stackSize) stackSize = image_.pe.sizeOfStackReserve;
    if (stackSize < 256 * 1024) stackSize = 256 * 1024;
    stackSize = (stackSize + 0xFFFF) & ~0xFFFFull;

    uint64_t stackBase = kStackTop - stackSize;
    mem_.map(stackBase, stackSize, PROT_RW, "stack");

    // Een guard page onderaan zodat stack overflow een duidelijke fout geeft.
    mem_.protect(stackBase, Memory::kPageSize, PROT_NONE_);

    // RSP 16-byte aligned, met wat ruimte zodat de eerste push niet meteen
    // buiten de stack valt.
    cpu_.gpr[RSP] = (kStackTop - 0x200) & ~0xFull;
    MACEMU_LOG_DEBUG("stack: 0x%llx-0x%llx, rsp=0x%llx", (unsigned long long)stackBase,
                     (unsigned long long)kStackTop, (unsigned long long)cpu_.gpr[RSP]);
}

void Emulator::load() {
    image_.pe = pe::PeFile::parseFile(opts_.exePath);
    size_t slash = opts_.exePath.find_last_of("/\\");
    image_.name = (slash == std::string::npos) ? opts_.exePath : opts_.exePath.substr(slash + 1);

    if (image_.pe.machine != pe::kMachineAmd64) {
        throw EmuError(strFormat(
            "macemu's CPU-core ondersteunt alleen x86-64 (AMD64). Dit bestand is %s.\n"
            "  Draai eerst `macemu-peinfo %s` voor het volledige Fase 0-rapport.",
            image_.pe.machineName().c_str(), opts_.exePath.c_str()));
    }
    if (image_.pe.hasClrHeader && !opts_.force) {
        throw EmuError(
            "dit is een .NET-assembly: de echte code is IL-bytecode, geen x86. Een x86-emulator "
            "voert hier niets zinvols van uit.\n"
            "  Gebruik --force als je het toch wilt proberen (verwacht een crash in mscoree).");
    }

    mapSections();
    applyRelocations();

    win32_->initialize(kHleBase, kHleSize);
    win32_->stubUnknown = opts_.stubUnknown;
    resolveImports();

    finalizeProtections();
    setupStack();
    win32_->setupProcessEnvironment(image_.base, opts_.exePath, opts_.guestArgs);

    cpu_.rip = image_.entryPoint;
    cpu_.gsBase = win32_->tebAddress();
    // Win64 verwacht bij het entry point: RCX = hInstance-achtig, en een
    // uitgelijnde stack. De echte loader springt via ntdll; dat imiteren we
    // door een returnadres te pushen dat het proces netjes afsluit.
    cpu_.gpr[RCX] = image_.base;
    cpu_.gpr[RDX] = 0;

    MACEMU_LOG_INFO("image geladen op 0x%llx, entry point 0x%llx",
                    (unsigned long long)image_.base, (unsigned long long)image_.entryPoint);
}

} // namespace macemu
