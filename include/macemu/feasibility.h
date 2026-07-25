// macemu - feasibility.h
//
// Fase 0: haalbaarheidscheck. Kijkt naar een geparste PE en zegt eerlijk of
// het realistisch is om dit binaire bestand met een handgeschreven Win32-laag
// te draaien - of dat de dependency-boom te groot is.
#pragma once

#include <string>
#include <vector>

#include "macemu/pe.h"

namespace macemu {

enum class Verdict {
    Feasible,      // haalbaar: kleine, bekende dependency-set
    FeasibleWork,  // haalbaar, maar er moet nog flink wat API-werk gebeuren
    BigJump,       // kan, maar is een project op zich (subsystemen ontbreken)
    Blocked,       // niet haalbaar met deze aanpak: andere runtime nodig
};

enum class DepClass {
    Implemented, // macemu heeft hier (deels) stubs voor
    Planned,     // realistisch bij te bouwen met handwerk
    BigJump,     // groot subsysteem, apart project
    Blocker,     // vereist een complete externe runtime (.NET, WinRT, D3D)
    Unknown,
};

struct DepInfo {
    std::string dll;
    DepClass cls;
    std::string note;
    size_t symbolCount;
};

struct FeasibilityReport {
    Verdict verdict = Verdict::Feasible;
    std::vector<DepInfo> deps;
    std::vector<std::string> blockers;   // harde blokkades
    std::vector<std::string> warnings;   // let-op-punten
    std::vector<std::string> notes;      // informatief
    std::vector<std::string> nextSteps;  // concreet advies
    size_t totalImports = 0;
    bool isDotNet = false;
    bool isWinRT = false;
    bool needsDirectX = false;

    std::string verdictText() const;
};

FeasibilityReport assessFeasibility(const pe::PeFile& pe);

// Print een leesbaar Fase 0-rapport (Nederlands) naar stdout.
void printFeasibilityReport(const pe::PeFile& pe, const FeasibilityReport& rep, bool verbose);

// Classificeert één DLL-naam (lowercase, met .dll).
DepClass classifyDll(const std::string& lowerName, std::string& noteOut);

} // namespace macemu
