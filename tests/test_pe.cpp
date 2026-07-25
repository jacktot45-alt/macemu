// Fase 0/2-test: PE-parser en haalbaarheidscheck op de gegenereerde fixtures.
#include <string>

#include "macemu/feasibility.h"
#include "macemu/pe.h"
#include "test_util.h"

using namespace macemu;

int main() {
    TEST("hello.exe parsen");
    {
        pe::PeFile p = pe::PeFile::parseFile("fixtures/hello.exe");
        CHECK_EQ(p.machine, pe::kMachineAmd64);
        CHECK(p.is64);
        CHECK(!p.isDll());
        CHECK_EQ(p.subsystem, pe::kSubsystemWindowsCui);
        CHECK_EQ(p.imageBase, 0x140000000ull);
        CHECK_EQ(p.entryPointRva, 0x1000u);
        CHECK_EQ(p.sections.size(), size_t(3));
        CHECK(p.sections[0].name == ".text");
        CHECK(p.sections[1].name == ".rdata");
        CHECK(p.sections[2].name == ".data");
        CHECK(!p.hasClrHeader);

        CHECK_EQ(p.imports.size(), size_t(1));
        CHECK(p.imports[0].dllName == "kernel32.dll");
        CHECK_EQ(p.imports[0].symbols.size(), size_t(3));
        bool foundWriteFile = false;
        for (const auto& s : p.imports[0].symbols)
            if (s.name == "WriteFile") foundWriteFile = true;
        CHECK(foundWriteFile);
    }

    TEST("haalbaarheidsoordeel voor een pure Win32 console-app");
    {
        pe::PeFile p = pe::PeFile::parseFile("fixtures/hello.exe");
        FeasibilityReport r = assessFeasibility(p);
        CHECK(r.verdict == Verdict::Feasible);
        CHECK(r.blockers.empty());
        CHECK(!r.isDotNet);
        CHECK_EQ(r.totalImports, size_t(3));
    }

    TEST("window.exe is een GUI-app met user32 en gdi32");
    {
        pe::PeFile p = pe::PeFile::parseFile("fixtures/window.exe");
        CHECK_EQ(p.subsystem, pe::kSubsystemWindowsGui);
        auto dlls = p.importedDllNames();
        CHECK_EQ(dlls.size(), size_t(3));
        bool user32 = false, gdi32 = false;
        for (const auto& d : dlls) {
            if (d == "user32.dll") user32 = true;
            if (d == "gdi32.dll") gdi32 = true;
        }
        CHECK(user32);
        CHECK(gdi32);

        FeasibilityReport r = assessFeasibility(p);
        CHECK(r.verdict == Verdict::Feasible || r.verdict == Verdict::FeasibleWork);
        bool guiNote = false;
        for (const auto& n : r.notes)
            if (n.find("Windows GUI") != std::string::npos) guiNote = true;
        CHECK(guiNote);
    }

    TEST("een niet-PE-bestand geeft een duidelijke fout");
    {
        bool threw = false;
        try {
            std::vector<uint8_t> junk(256, 0x41);
            pe::PeFile::parse(junk, "junk");
        } catch (const EmuError& e) {
            threw = true;
            CHECK(std::string(e.what()).find("MZ") != std::string::npos);
        }
        CHECK(threw);
    }

    TEST("DLL-classificatie");
    {
        std::string note;
        CHECK(classifyDll("kernel32.dll", note) == DepClass::Implemented);
        CHECK(classifyDll("d3d11.dll", note) == DepClass::Blocker);
        CHECK(classifyDll("mscoree.dll", note) == DepClass::Blocker);
        CHECK(classifyDll("combase.dll", note) == DepClass::Blocker);
        CHECK(classifyDll("comctl32.dll", note) == DepClass::BigJump);
        CHECK(classifyDll("api-ms-win-core-winrt-l1-1-0.dll", note) == DepClass::Blocker);
        CHECK(classifyDll("api-ms-win-crt-runtime-l1-1-0.dll", note) == DepClass::Planned);
        CHECK(classifyDll("iets-onbekends.dll", note) == DepClass::Unknown);
    }

    return testing::summary("test_pe");
}
