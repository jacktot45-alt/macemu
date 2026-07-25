// macemu - overige DLL's: msvcrt/ucrtbase (C-runtime), ntdll, advapi32, shlwapi.
//
// De CRT is technisch gezien geen OS-API, maar veel oudere .exe's linken er
// dynamisch tegenaan. Een handvol functies scheelt enorm veel geëmuleerde
// instructies (en dus debugtijd).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "macemu/emulator.h"
#include "macemu/win32.h"

namespace macemu {

namespace {

// Mini-printf. Ondersteunt %d %i %u %x %X %c %s %p %f %% met eenvoudige
// breedte/precisie. Genoeg voor console-output van testprogramma's.
std::string formatPrintf(Cpu& cpu, uint64_t fmtAddr, int firstArg, bool wideFmt = false) {
    std::string fmt = wideFmt ? cpu.mem.readWideString(fmtAddr) : cpu.mem.readCString(fmtAddr);
    std::string out;
    int argIdx = firstArg;

    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        if (i + 1 >= fmt.size()) break;
        std::string spec = "%";
        ++i;
        while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) spec += fmt[i++];
        while (i < fmt.size() && (std::isdigit(static_cast<unsigned char>(fmt[i])) || fmt[i] == '*'))
            spec += fmt[i++];
        if (i < fmt.size() && fmt[i] == '.') {
            spec += fmt[i++];
            while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i])))
                spec += fmt[i++];
        }
        // lengte-modifiers overslaan (l, ll, h, z, I64)
        while (i < fmt.size() && std::strchr("lhzjtI64", fmt[i]) && fmt[i] != 'd' && fmt[i] != 'i')
            ++i;
        if (i >= fmt.size()) break;
        char conv = fmt[i];

        char buf[256];
        switch (conv) {
            case '%': out += '%'; break;
            case 'd': case 'i': {
                spec += "lld";
                std::snprintf(buf, sizeof(buf), spec.c_str(),
                              static_cast<long long>(apiArg(cpu, argIdx++)));
                out += buf;
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                spec += "ll";
                spec += conv;
                std::snprintf(buf, sizeof(buf), spec.c_str(),
                              static_cast<unsigned long long>(apiArg(cpu, argIdx++)));
                out += buf;
                break;
            }
            case 'p': {
                std::snprintf(buf, sizeof(buf), "%016llX",
                              static_cast<unsigned long long>(apiArg(cpu, argIdx++)));
                out += buf;
                break;
            }
            case 'c': out += static_cast<char>(apiArg(cpu, argIdx++)); break;
            case 's': {
                uint64_t p = apiArg(cpu, argIdx++);
                out += p ? cpu.mem.readCString(p) : "(null)";
                break;
            }
            case 'S': {
                uint64_t p = apiArg(cpu, argIdx++);
                out += p ? cpu.mem.readWideString(p) : "(null)";
                break;
            }
            case 'f': case 'g': case 'e': case 'G': case 'E': {
                spec += conv;
                double d = apiArgDouble(cpu, argIdx++);
                std::snprintf(buf, sizeof(buf), spec.c_str(), d);
                out += buf;
                break;
            }
            default:
                out += spec;
                out += conv;
                break;
        }
    }
    return out;
}

} // namespace

