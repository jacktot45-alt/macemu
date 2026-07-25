// End-to-end test: de complete emulator op de gegenereerde .exe-bestanden.
//
// Dit is de test die telt: PE-loader + CPU + Win32-laag + (headless) venster.
#include <unistd.h>

#include <cstdio>
#include <string>

#include "macemu/emulator.h"
#include "test_util.h"

using namespace macemu;

namespace {

// Zoekt de RVA van een sectie op naam.
uint32_t sectionRva(const pe::PeFile& p, const char* name) {
    for (const auto& s : p.sections)
        if (s.name == name) return s.virtualAddress;
    return 0;
}

} // namespace

int main() {
    g_logLevel = LogLevel::Error; // testuitvoer schoon houden

    TEST("hello.exe draait en schrijft via WriteFile");
    {
        EmulatorOptions o;
        o.exePath = "fixtures/hello.exe";
        o.showAnalysis = false;
        o.maxInstructions = 1000000;
        Emulator emu(o);
        emu.load();

        CHECK_EQ(emu.image().base, 0x140000000ull);
        CHECK_EQ(emu.image().entryPoint, 0x140001000ull);
        CHECK(emu.image().unresolvedImports.empty());

        // stdout tijdelijk omleiden zodat we kunnen controleren wat de gast
        // schrijft (en de testuitvoer leesbaar blijft)
        std::fflush(stdout);
        int savedFd = dup(fileno(stdout));
        FILE* tmp = std::fopen("hello_out.txt", "w");
        CHECK(tmp != nullptr);
        dup2(fileno(tmp), fileno(stdout));
        int rc = emu.execute();
        std::fflush(stdout);
        dup2(savedFd, fileno(stdout));
        close(savedFd);
        std::fclose(tmp);

        CHECK_EQ(rc, 0);
        CHECK(emu.cpu().instructionsExecuted > 5);

        // De gast schreef het aantal geschreven bytes in .data.
        uint32_t dataRva = sectionRva(emu.image().pe, ".data");
        CHECK(dataRva != 0);
        uint32_t written = emu.memory().read32(emu.image().base + dataRva);
        CHECK(written > 50);

        // En de tekst moet echt in het bestand staan.
        FILE* f = std::fopen("hello_out.txt", "rb");
        CHECK(f != nullptr);
        if (f) {
            char buf[512] = {0};
            size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            CHECK(n > 50);
            CHECK(std::string(buf).find("macemu") != std::string::npos);
            std::remove("hello_out.txt");
        }
    }

    TEST("window.exe opent een venster en tekent erin");
    {
        EmulatorOptions o;
        o.exePath = "fixtures/window.exe";
        o.showAnalysis = false;
        o.headless = true;
        o.headlessSeconds = 0.15;
        o.maxInstructions = 50000000;
        o.screenshotPath = "window_test.ppm";
        Emulator emu(o);
        emu.load();
        int rc = emu.execute();
        CHECK_EQ(rc, 0);

        // Er moet precies één venster zijn aangemaakt, met een WndProc.
        auto& windows = emu.win32().windowMap();
        CHECK_EQ(windows.size(), size_t(1));
        if (!windows.empty()) {
            WindowObject& w = windows.begin()->second;
            CHECK(w.wndProc != 0);
            CHECK(w.title == "macemu - Fase 4");
            CHECK(w.clientWidth > 100);

            // WM_PAINT moet in de client-framebuffer zichtbaar zijn: er is
            // wit (FillRect) en er zijn zwarte pixels (tekst + blokje).
            int white = 0, dark = 0;
            for (uint32_t px : w.client.pixels) {
                if (px == 0xFFFFFF) ++white;
                if ((px & 0xFFFFFF) == 0x000000) ++dark;
            }
            CHECK(white > 1000);
            CHECK(dark > 100);
        }

        CHECK(emu.win32().saveScreenshot("window_test.ppm"));
        FILE* f = std::fopen("window_test.ppm", "rb");
        CHECK(f != nullptr);
        if (f) {
            std::fclose(f);
            std::remove("window_test.ppm");
        }
    }

    TEST("crash.exe geeft een bruikbare foutmelding over de ontbrekende API");
    {
        EmulatorOptions o;
        o.exePath = "fixtures/crash.exe";
        o.showAnalysis = false;
        o.maxInstructions = 100000;
        Emulator emu(o);
        emu.load();

        // De onbekende import staat in de werklijst.
        bool found = false;
        for (const auto& u : emu.image().unresolvedImports)
            if (u.second == "CreateHardLinkW") found = true;
        CHECK(found);

        bool threw = false;
        try {
            emu.cpu().push64(emu.cpu().hleSlotAddress(0));
            emu.cpu().run(o.maxInstructions);
        } catch (const std::exception& e) {
            threw = true;
            std::string msg = e.what();
            CHECK(msg.find("CreateHardLinkW") != std::string::npos);
            CHECK(msg.find("niet-geïmplementeerde") != std::string::npos);
        }
        CHECK(threw);
    }

    TEST("--stub-unknown laat crash.exe wel doorlopen");
    {
        EmulatorOptions o;
        o.exePath = "fixtures/crash.exe";
        o.showAnalysis = false;
        o.stubUnknown = true;
        o.maxInstructions = 100000;
        Emulator emu(o);
        emu.load();
        int rc = emu.execute();
        CHECK_EQ(rc, 0);
    }

    return testing::summary("test_integration");
}
