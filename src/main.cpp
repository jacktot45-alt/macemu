// macemu - command line
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "macemu/emulator.h"
#include "macemu/host_window.h"

using namespace macemu;

static void usage() {
    std::printf(
        "macemu - een x86-64 + Win32 emulator voor macOS (leerproject)\n"
        "\n"
        "Gebruik:\n"
        "  macemu <programma.exe> [opties] [-- args voor de gast]\n"
        "\n"
        "Opties:\n"
        "  --dry-run           laden + imports resolven, niet uitvoeren\n"
        "                      (toont de Fase 3-werklijst)\n"
        "  --trace             log elke uitgevoerde instructie\n"
        "  --trace-limit N     stop met loggen na N instructies\n"
        "  --max-instr N       breek af na N instructies (tegen oneindige lussen)\n"
        "  --headless          geen venster openen (tekent alleen in het geheugen)\n"
        "  --headless-secs S   hoe lang een headless GUI-app draait (standaard 1.5)\n"
        "  --screenshot P      schrijf het venster als .ppm weg bij afsluiten\n"
        "  --stub-unknown      onbekende imports geven 0 terug i.p.v. te stoppen\n"
        "  --force             negeer het Fase 0-oordeel 'GEBLOKKEERD'\n"
        "  --quiet             alleen fouten tonen\n"
        "  -v / -vv            meer logging (debug / trace)\n"
        "  --no-analysis       sla het Fase 0-regeltje bij het starten over\n"
        "  -h, --help          deze tekst\n"
        "\n"
        "Voorbeelden:\n"
        "  macemu-peinfo spel.exe            # Fase 0: is dit haalbaar?\n"
        "  macemu spel.exe --dry-run         # Fase 2: laadt het image?\n"
        "  macemu spel.exe -v                # Fase 3+: draaien\n"
        "  macemu spel.exe --headless --screenshot uit.ppm\n");
}

int main(int argc, char** argv) {
    EmulatorOptions opts;
    bool afterDashDash = false;

    if (const char* env = std::getenv("MACEMU_LOG")) {
        int lvl = std::atoi(env);
        if (lvl >= 0 && lvl <= 4) g_logLevel = static_cast<LogLevel>(lvl);
    }

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (afterDashDash) {
            opts.guestArgs.push_back(a);
            continue;
        }
        if (a == "--") { afterDashDash = true; }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--dry-run") opts.dryRun = true;
        else if (a == "--trace") { opts.trace = true; g_logLevel = LogLevel::Trace; }
        else if (a == "--trace-limit" && i + 1 < argc) opts.traceLimit = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--max-instr" && i + 1 < argc) opts.maxInstructions = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--headless") opts.headless = true;
        else if (a == "--headless-secs" && i + 1 < argc) opts.headlessSeconds = std::atof(argv[++i]);
        else if (a == "--screenshot" && i + 1 < argc) opts.screenshotPath = argv[++i];
        else if (a == "--stub-unknown") opts.stubUnknown = true;
        else if (a == "--force") opts.force = true;
        else if (a == "--no-analysis") opts.showAnalysis = false;
        else if (a == "--quiet") g_logLevel = LogLevel::Error;
        else if (a == "-v") g_logLevel = LogLevel::Debug;
        else if (a == "-vv") g_logLevel = LogLevel::Trace;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "onbekende optie: %s\n", a.c_str());
            return 2;
        } else if (opts.exePath.empty()) {
            opts.exePath = a;
        } else {
            opts.guestArgs.push_back(a);
        }
    }

    if (opts.exePath.empty()) {
        usage();
        return 2;
    }
    if (!HostWindow::sdlAvailable() && !opts.headless)
        MACEMU_LOG_DEBUG("zonder SDL2 gebouwd - GUI-apps draaien headless");

    Emulator emu(opts);
    return emu.run();
}
