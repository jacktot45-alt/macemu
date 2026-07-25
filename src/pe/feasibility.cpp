#include "macemu/feasibility.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace macemu {

namespace {

bool startsWith(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

bool contains(const std::string& s, const char* p) {
    return s.find(p) != std::string::npos;
}

struct DllRule {
    const char* name;
    DepClass cls;
    const char* note;
};

// Deze tabel is het hart van de haalbaarheidscheck. Hij groeit mee met wat
// macemu daadwerkelijk implementeert - zie docs/status.md.
const DllRule kRules[] = {
    // --- waar we stubs voor hebben --------------------------------------
    {"kernel32.dll", DepClass::Implemented, "proces/heap/geheugen/handles - kernstubs aanwezig"},
    {"kernelbase.dll", DepClass::Implemented, "wordt op kernel32-stubs afgebeeld"},
    {"user32.dll", DepClass::Implemented, "vensters, message loop, input - basis aanwezig"},
    {"gdi32.dll", DepClass::Implemented, "tekenprimitieven op een software-framebuffer"},
    {"msvcrt.dll", DepClass::Implemented, "klassieke CRT - veelgebruikte functies aanwezig"},
    {"ntdll.dll", DepClass::Planned, "alleen de Rtl*-functies die de CRT echt aanroept"},

    // --- realistisch bij te bouwen ---------------------------------------
    {"advapi32.dll", DepClass::Planned, "registry/crypto - een fake registry is goed te doen"},
    {"shell32.dll", DepClass::Planned, "meestal maar een paar calls (paden, iconen)"},
    {"shlwapi.dll", DepClass::Planned, "pure padmanipulatie, eenvoudig na te bouwen"},
    {"comdlg32.dll", DepClass::Planned, "open/save-dialogen; kan naar een host-dialoog"},
    {"winmm.dll", DepClass::Planned, "timers en simpele geluidsweergave"},
    {"version.dll", DepClass::Planned, "versie-resources uitlezen, klein"},
    {"ole32.dll", DepClass::Planned, "alleen CoInitialize/CoTaskMemAlloc is vaak genoeg"},
    {"oleaut32.dll", DepClass::Planned, "BSTR/VARIANT - overzichtelijk maar veel functies"},
    {"imm32.dll", DepClass::Planned, "IME - stubs volstaan voor westerse invoer"},
    {"vcruntime140.dll", DepClass::Planned, "moderne MSVC-runtime, matig aantal functies"},
    {"vcruntime140_1.dll", DepClass::Planned, "C++ exception unwinding (x64) - lastig maar klein"},
    {"msvcp140.dll", DepClass::Planned, "C++ standard library, veel symbolen"},
    {"ucrtbase.dll", DepClass::Planned, "moderne CRT, groot maar goed gedocumenteerd"},

    // --- grote sprong ------------------------------------------------------
    {"comctl32.dll", DepClass::BigJump, "common controls = een hele widget-toolkit nabouwen"},
    {"uxtheme.dll", DepClass::BigJump, "visual styles; kan vaak worden gestubd naar 'geen thema'"},
    {"gdiplus.dll", DepClass::BigJump, "GDI+ is een complete 2D-graphics library"},
    {"ws2_32.dll", DepClass::BigJump, "sockets naar host-sockets vertalen; apart deelproject"},
    {"winspool.drv", DepClass::BigJump, "printen - vrijwel altijd te stubben"},
    {"rpcrt4.dll", DepClass::BigJump, "RPC/marshalling"},
    {"dwmapi.dll", DepClass::BigJump, "desktop composition; meestal veilig te stubben"},

    // --- blockers ----------------------------------------------------------
    {"mscoree.dll", DepClass::Blocker, ".NET Framework host - vereist een complete CLR"},
    {"mscoreei.dll", DepClass::Blocker, ".NET Framework host"},
    {"combase.dll", DepClass::Blocker, "WinRT/COM-basis - kern van UWP-apps"},
    {"d3d9.dll", DepClass::Blocker, "Direct3D 9 - GPU-API vertalen naar Metal is een eigen project"},
    {"d3d10.dll", DepClass::Blocker, "Direct3D 10"},
    {"d3d11.dll", DepClass::Blocker, "Direct3D 11 - zeer grote COM-API"},
    {"d3d12.dll", DepClass::Blocker, "Direct3D 12"},
    {"dxgi.dll", DepClass::Blocker, "DXGI hoort bij Direct3D 10+"},
    {"d2d1.dll", DepClass::Blocker, "Direct2D - grote COM-API bovenop D3D"},
    {"dwrite.dll", DepClass::Blocker, "DirectWrite - tekst-rendering stack"},
    {"windowscodecs.dll", DepClass::Blocker, "WIC - image codecs via COM"},
    {"xaudio2_9.dll", DepClass::Blocker, "XAudio2"},
    {"dcomp.dll", DepClass::Blocker, "DirectComposition"},
};

} // namespace

DepClass classifyDll(const std::string& lower, std::string& noteOut) {
    for (const auto& r : kRules) {
        if (lower == r.name) {
            noteOut = r.note;
            return r.cls;
        }
    }
    // API sets: api-ms-win-core-*, ext-ms-*. Dat zijn forwarder-DLL's.
    if (startsWith(lower, "api-ms-win-core-winrt") || startsWith(lower, "api-ms-win-core-com")) {
        noteOut = "WinRT/COM API-set - wijst op een UWP/Store-app";
        return DepClass::Blocker;
    }
    if (startsWith(lower, "api-ms-win-crt-")) {
        noteOut = "UCRT API-set, wordt doorgestuurd naar ucrtbase";
        return DepClass::Planned;
    }
    if (startsWith(lower, "api-ms-win-") || startsWith(lower, "ext-ms-win-")) {
        noteOut = "API-set (forwarder) - moet naar de echte DLL worden gemapt";
        return DepClass::Planned;
    }
    if (startsWith(lower, "d3d") || startsWith(lower, "dxgi") || startsWith(lower, "xaudio") ||
        startsWith(lower, "d2d")) {
        noteOut = "DirectX-component";
        return DepClass::Blocker;
    }
    if (contains(lower, "xaml") || contains(lower, "windows.ui")) {
        noteOut = "XAML/WinUI - UWP-presentatielaag";
        return DepClass::Blocker;
    }
    noteOut = "onbekende DLL - handmatig onderzoeken welke symbolen nodig zijn";
    return DepClass::Unknown;
}

std::string FeasibilityReport::verdictText() const {
    switch (verdict) {
        case Verdict::Feasible: return "HAALBAAR";
        case Verdict::FeasibleWork: return "HAALBAAR MET WERK";
        case Verdict::BigJump: return "GROTE SPRONG";
        case Verdict::Blocked: return "GEBLOKKEERD";
    }
    return "?";
}

FeasibilityReport assessFeasibility(const pe::PeFile& pe) {
    FeasibilityReport rep;

    // 1. Architectuur.
    if (pe.machine == pe::kMachineArm64 || pe.machine == pe::kMachineArmNT) {
        rep.blockers.push_back(
            "De executable is voor ARM gecompileerd, niet voor x86(-64). Een x86-emulator "
            "helpt hier niet; je zou een ARM-emulator nodig hebben (of, op Apple Silicon, "
            "de instructies zijn native maar de Windows-ABI verschilt nog steeds).");
    } else if (pe.machine != pe::kMachineAmd64 && pe.machine != pe::kMachineI386) {
        rep.blockers.push_back("Onbekende/niet-ondersteunde machine-architectuur: " +
                               pe.machineName());
    }
    if (pe.machine == pe::kMachineI386) {
        rep.warnings.push_back(
            "32-bit x86 (i386). macemu's CPU-core is op 64-bit long mode gericht; "
            "32-bit werkt met dezelfde decoder maar vereist een aparte ABI (stdcall, "
            "argumenten op de stack) in de Win32-laag.");
    }

    // 2. .NET?
    if (pe.hasClrHeader) {
        rep.isDotNet = true;
        bool ilOnly = (pe.clrFlags & 1) != 0;
        rep.blockers.push_back(strFormat(
            "Dit is een .NET-assembly (CLR-header aanwezig, runtime v%u.%u, %s). De echte "
            "code zit als IL-bytecode in het bestand, niet als x86. Een x86-emulator voert "
            "hier vrijwel niets van uit: je zou een complete CLR (JIT + GC + BCL) moeten "
            "implementeren of embedden. Dat is een compleet ander, veel groter project.",
            pe.clrMajorRuntime, pe.clrMinorRuntime, ilOnly ? "IL-only" : "mixed mode"));
    }

    // 3. Dependencies classificeren.
    std::map<std::string, size_t> symCount;
    for (const auto& m : pe.imports) {
        std::string lower = m.dllName;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        symCount[lower] += m.symbols.size();
        rep.totalImports += m.symbols.size();
    }

    int nBlocker = 0, nBigJump = 0, nPlanned = 0, nUnknown = 0;
    for (const auto& kv : symCount) {
        DepInfo d;
        d.dll = kv.first;
        d.symbolCount = kv.second;
        d.cls = classifyDll(kv.first, d.note);
        switch (d.cls) {
            case DepClass::Blocker: ++nBlocker; break;
            case DepClass::BigJump: ++nBigJump; break;
            case DepClass::Planned: ++nPlanned; break;
            case DepClass::Unknown: ++nUnknown; break;
            default: break;
        }
        if (d.cls == DepClass::Blocker &&
            (startsWith(d.dll, "d3d") || startsWith(d.dll, "d2d") || startsWith(d.dll, "dxgi") ||
             startsWith(d.dll, "dwrite")))
            rep.needsDirectX = true;
        if (d.cls == DepClass::Blocker &&
            (d.dll == "combase.dll" || startsWith(d.dll, "api-ms-win-core-winrt")))
            rep.isWinRT = true;
        rep.deps.push_back(d);
    }

    if (rep.isWinRT) {
        rep.blockers.push_back(
            "De executable gebruikt WinRT/COM-basis (combase / api-ms-win-core-winrt). Dat "
            "wijst sterk op een UWP/Store-app. UWP betekent: een activation-model met "
            "COM-interfaces, een XAML-presentatielaag en een app-container. Dat is niet met "
            "een handvol Win32-stubs na te bouwen.");
    }
    if (rep.needsDirectX) {
        rep.blockers.push_back(
            "De executable rendert via DirectX (D3D/D2D/DirectWrite). Die API's zijn "
            "COM-gebaseerd en enorm; ze vertalen naar Metal of OpenGL is op zichzelf een "
            "project van jaren (vergelijk: DXVK, MoltenVK).");
    }

    // 4. Extra signalen.
    if (pe.subsystem == pe::kSubsystemWindowsGui)
        rep.notes.push_back("Subsystem = Windows GUI: de app opent vensters, dus Fase 4 "
                            "(user32/gdi32 + tekenlaag) is verplicht, niet optioneel.");
    if (pe.subsystem == pe::kSubsystemWindowsCui)
        rep.notes.push_back("Subsystem = Console: je kunt heel ver komen zonder enige "
                            "grafische laag. Ideaal startpunt.");
    if (pe.subsystem == pe::kSubsystemNative)
        rep.warnings.push_back("Subsystem = Native: dit is een driver of ntdll-achtige "
                               "component, geen normale applicatie.");

    if (pe.isDll())
        rep.warnings.push_back("Dit bestand is een DLL, geen .exe. Er is geen normaal "
                               "entry point om te starten (alleen DllMain).");

    if (!pe.hasRelocations() && pe.isAslr())
        rep.warnings.push_back("ASLR staat aan maar er is geen relocation-directory; "
                               "het image moet op zijn voorkeurs-imagebase geladen worden.");

    if (pe.hasTls)
        rep.notes.push_back("Er is een TLS-directory met callbacks: die moeten vóór het "
                            "entry point worden aangeroepen.");

    if (pe.dataDirs[pe::kDirException].size > 0 && pe.is64)
        rep.notes.push_back("Er is een .pdata/exception-directory (x64 SEH). Zolang de app "
                            "geen exceptions gooit heb je die niet nodig; zodra wel, is "
                            "unwinding een flink stuk werk.");

    if (rep.totalImports == 0 && !pe.hasClrHeader)
        rep.warnings.push_back("Geen imports gevonden. Mogelijk is het bestand gepackt "
                               "(UPX/installer) of laadt het alles dynamisch. Uitpakken of "
                               "installeren en de echte .exe analyseren.");

    // 5. Eindoordeel.
    if (!rep.blockers.empty()) {
        rep.verdict = Verdict::Blocked;
    } else if (nBigJump > 0 || rep.totalImports > 400) {
        rep.verdict = Verdict::BigJump;
    } else if (nPlanned > 2 || nUnknown > 0 || rep.totalImports > 80) {
        rep.verdict = Verdict::FeasibleWork;
    } else {
        rep.verdict = Verdict::Feasible;
    }

    // 6. Advies.
    if (rep.verdict == Verdict::Blocked) {
        if (rep.isDotNet || rep.isWinRT) {
            rep.nextSteps.push_back(
                "Kies een ander doelbestand. Voor Minesweeper: de Win32-versie uit "
                "Windows XP/Vista/7 (winmine.exe of MineSweeper.exe) is puur kernel32 + "
                "user32 + gdi32 + een paar advapi32-calls. Dat is wél haalbaar.");
        }
        rep.nextSteps.push_back(
            "Draai deze analyse op het alternatieve bestand en kom pas terug bij Fase 1 "
            "als het oordeel HAALBAAR of HAALBAAR MET WERK is.");
    } else {
        rep.nextSteps.push_back(
            "Ga door naar Fase 2: laad het image met `macemu <exe> --dry-run` en kijk welke "
            "import de emulator als eerste tegenkomt.");
        rep.nextSteps.push_back(
            "Implementeer Win32-functies pas als de emulator erop vastloopt - niet vooraf "
            "raden welke nodig zijn.");
    }
    return rep;
}

void printFeasibilityReport(const pe::PeFile& pe, const FeasibilityReport& rep, bool verbose) {
    std::printf("=================================================================\n");
    std::printf(" FASE 0 - HAALBAARHEIDSRAPPORT\n");
    std::printf(" bestand: %s\n", pe.path.empty() ? "(geheugen)" : pe.path.c_str());
    std::printf("=================================================================\n\n");

    std::printf("PE-header\n");
    std::printf("  machine            : %s\n", pe.machineName().c_str());
    std::printf("  formaat            : %s\n", pe.is64 ? "PE32+ (64-bit)" : "PE32 (32-bit)");
    std::printf("  type               : %s\n", pe.isDll() ? "DLL" : "EXE");
    std::printf("  subsystem          : %s (v%u.%u)\n", pe.subsystemName().c_str(),
                pe.majorSubsystemVersion, pe.minorSubsystemVersion);
    std::printf("  image base         : 0x%llx\n", (unsigned long long)pe.imageBase);
    std::printf("  entry point        : RVA 0x%x  (VA 0x%llx)\n", pe.entryPointRva,
                (unsigned long long)(pe.imageBase + pe.entryPointRva));
    std::printf("  size of image      : %u bytes\n", pe.sizeOfImage);
    std::printf("  stack reserve      : %llu bytes\n", (unsigned long long)pe.sizeOfStackReserve);
    std::printf("  relocations        : %s (%zu entries)\n",
                pe.hasRelocations() ? "ja" : "nee", pe.relocations.size());
    std::printf("  ASLR (dyn. base)   : %s\n", pe.isAslr() ? "ja" : "nee");
    std::printf("  TLS-directory      : %s\n", pe.hasTls ? "ja" : "nee");
    std::printf("  .NET/CLR-header    : %s\n", pe.hasClrHeader ? "JA" : "nee");
    std::printf("\n");

    std::printf("Secties (%zu)\n", pe.sections.size());
    std::printf("  naam      RVA         v.size      raw.size    rechten\n");
    for (const auto& s : pe.sections) {
        char prot[4] = {
            static_cast<char>((s.characteristics & pe::kSecMemRead) ? 'r' : '-'),
            static_cast<char>((s.characteristics & pe::kSecMemWrite) ? 'w' : '-'),
            static_cast<char>((s.characteristics & pe::kSecMemExecute) ? 'x' : '-'),
            0,
        };
        std::printf("  %-8s  0x%08x  %10u  %10u  %s\n", s.name.c_str(), s.virtualAddress,
                    s.virtualSize, s.rawSize, prot);
    }
    std::printf("\n");

    std::printf("Afhankelijkheden (%zu DLL's, %zu geïmporteerde symbolen)\n", rep.deps.size(),
                rep.totalImports);
    if (rep.deps.empty()) std::printf("  (geen)\n");
    for (const auto& d : rep.deps) {
        const char* tag = "?";
        switch (d.cls) {
            case DepClass::Implemented: tag = "[OK      ]"; break;
            case DepClass::Planned:     tag = "[TE DOEN ]"; break;
            case DepClass::BigJump:     tag = "[GROOT   ]"; break;
            case DepClass::Blocker:     tag = "[BLOKKADE]"; break;
            case DepClass::Unknown:     tag = "[ONBEKEND]"; break;
        }
        std::printf("  %s %-28s %4zu symbolen  %s\n", tag, d.dll.c_str(), d.symbolCount,
                    d.note.c_str());
    }
    std::printf("\n");

    if (verbose) {
        for (const auto& m : pe.imports) {
            std::printf("  %s%s\n", m.dllName.c_str(), m.delayLoaded ? "  (delay-load)" : "");
            for (const auto& s : m.symbols) {
                if (s.byOrdinal)
                    std::printf("      #%u\n", s.ordinal);
                else
                    std::printf("      %s\n", s.name.c_str());
            }
        }
        std::printf("\n");
    }

    if (!rep.blockers.empty()) {
        std::printf("BLOKKADES\n");
        for (const auto& b : rep.blockers) std::printf("  !! %s\n\n", b.c_str());
    }
    if (!rep.warnings.empty()) {
        std::printf("WAARSCHUWINGEN\n");
        for (const auto& w : rep.warnings) std::printf("  *  %s\n\n", w.c_str());
    }
    if (!rep.notes.empty()) {
        std::printf("OPMERKINGEN\n");
        for (const auto& n : rep.notes) std::printf("  -  %s\n\n", n.c_str());
    }

    std::printf("-----------------------------------------------------------------\n");
    std::printf("OORDEEL: %s\n", rep.verdictText().c_str());
    switch (rep.verdict) {
        case Verdict::Feasible:
            std::printf("  Kleine, bekende dependency-set. Dit is een goed doelbestand om\n"
                        "  Fase 1-3 op te bouwen.\n");
            break;
        case Verdict::FeasibleWork:
            std::printf("  Te doen, maar reken op weken tot maanden aan API-werk. Bouw\n"
                        "  incrementeel: implementeer telkens de functie waar de emulator\n"
                        "  op vastloopt.\n");
            break;
        case Verdict::BigJump:
            std::printf("  Dit vereist hele subsystemen (widget-toolkit, sockets, GDI+).\n"
                        "  Kan, maar het is geen kwestie van 'nog een paar stubs'. Overweeg\n"
                        "  een eenvoudiger doelbestand om de emulator eerst te bewijzen.\n");
            break;
        case Verdict::Blocked:
            std::printf("  Met een handgeschreven Win32-laag kom je hier NIET. Er is een\n"
                        "  complete externe runtime nodig. Doorbouwen op dit doelbestand is\n"
                        "  een doodlopend pad.\n");
            break;
    }
    std::printf("\nVOLGENDE STAPPEN\n");
    for (const auto& s : rep.nextSteps) std::printf("  -> %s\n", s.c_str());
    std::printf("\n");
}

} // namespace macemu
