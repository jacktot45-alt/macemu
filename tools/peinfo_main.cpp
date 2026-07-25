// macemu-peinfo - Fase 0 gereedschap.
//
// Leest een .exe/.dll, print de PE-headers en geeft een eerlijk oordeel over
// de haalbaarheid van "dit draaien met een handgeschreven Win32-laag".
#include <cstdio>
#include <cstring>
#include <string>

#include "macemu/feasibility.h"
#include "macemu/pe.h"
#include "macemu/util.h"

using namespace macemu;

static void usage() {
    std::printf(
        "macemu-peinfo - PE/COFF-analyse en haalbaarheidscheck (Fase 0)\n"
        "\n"
        "Gebruik:\n"
        "  macemu-peinfo <bestand.exe> [opties]\n"
        "\n"
        "Opties:\n"
        "  -v, --verbose     print alle geïmporteerde symboolnamen\n"
        "  -e, --exports     print de export-tabel\n"
        "  -h, --help        deze tekst\n"
        "\n"
        "Exit-code: 0 = haalbaar, 1 = haalbaar met werk, 2 = grote sprong,\n"
        "           3 = geblokkeerd, 4 = fout bij lezen.\n");
}

int main(int argc, char** argv) {
    std::string path;
    bool verbose = false;
    bool showExports = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "-e" || a == "--exports") showExports = true;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "onbekende optie: %s\n", a.c_str());
            return 4;
        } else path = a;
    }

    if (path.empty()) { usage(); return 4; }

    try {
        pe::PeFile file = pe::PeFile::parseFile(path);
        FeasibilityReport rep = assessFeasibility(file);
        printFeasibilityReport(file, rep, verbose);

        if (showExports) {
            std::printf("Exports (%zu)\n", file.exports.size());
            for (const auto& e : file.exports) {
                if (!e.forwarder.empty())
                    std::printf("  #%-5u %-40s -> %s\n", e.ordinal,
                                e.name.empty() ? "(zonder naam)" : e.name.c_str(),
                                e.forwarder.c_str());
                else
                    std::printf("  #%-5u %-40s RVA 0x%08x\n", e.ordinal,
                                e.name.empty() ? "(zonder naam)" : e.name.c_str(), e.rva);
            }
            std::printf("\n");
        }

        switch (rep.verdict) {
            case Verdict::Feasible: return 0;
            case Verdict::FeasibleWork: return 1;
            case Verdict::BigJump: return 2;
            case Verdict::Blocked: return 3;
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fout: %s\n", e.what());
        return 4;
    }
}