void Win32::registerMisc() {
    // ----------------------------------------------------------------- CRT
    auto crt = [this](const char* name, ApiHandler fn) {
        registerApi("msvcrt.dll", name, fn);
        registerApi("ucrtbase.dll", name, fn);
        registerApi("vcruntime140.dll", name, fn);
    };

    crt("malloc", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        return emu.win32().processHeap().allocate(apiArg(cpu, 0), false);
    });
    crt("calloc", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        return emu.win32().processHeap().allocate(apiArg(cpu, 0) * apiArg(cpu, 1), true);
    });
    crt("realloc", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        return emu.win32().processHeap().reallocate(apiArg(cpu, 0), apiArg(cpu, 1), false);
    });
    crt("free", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        emu.win32().processHeap().free(apiArg(cpu, 0));
        return 0;
    });
    crt("_msize", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        return emu.win32().processHeap().sizeOf(apiArg(cpu, 0));
    });

    // rand()/srand() draaien gewoon op de host-C-library. Dat levert geen
    // bit-exacte Windows-uitkomst op, maar voor "willekeurige mijnen plaatsen"
    // maakt dat niets uit.
    crt("srand", [](Emulator&, Cpu& cpu) -> uint64_t {
        std::srand(static_cast<unsigned>(apiArg(cpu, 0)));
        return 0;
    });
    crt("rand", [](Emulator&, Cpu&) -> uint64_t {
        return static_cast<uint64_t>(std::rand());
    });
    crt("time", [](Emulator&, Cpu& cpu) -> uint64_t {
        std::time_t t = std::time(nullptr);
        uint64_t p = apiArg(cpu, 0);
        if (p) cpu.mem.write64(p, static_cast<uint64_t>(t));
        return static_cast<uint64_t>(t);
    });

    crt("memset", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        cpu.mem.fill(p, static_cast<uint8_t>(apiArg(cpu, 1)),
                     static_cast<size_t>(apiArg(cpu, 2)));
        return p;
    });
    crt("memcpy", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t d = apiArg(cpu, 0), s = apiArg(cpu, 1);
        size_t n = static_cast<size_t>(apiArg(cpu, 2));
        std::vector<uint8_t> tmp(n);
        if (n) {
            cpu.mem.readBlock(s, tmp.data(), n);
            cpu.mem.writeBlock(d, tmp.data(), n);
        }
        return d;
    });
    crt("memmove", registry_["msvcrt.dll!memcpy"]);
    crt("memcmp", [](Emulator&, Cpu& cpu) -> uint64_t {
        size_t n = static_cast<size_t>(apiArg(cpu, 2));
        std::vector<uint8_t> a(n), b(n);
        if (n) {
            cpu.mem.readBlock(apiArg(cpu, 0), a.data(), n);
            cpu.mem.readBlock(apiArg(cpu, 1), b.data(), n);
        }
        int r = n ? std::memcmp(a.data(), b.data(), n) : 0;
        return static_cast<uint64_t>(static_cast<int64_t>(r));
    });
    crt("strlen", [](Emulator&, Cpu& cpu) -> uint64_t {
        return cpu.mem.readCString(apiArg(cpu, 0)).size();
    });
    crt("strcmp", [](Emulator&, Cpu& cpu) -> uint64_t {
        std::string a = cpu.mem.readCString(apiArg(cpu, 0));
        std::string b = cpu.mem.readCString(apiArg(cpu, 1));
        int r = a.compare(b);
        return static_cast<uint64_t>(static_cast<int64_t>(r < 0 ? -1 : (r > 0 ? 1 : 0)));
    });
    crt("strcpy", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t d = apiArg(cpu, 0);
        cpu.mem.writeCString(d, cpu.mem.readCString(apiArg(cpu, 1)));
        return d;
    });
    crt("strcat", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t d = apiArg(cpu, 0);
        std::string a = cpu.mem.readCString(d);
        cpu.mem.writeCString(d + a.size(), cpu.mem.readCString(apiArg(cpu, 1)));
        return d;
    });

    crt("printf", [](Emulator&, Cpu& cpu) -> uint64_t {
        std::string s = formatPrintf(cpu, apiArg(cpu, 0), 1);
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
        return s.size();
    });
    crt("wprintf", [](Emulator&, Cpu& cpu) -> uint64_t {
        std::string s = formatPrintf(cpu, apiArg(cpu, 0), 1, true);
        std::fwrite(s.data(), 1, s.size(), stdout);
        return s.size();
    });
    crt("puts", [](Emulator&, Cpu& cpu) -> uint64_t {
        std::string s = cpu.mem.readCString(apiArg(cpu, 0));
        std::printf("%s\n", s.c_str());
        std::fflush(stdout);
        return s.size() + 1;
    });
    crt("putchar", [](Emulator&, Cpu& cpu) -> uint64_t {
        std::putchar(static_cast<int>(apiArg(cpu, 0)));
        return apiArg(cpu, 0);
    });
    crt("sprintf", [](Emulator&, Cpu& cpu) -> uint64_t {
        std::string s = formatPrintf(cpu, apiArg(cpu, 1), 2);
        cpu.mem.writeCString(apiArg(cpu, 0), s);
        return s.size();
    });
    crt("_snprintf", [](Emulator&, Cpu& cpu) -> uint64_t {
        std::string s = formatPrintf(cpu, apiArg(cpu, 2), 3);
        uint64_t cap = apiArg(cpu, 1);
        if (s.size() >= cap && cap) s.resize(cap - 1);
        cpu.mem.writeCString(apiArg(cpu, 0), s);
        return s.size();
    });
    crt("snprintf", registry_["msvcrt.dll!_snprintf"]);

    // __acrt_iob_func hoort bij UCRT's interne stdio-plumbing en wordt door de
    // CRT-opstartcode aangeroepen, ook als de gast zelf geen printf/fprintf
    // gebruikt. We geven drie neplege FILE-structs terug (index 0/1/2 =
    // stdin/stdout/stderr) - genoeg om de CRT tevreden te stellen zonder de
    // hele gebufferde stdio-laag te implementeren.
    registerApi("ucrtbase.dll", "__acrt_iob_func", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        static std::unordered_map<Emulator*, uint64_t> bases;
        uint64_t& base = bases[&emu];
        if (!base) {
            base = emu.allocScratch(3 * 64, 16);
            emu.memory().fill(base, 0, 3 * 64);
        }
        uint32_t idx = static_cast<uint32_t>(apiArg(cpu, 0));
        if (idx > 2) idx = 2;
        return base + idx * 64;
    });
    // setvbuf(stream, buffer, mode, size) - buffering stellen we niet echt in
    // (elke "write" gaat toch direct naar de host), dus 0 (succes) is genoeg.
    crt("setvbuf", [](Emulator&, Cpu&) -> uint64_t { return 0; });

    crt("exit", [](Emulator&, Cpu& cpu) -> uint64_t {
        cpu.exitCode = apiArg(cpu, 0) & 0xFFFFFFFF;
        cpu.halted = true;
        return 0;
    });
    crt("_exit", registry_["msvcrt.dll!exit"]);
    crt("abort", [](Emulator&, Cpu& cpu) -> uint64_t {
        cpu.exitCode = 3;
        cpu.halted = true;
        return 0;
    });
    crt("atexit", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    crt("_initterm", [](Emulator&, Cpu& cpu) -> uint64_t {
        // Array van functiepointers aflopen en aanroepen (CRT-initialisatie).
        uint64_t begin = apiArg(cpu, 0), end = apiArg(cpu, 1);
        for (uint64_t p = begin; p < end; p += 8) {
            uint64_t fn = cpu.mem.read64(p);
            if (fn) cpu.callGuest(fn, {});
        }
        return 0;
    });
    crt("_initterm_e", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t begin = apiArg(cpu, 0), end = apiArg(cpu, 1);
        for (uint64_t p = begin; p < end; p += 8) {
            uint64_t fn = cpu.mem.read64(p);
            if (fn && cpu.callGuest(fn, {}) != 0) return 1;
        }
        return 0;
    });
    crt("_controlfp", [](Emulator&, Cpu&) -> uint64_t { return 0x9001F; });
    crt("_controlfp_s", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    crt("_set_app_type", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    crt("__set_app_type", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    crt("_configure_narrow_argv", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    crt("_initialize_narrow_environment", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    crt("_get_initial_narrow_environment",
        [](Emulator& emu, Cpu&) -> uint64_t { return emu.win32().environmentA(); });
    crt("__p___argc", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t p = emu.allocScratch(8, 8);
        cpu.mem.write32(p, 1);
        return p;
    });
    crt("_cexit", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    crt("__C_specific_handler", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    crt("_seh_filter_exe", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    crt("terminate", [](Emulator&, Cpu& cpu) -> uint64_t {
        cpu.exitCode = 3;
        cpu.halted = true;
        return 0;
    });

    // --------------------------------------------------------------- ntdll
    auto nt = [this](const char* name, ApiHandler fn) {
        registerApi("ntdll.dll", name, std::move(fn));
    };
    // x64 SEH-unwinding: zolang de gast geen exception gooit hoeft dit niets te
    // doen. Zodra hij dat wel doet is dit een heel deelproject.
    nt("RtlCaptureContext", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t ctx = apiArg(cpu, 0);
        if (ctx) cpu.mem.fill(ctx, 0, 0x4D0);
        return 0;
    });
    nt("RtlLookupFunctionEntry", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    nt("RtlVirtualUnwind", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    nt("RtlUnwind", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    nt("RtlUnwindEx", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    nt("RtlAddFunctionTable", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    nt("RtlPcToFileHeader", [](Emulator& emu, Cpu&) -> uint64_t { return emu.image().base; });
    nt("NtQueryPerformanceCounter", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    nt("RtlAllocateHeap", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        return emu.win32().processHeap().allocate(apiArg(cpu, 2), (apiArg(cpu, 1) & 8) != 0);
    });
    nt("RtlFreeHeap", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        return emu.win32().processHeap().free(apiArg(cpu, 2)) ? 1 : 0;
    });

    // ------------------------------------------------------------ advapi32
    auto adv = [this](const char* name, ApiHandler fn) {
        registerApi("advapi32.dll", name, std::move(fn));
    };
    // Er is geen registry. Alles meldt "niet gevonden"; de meeste apps kunnen
    // daarmee overweg en vallen terug op standaardwaarden.
    adv("RegOpenKeyExA", [](Emulator&, Cpu&) -> uint64_t { return 2; });
    adv("RegOpenKeyExW", [](Emulator&, Cpu&) -> uint64_t { return 2; });
    adv("RegCreateKeyExA", [](Emulator&, Cpu&) -> uint64_t { return 5; });
    adv("RegCreateKeyExW", [](Emulator&, Cpu&) -> uint64_t { return 5; });
    adv("RegQueryValueExA", [](Emulator&, Cpu&) -> uint64_t { return 2; });
    adv("RegQueryValueExW", [](Emulator&, Cpu&) -> uint64_t { return 2; });
    adv("RegSetValueExA", [](Emulator&, Cpu&) -> uint64_t { return 5; });
    adv("RegSetValueExW", [](Emulator&, Cpu&) -> uint64_t { return 5; });
    adv("RegCloseKey", [](Emulator&, Cpu&) -> uint64_t { return 0; });

    // ------------------------------------------------------------- shlwapi
    registerApi("shlwapi.dll", "PathFindFileNameA", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        std::string s = cpu.mem.readCString(p);
        size_t pos = s.find_last_of("\\/");
        return pos == std::string::npos ? p : p + pos + 1;
    });
}

} // namespace macemu
