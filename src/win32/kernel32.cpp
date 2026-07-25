// macemu - kernel32-stubs (Fase 3)
//
// Elke functie hier is een *herimplementatie*, geen emulatie van de echte DLL.
// Dat is de hele truc van HLE: we hoeven geen Windows-code te draaien, alleen
// hetzelfde waarneembare gedrag te leveren.
//
// Belangrijkste bewuste beperkingen (zie docs/fase3-win32.md):
//   * single-threaded: CreateThread draait de threadfunctie meteen uit
//   * geen echte security/ACL's, geen registry-permissies
//   * bestands-I/O gaat rechtstreeks naar de host, zonder pad-vertaling
#include <chrono>
#include <cstring>
#include <thread>

#include "macemu/emulator.h"
#include "macemu/win32.h"

namespace macemu {

namespace {

uint64_t nowFileTime() {
    // FILETIME: aantal 100ns-intervallen sinds 1 januari 1601.
    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    uint64_t us = static_cast<uint64_t>(duration_cast<microseconds>(now).count());
    return us * 10ull + 116444736000000000ull;
}

uint64_t monotonicMs() {
    using namespace std::chrono;
    static auto start = steady_clock::now();
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now() - start).count());
}

std::string readStringArg(Cpu& cpu, uint64_t addr, bool wide) {
    if (addr == 0) return {};
    return wide ? cpu.mem.readWideString(addr) : cpu.mem.readCString(addr);
}

} // namespace

void Win32::registerKernel32() {
    auto reg = [this](const char* name, ApiHandler fn) {
        registerApi("kernel32.dll", name, std::move(fn));
    };

    // ---------------------------------------------------------------- fouten
    reg("GetLastError", [](Emulator& emu, Cpu&) -> uint64_t { return emu.win32().lastError(); });
    reg("SetLastError", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        emu.win32().setLastError(static_cast<uint32_t>(apiArg(cpu, 0)));
        return 0;
    });

    // --------------------------------------------------------------- modules
    auto getModuleHandle = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            uint64_t p = apiArg(cpu, 0);
            if (p == 0) return emu.image().base;
            return emu.win32().moduleHandleFor(readStringArg(cpu, p, wide));
        };
    };
    reg("GetModuleHandleA", getModuleHandle(false));
    reg("GetModuleHandleW", getModuleHandle(true));

    auto getModuleHandleEx = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            uint64_t name = apiArg(cpu, 1);
            uint64_t out = apiArg(cpu, 2);
            uint64_t h = name ? emu.win32().moduleHandleFor(readStringArg(cpu, name, wide))
                              : emu.image().base;
            if (out) cpu.mem.write64(out, h);
            return 1;
        };
    };
    reg("GetModuleHandleExA", getModuleHandleEx(false));
    reg("GetModuleHandleExW", getModuleHandleEx(true));

    auto getModuleFileName = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            uint64_t h = apiArg(cpu, 0);
            uint64_t buf = apiArg(cpu, 1);
            uint32_t cap = static_cast<uint32_t>(apiArg(cpu, 2));
            std::string name = emu.win32().moduleNameFor(h);
            std::string path = "C:\\macemu\\" + (name.empty() ? emu.image().name : name);
            if (!buf || cap == 0) return 0;
            if (wide) {
                auto w = utf8ToUtf16(path);
                uint32_t n = std::min<uint32_t>(cap - 1, static_cast<uint32_t>(w.size()));
                for (uint32_t i = 0; i < n; ++i) cpu.mem.write16(buf + i * 2, w[i]);
                cpu.mem.write16(buf + n * 2, 0);
                return n;
            }
            uint32_t n = std::min<uint32_t>(cap - 1, static_cast<uint32_t>(path.size()));
            cpu.mem.writeBlock(buf, path.data(), n);
            cpu.mem.write8(buf + n, 0);
            return n;
        };
    };
    reg("GetModuleFileNameA", getModuleFileName(false));
    reg("GetModuleFileNameW", getModuleFileName(true));

    auto loadLibrary = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            std::string name = readStringArg(cpu, apiArg(cpu, 0), wide);
            MACEMU_LOG_DEBUG("LoadLibrary(\"%s\") -> nep-module", name.c_str());
            return emu.win32().moduleHandleFor(name);
        };
    };
    reg("LoadLibraryA", loadLibrary(false));
    reg("LoadLibraryW", loadLibrary(true));
    reg("LoadLibraryExA", loadLibrary(false));
    reg("LoadLibraryExW", loadLibrary(true));
    reg("FreeLibrary", [](Emulator&, Cpu&) -> uint64_t { return 1; });

    reg("GetProcAddress", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t hmod = apiArg(cpu, 0);
        uint64_t namePtr = apiArg(cpu, 1);
        std::string dll = emu.win32().moduleNameFor(hmod);
        if (dll.empty()) dll = "kernel32.dll";
        // Een naam-pointer met alleen een laag getal is een ordinal.
        if (namePtr < 0x10000) {
            MACEMU_LOG_WARN("GetProcAddress via ordinal #%llu in %s wordt niet ondersteund",
                            (unsigned long long)namePtr, dll.c_str());
            emu.win32().setLastError(ERROR_PROC_NOT_FOUND_);
            return 0;
        }
        std::string fn = cpu.mem.readCString(namePtr);
        if (!emu.win32().hasApi(dll, fn)) {
            MACEMU_LOG_WARN("GetProcAddress(%s, \"%s\"): niet geïmplementeerd", dll.c_str(),
                            fn.c_str());
            emu.win32().setLastError(ERROR_PROC_NOT_FOUND_);
            return 0;
        }
        return emu.win32().resolveImport(dll, fn, 0, false);
    });

    // ------------------------------------------------------------------ heap
    reg("GetProcessHeap",
        [](Emulator& emu, Cpu&) -> uint64_t { return emu.win32().processHeapHandle(); });

    reg("HeapAlloc", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t h = apiArg(cpu, 0);
        uint32_t flags = static_cast<uint32_t>(apiArg(cpu, 1));
        uint64_t size = apiArg(cpu, 2);
        auto ho = emu.win32().lookup<HeapObject>(h);
        GuestHeap& heap = ho ? *ho->heap : emu.win32().processHeap();
        return heap.allocate(size, (flags & 0x8) != 0); // HEAP_ZERO_MEMORY
    });
    reg("HeapFree", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t h = apiArg(cpu, 0);
        uint64_t p = apiArg(cpu, 2);
        if (!p) return 1;
        auto ho = emu.win32().lookup<HeapObject>(h);
        GuestHeap& heap = ho ? *ho->heap : emu.win32().processHeap();
        return heap.free(p) ? 1 : 0;
    });
    reg("HeapReAlloc", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t h = apiArg(cpu, 0);
        uint32_t flags = static_cast<uint32_t>(apiArg(cpu, 1));
        uint64_t p = apiArg(cpu, 2);
        uint64_t size = apiArg(cpu, 3);
        auto ho = emu.win32().lookup<HeapObject>(h);
        GuestHeap& heap = ho ? *ho->heap : emu.win32().processHeap();
        return heap.reallocate(p, size, (flags & 0x8) != 0);
    });
    reg("HeapSize", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        auto ho = emu.win32().lookup<HeapObject>(apiArg(cpu, 0));
        GuestHeap& heap = ho ? *ho->heap : emu.win32().processHeap();
        return heap.sizeOf(apiArg(cpu, 2));
    });
    reg("HeapCreate", [](Emulator& emu, Cpu&) -> uint64_t {
        return emu.win32().processHeapHandle(); // één heap is genoeg
    });
    reg("HeapDestroy", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("HeapValidate", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("HeapSetInformation", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("HeapCompact", [](Emulator&, Cpu&) -> uint64_t { return 0; });

    auto localAlloc = [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint32_t flags = static_cast<uint32_t>(apiArg(cpu, 0));
        return emu.win32().processHeap().allocate(apiArg(cpu, 1), (flags & 0x40) != 0);
    };
    reg("LocalAlloc", localAlloc);
    reg("GlobalAlloc", localAlloc);
    auto localFree = [](Emulator& emu, Cpu& cpu) -> uint64_t {
        emu.win32().processHeap().free(apiArg(cpu, 0));
        return 0;
    };
    reg("LocalFree", localFree);
    reg("GlobalFree", localFree);
    reg("GlobalLock", [](Emulator&, Cpu& cpu) -> uint64_t { return apiArg(cpu, 0); });
    reg("GlobalUnlock", [](Emulator&, Cpu&) -> uint64_t { return 1; });

    // -------------------------------------------------------- virtueel geheugen
    reg("VirtualAlloc", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t addr = apiArg(cpu, 0);
        uint64_t size = apiArg(cpu, 1);
        uint32_t type = static_cast<uint32_t>(apiArg(cpu, 2));
        uint32_t protect = static_cast<uint32_t>(apiArg(cpu, 3));
        if (size == 0) return 0;
        size = (size + 0xFFF) & ~0xFFFull;

        uint32_t prot = PROT_R;
        switch (protect & 0xFF) {
            case 0x01: prot = PROT_NONE_; break;        // PAGE_NOACCESS
            case 0x02: prot = PROT_R; break;            // PAGE_READONLY
            case 0x04: prot = PROT_RW; break;           // PAGE_READWRITE
            case 0x10: prot = PROT_RX; break;           // PAGE_EXECUTE
            case 0x20: prot = PROT_RX; break;           // PAGE_EXECUTE_READ
            case 0x40: prot = PROT_RWX; break;          // PAGE_EXECUTE_READWRITE
            default: prot = PROT_RW; break;
        }
        if (addr == 0) addr = emu.memory().findFreeRegion(size, 0x10000, 0x0000000300000000ull);
        else addr &= ~0xFFFull;

        bool commit = (type & 0x1000) != 0; // MEM_COMMIT
        emu.memory().map(addr, size, prot, "VirtualAlloc", !commit);
        MACEMU_LOG_DEBUG("VirtualAlloc -> 0x%llx (%llu bytes)", (unsigned long long)addr,
                         (unsigned long long)size);
        return addr;
    });
    reg("VirtualFree", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t addr = apiArg(cpu, 0);
        uint64_t size = apiArg(cpu, 1);
        uint32_t type = static_cast<uint32_t>(apiArg(cpu, 2));
        if (type & 0x8000) { // MEM_RELEASE: hele regio
            auto it = emu.memory().regions().find(addr & ~0xFFFull);
            if (it != emu.memory().regions().end()) size = it->second.size;
        }
        if (size) emu.memory().unmap(addr, size);
        return 1;
    });
    reg("VirtualProtect", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t addr = apiArg(cpu, 0);
        uint64_t size = apiArg(cpu, 1);
        uint32_t protect = static_cast<uint32_t>(apiArg(cpu, 2));
        uint64_t oldOut = apiArg(cpu, 3);
        uint32_t prot = PROT_RW;
        switch (protect & 0xFF) {
            case 0x01: prot = PROT_NONE_; break;
            case 0x02: prot = PROT_R; break;
            case 0x04: prot = PROT_RW; break;
            case 0x10: case 0x20: prot = PROT_RX; break;
            case 0x40: prot = PROT_RWX; break;
        }
        emu.memory().protect(addr, size, prot);
        if (oldOut) cpu.mem.write32(oldOut, 0x04);
        return 1;
    });
    reg("VirtualQuery", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t addr = apiArg(cpu, 0);
        uint64_t info = apiArg(cpu, 1);
        if (!info) return 0;
        bool mapped = emu.memory().isMapped(addr);
        cpu.mem.write64(info + 0x00, addr & ~0xFFFull);   // BaseAddress
        cpu.mem.write64(info + 0x08, addr & ~0xFFFull);   // AllocationBase
        cpu.mem.write32(info + 0x10, 0x04);               // AllocationProtect
        cpu.mem.write64(info + 0x18, 0x1000);             // RegionSize
        cpu.mem.write32(info + 0x20, mapped ? 0x1000 : 0x10000); // State
        cpu.mem.write32(info + 0x24, 0x04);               // Protect
        cpu.mem.write32(info + 0x28, 0x20000);            // Type = MEM_PRIVATE
        return 0x30;
    });

    // ------------------------------------------------------------------- I/O
    reg("GetStdHandle", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        int32_t which = static_cast<int32_t>(apiArg(cpu, 0));
        return emu.win32().stdHandle(which);
    });
    reg("SetStdHandle", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("GetFileType", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        auto f = emu.win32().lookup<FileObject>(apiArg(cpu, 0));
        if (!f) return 0;
        return f->isConsole ? 2 : 1; // FILE_TYPE_CHAR / FILE_TYPE_DISK
    });

    reg("WriteFile", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t h = apiArg(cpu, 0);
        uint64_t buf = apiArg(cpu, 1);
        uint32_t len = static_cast<uint32_t>(apiArg(cpu, 2));
        uint64_t writtenPtr = apiArg(cpu, 3);
        auto f = emu.win32().lookup<FileObject>(h);
        if (!f || !f->fp) {
            emu.win32().setLastError(ERROR_INVALID_HANDLE_);
            return 0;
        }
        std::vector<uint8_t> data(len);
        if (len) cpu.mem.readBlock(buf, data.data(), len);
        size_t n = std::fwrite(data.data(), 1, len, f->fp);
        std::fflush(f->fp);
        if (writtenPtr) cpu.mem.write32(writtenPtr, static_cast<uint32_t>(n));
        return 1;
    });

    auto writeConsole = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            uint64_t h = apiArg(cpu, 0);
            uint64_t buf = apiArg(cpu, 1);
            uint32_t count = static_cast<uint32_t>(apiArg(cpu, 2));
            uint64_t writtenPtr = apiArg(cpu, 3);
            auto f = emu.win32().lookup<FileObject>(h);
            FILE* out = (f && f->fp) ? f->fp : stdout;
            std::string text;
            if (wide) {
                std::vector<uint16_t> w(count);
                for (uint32_t i = 0; i < count; ++i) w[i] = cpu.mem.read16(buf + i * 2);
                text = utf16ToUtf8(w);
            } else {
                std::vector<uint8_t> b(count);
                if (count) cpu.mem.readBlock(buf, b.data(), count);
                text.assign(b.begin(), b.end());
            }
            std::fwrite(text.data(), 1, text.size(), out);
            std::fflush(out);
            if (writtenPtr) cpu.mem.write32(writtenPtr, count);
            return 1;
        };
    };
    reg("WriteConsoleA", writeConsole(false));
    reg("WriteConsoleW", writeConsole(true));

    reg("ReadFile", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t h = apiArg(cpu, 0);
        uint64_t buf = apiArg(cpu, 1);
        uint32_t len = static_cast<uint32_t>(apiArg(cpu, 2));
        uint64_t readPtr = apiArg(cpu, 3);
        auto f = emu.win32().lookup<FileObject>(h);
        if (!f || !f->fp) return 0;
        std::vector<uint8_t> data(len);
        size_t n = std::fread(data.data(), 1, len, f->fp);
        if (n) cpu.mem.writeBlock(buf, data.data(), n);
        if (readPtr) cpu.mem.write32(readPtr, static_cast<uint32_t>(n));
        return 1;
    });

    auto createFile = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            std::string name = readStringArg(cpu, apiArg(cpu, 0), wide);
            uint32_t access = static_cast<uint32_t>(apiArg(cpu, 1));
            uint32_t disposition = static_cast<uint32_t>(apiArg(cpu, 4));
            // Windows-paden worden ruw naar het huidige werkpad vertaald.
            std::string hostPath = name;
            for (auto& c : hostPath)
                if (c == '\\') c = '/';
            size_t colon = hostPath.find(':');
            if (colon != std::string::npos && colon <= 1) hostPath = "." + hostPath.substr(colon + 1);

            const char* mode = "rb";
            if (access & 0x40000000u) mode = (disposition == 2 || disposition == 1) ? "wb" : "r+b";
            FILE* fp = std::fopen(hostPath.c_str(), mode);
            if (!fp) {
                MACEMU_LOG_WARN("CreateFile(\"%s\") mislukt (host-pad \"%s\")", name.c_str(),
                                hostPath.c_str());
                emu.win32().setLastError(ERROR_FILE_NOT_FOUND_);
                return static_cast<uint64_t>(-1); // INVALID_HANDLE_VALUE
            }
            auto f = std::make_shared<FileObject>();
            f->fp = fp;
            f->ownsFp = true;
            f->path = name;
            return emu.win32().createHandle(f);
        };
    };
    reg("CreateFileA", createFile(false));
    reg("CreateFileW", createFile(true));

    reg("CloseHandle", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        return emu.win32().closeHandle(apiArg(cpu, 0)) ? 1 : 0;
    });
    reg("FlushFileBuffers", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        auto f = emu.win32().lookup<FileObject>(apiArg(cpu, 0));
        if (f && f->fp) std::fflush(f->fp);
        return 1;
    });

    // --------------------------------------------------------------- proces
    reg("ExitProcess", [](Emulator&, Cpu& cpu) -> uint64_t {
        cpu.exitCode = apiArg(cpu, 0) & 0xFFFFFFFF;
        cpu.halted = true;
        MACEMU_LOG_INFO("ExitProcess(%llu)", (unsigned long long)cpu.exitCode);
        return 0;
    });
    reg("TerminateProcess", [](Emulator&, Cpu& cpu) -> uint64_t {
        cpu.exitCode = apiArg(cpu, 1) & 0xFFFFFFFF;
        cpu.halted = true;
        return 1;
    });
    reg("GetCurrentProcess", [](Emulator&, Cpu&) -> uint64_t { return static_cast<uint64_t>(-1); });
    reg("GetCurrentProcessId", [](Emulator&, Cpu&) -> uint64_t { return 0x1234; });
    reg("GetCurrentThread", [](Emulator&, Cpu&) -> uint64_t { return static_cast<uint64_t>(-2); });
    reg("GetCurrentThreadId", [](Emulator&, Cpu&) -> uint64_t { return 0x5678; });

    reg("GetCommandLineA",
        [](Emulator& emu, Cpu&) -> uint64_t { return emu.win32().commandLineA(); });
    reg("GetCommandLineW",
        [](Emulator& emu, Cpu&) -> uint64_t { return emu.win32().commandLineW(); });

    reg("GetEnvironmentStrings",
        [](Emulator& emu, Cpu&) -> uint64_t { return emu.win32().environmentA(); });
    reg("GetEnvironmentStringsW",
        [](Emulator& emu, Cpu&) -> uint64_t { return emu.win32().environmentW(); });
    reg("FreeEnvironmentStringsA", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("FreeEnvironmentStringsW", [](Emulator&, Cpu&) -> uint64_t { return 1; });

    auto getEnvVar = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            (void)emu;
            uint64_t buf = apiArg(cpu, 1);
            uint32_t cap = static_cast<uint32_t>(apiArg(cpu, 2));
            (void)buf;
            (void)cap;
            (void)wide;
            emu.win32().setLastError(203); // ERROR_ENVVAR_NOT_FOUND
            return 0;
        };
    };
    reg("GetEnvironmentVariableA", getEnvVar(false));
    reg("GetEnvironmentVariableW", getEnvVar(true));

    auto getStartupInfo = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            uint64_t si = apiArg(cpu, 0);
            if (!si) return 0;
            cpu.mem.fill(si, 0, 104);
            cpu.mem.write32(si, 104); // cb
            cpu.mem.write64(si + 0x50, emu.win32().stdHandle(-10));
            cpu.mem.write64(si + 0x58, emu.win32().stdHandle(-11));
            cpu.mem.write64(si + 0x60, emu.win32().stdHandle(-12));
            return 0;
        };
    };
    reg("GetStartupInfoA", getStartupInfo(false));
    reg("GetStartupInfoW", getStartupInfo(true));

    // ------------------------------------------------------------------ tijd
    reg("GetSystemTimeAsFileTime", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (p) cpu.mem.write64(p, nowFileTime());
        return 0;
    });
    reg("GetSystemTimePreciseAsFileTime", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (p) cpu.mem.write64(p, nowFileTime());
        return 0;
    });
    reg("GetTickCount", [](Emulator&, Cpu&) -> uint64_t { return monotonicMs() & 0xFFFFFFFF; });
    reg("GetTickCount64", [](Emulator&, Cpu&) -> uint64_t { return monotonicMs(); });
    reg("QueryPerformanceCounter", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        using namespace std::chrono;
        uint64_t ticks = static_cast<uint64_t>(
            duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
        if (p) cpu.mem.write64(p, ticks);
        return 1;
    });
    reg("QueryPerformanceFrequency", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (p) cpu.mem.write64(p, 1000000); // microseconden
        return 1;
    });
    reg("Sleep", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint32_t ms = static_cast<uint32_t>(apiArg(cpu, 0));
        // In een venster-app moeten we tijdens Sleep wel host-events blijven
        // ophalen, anders lijkt het venster vast te lopen.
        emu.win32().pumpHostEvents();
        if (ms > 0 && ms < 2000) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return 0;
    });

    // ---------------------------------------------------- critical sections
    // Single-threaded: een critical section is hier puur boekhouding.
    reg("InitializeCriticalSection", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (p) cpu.mem.fill(p, 0, 40);
        return 0;
    });
    reg("InitializeCriticalSectionAndSpinCount", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (p) cpu.mem.fill(p, 0, 40);
        return 1;
    });
    reg("InitializeCriticalSectionEx", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (p) cpu.mem.fill(p, 0, 40);
        return 1;
    });
    reg("EnterCriticalSection", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("LeaveCriticalSection", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("TryEnterCriticalSection", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("DeleteCriticalSection", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("InitializeSListHead", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (p) cpu.mem.fill(p, 0, 16);
        return 0;
    });
    reg("AcquireSRWLockExclusive", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("ReleaseSRWLockExclusive", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("AcquireSRWLockShared", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("ReleaseSRWLockShared", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("InitOnceExecuteOnce", [](Emulator&, Cpu&) -> uint64_t { return 1; });

    // ------------------------------------------------------------------- TLS
    reg("TlsAlloc", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        static uint32_t next = 0;
        (void)emu;
        (void)cpu;
        if (next >= 64) return 0xFFFFFFFF;
        return next++;
    });
    reg("TlsFree", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("TlsGetValue", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint32_t idx = static_cast<uint32_t>(apiArg(cpu, 0));
        if (idx >= 64) return 0;
        return cpu.mem.read64(emu.win32().tebAddress() + 0x1480 + idx * 8);
    });
    reg("TlsSetValue", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint32_t idx = static_cast<uint32_t>(apiArg(cpu, 0));
        if (idx >= 64) return 0;
        cpu.mem.write64(emu.win32().tebAddress() + 0x1480 + idx * 8, apiArg(cpu, 1));
        return 1;
    });
    // Fiber-local storage: de UCRT gebruikt dit voor per-thread state.
    reg("FlsAlloc", [](Emulator&, Cpu&) -> uint64_t {
        static uint32_t next = 32;
        return next++;
    });
    reg("FlsFree", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("FlsGetValue", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint32_t idx = static_cast<uint32_t>(apiArg(cpu, 0));
        if (idx >= 64) return 0;
        return cpu.mem.read64(emu.win32().tebAddress() + 0x1480 + idx * 8);
    });
    reg("FlsSetValue", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint32_t idx = static_cast<uint32_t>(apiArg(cpu, 0));
        if (idx >= 64) return 0;
        cpu.mem.write64(emu.win32().tebAddress() + 0x1480 + idx * 8, apiArg(cpu, 1));
        return 1;
    });

    // ------------------------------------------------------------ diversen
    reg("IsProcessorFeaturePresent", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint32_t f = static_cast<uint32_t>(apiArg(cpu, 0));
        // 10 = SSE2, 12 = XMMI64. Alles wat we niet emuleren melden we als afwezig.
        return (f == 10 || f == 12 || f == 6) ? 1 : 0;
    });
    reg("IsDebuggerPresent", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("OutputDebugStringA", [](Emulator&, Cpu& cpu) -> uint64_t {
        MACEMU_LOG_INFO("[gast] %s", cpu.mem.readCString(apiArg(cpu, 0)).c_str());
        return 0;
    });
    reg("OutputDebugStringW", [](Emulator&, Cpu& cpu) -> uint64_t {
        MACEMU_LOG_INFO("[gast] %s", cpu.mem.readWideString(apiArg(cpu, 0)).c_str());
        return 0;
    });
    reg("SetUnhandledExceptionFilter", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("UnhandledExceptionFilter", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("SetErrorMode", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("RaiseException", [](Emulator&, Cpu& cpu) -> uint64_t {
        throw EmuError(strFormat("de gast riep RaiseException(0x%llx) aan - er is geen SEH "
                                 "geïmplementeerd, dus dit is fataal",
                                 (unsigned long long)apiArg(cpu, 0)));
    });
    reg("EncodePointer", [](Emulator&, Cpu& cpu) -> uint64_t { return apiArg(cpu, 0); });
    reg("DecodePointer", [](Emulator&, Cpu& cpu) -> uint64_t { return apiArg(cpu, 0); });

    reg("GetVersion", [](Emulator&, Cpu&) -> uint64_t { return 0x0A00; });
    reg("GetSystemInfo", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (!p) return 0;
        cpu.mem.fill(p, 0, 48);
        cpu.mem.write32(p + 0x00, 9);       // PROCESSOR_ARCHITECTURE_AMD64
        cpu.mem.write32(p + 0x04, 0x1000);  // dwPageSize
        cpu.mem.write64(p + 0x08, 0x10000); // lpMinimumApplicationAddress
        cpu.mem.write64(p + 0x10, 0x00007FFFFFFF0000ull);
        cpu.mem.write64(p + 0x18, 1);       // dwActiveProcessorMask
        cpu.mem.write32(p + 0x20, 1);       // dwNumberOfProcessors
        cpu.mem.write32(p + 0x2C, 0x10000); // dwAllocationGranularity
        return 0;
    });
    reg("GetNativeSystemInfo", registry_["kernel32.dll!GetSystemInfo"]);

    reg("GetACP", [](Emulator&, Cpu&) -> uint64_t { return 1252; });
    reg("GetOEMCP", [](Emulator&, Cpu&) -> uint64_t { return 437; });
    reg("GetConsoleCP", [](Emulator&, Cpu&) -> uint64_t { return 437; });
    reg("GetConsoleOutputCP", [](Emulator&, Cpu&) -> uint64_t { return 437; });
    reg("SetConsoleCtrlHandler", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("GetConsoleMode", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 1);
        if (p) cpu.mem.write32(p, 3);
        return 1;
    });
    reg("SetConsoleMode", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("GetCPInfo", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 1);
        if (p) {
            cpu.mem.fill(p, 0, 20);
            cpu.mem.write32(p, 1); // MaxCharSize
        }
        return 1;
    });

    reg("MultiByteToWideChar", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t src = apiArg(cpu, 2);
        int32_t srcLen = static_cast<int32_t>(apiArg(cpu, 3));
        uint64_t dst = apiArg(cpu, 4);
        int32_t dstCap = static_cast<int32_t>(apiArg(cpu, 5));
        std::string s;
        if (srcLen < 0) s = cpu.mem.readCString(src);
        else {
            std::vector<uint8_t> b(static_cast<size_t>(srcLen));
            if (srcLen) cpu.mem.readBlock(src, b.data(), b.size());
            s.assign(b.begin(), b.end());
        }
        std::vector<uint16_t> w = utf8ToUtf16(s);
        if (srcLen < 0) w.push_back(0);
        if (dstCap == 0 || dst == 0) return w.size();
        int32_t n = std::min<int32_t>(dstCap, static_cast<int32_t>(w.size()));
        for (int32_t i = 0; i < n; ++i) cpu.mem.write16(dst + i * 2, w[i]);
        return static_cast<uint64_t>(n);
    });
    reg("WideCharToMultiByte", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t src = apiArg(cpu, 2);
        int32_t srcLen = static_cast<int32_t>(apiArg(cpu, 3));
        uint64_t dst = apiArg(cpu, 4);
        int32_t dstCap = static_cast<int32_t>(apiArg(cpu, 5));
        std::vector<uint16_t> w;
        if (srcLen < 0) {
            for (int i = 0;; ++i) {
                uint16_t c = cpu.mem.read16(src + i * 2);
                w.push_back(c);
                if (!c) break;
            }
        } else {
            for (int32_t i = 0; i < srcLen; ++i) w.push_back(cpu.mem.read16(src + i * 2));
        }
        std::string s = utf16ToUtf8(w);
        if (dstCap == 0 || dst == 0) return s.size();
        int32_t n = std::min<int32_t>(dstCap, static_cast<int32_t>(s.size()));
        cpu.mem.writeBlock(dst, s.data(), static_cast<size_t>(n));
        return static_cast<uint64_t>(n);
    });
    reg("GetStringTypeW", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("LCMapStringW", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("GetUserDefaultLCID", [](Emulator&, Cpu&) -> uint64_t { return 0x0409; });

    reg("lstrlenA", [](Emulator&, Cpu& cpu) -> uint64_t {
        return cpu.mem.readCString(apiArg(cpu, 0)).size();
    });
    reg("lstrlenW", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        uint64_t n = 0;
        while (cpu.mem.read16(p + n * 2) != 0) ++n;
        return n;
    });

    // ---------------------------------------------------- events & wachten
    auto createEvent = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            auto e = std::make_shared<EventObject>();
            e->manualReset = apiArg(cpu, 1) != 0;
            e->signaled = apiArg(cpu, 2) != 0;
            uint64_t namePtr = apiArg(cpu, 3);
            if (namePtr) e->name = readStringArg(cpu, namePtr, wide);
            return emu.win32().createHandle(e);
        };
    };
    reg("CreateEventA", createEvent(false));
    reg("CreateEventW", createEvent(true));
    reg("SetEvent", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        auto e = emu.win32().lookup<EventObject>(apiArg(cpu, 0));
        if (e) e->signaled = true;
        return e ? 1 : 0;
    });
    reg("ResetEvent", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        auto e = emu.win32().lookup<EventObject>(apiArg(cpu, 0));
        if (e) e->signaled = false;
        return e ? 1 : 0;
    });
    reg("WaitForSingleObject", [](Emulator&, Cpu&) -> uint64_t {
        return 0; // WAIT_OBJECT_0: single-threaded, dus nooit echt wachten
    });
    reg("WaitForMultipleObjects", [](Emulator&, Cpu&) -> uint64_t { return 0; });

    reg("CreateThread", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t start = apiArg(cpu, 2);
        uint64_t param = apiArg(cpu, 3);
        uint64_t tidOut = apiArg(cpu, 6);
        MACEMU_LOG_WARN("CreateThread: macemu is single-threaded en draait de threadfunctie "
                        "meteen synchroon uit (0x%llx)", (unsigned long long)start);
        uint64_t rc = cpu.callGuest(start, {param});
        auto t = std::make_shared<ThreadObject>();
        t->tid = 0x9000;
        if (tidOut) cpu.mem.write32(tidOut, t->tid);
        MACEMU_LOG_DEBUG("thread klaar met code %llu", (unsigned long long)rc);
        return emu.win32().createHandle(t);
    });

    reg("GetExitCodeThread", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 1);
        if (p) cpu.mem.write32(p, 0);
        return 1;
    });

    reg("GetCurrentDirectoryA", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t buf = apiArg(cpu, 1);
        std::string dir = "C:\\macemu";
        if (buf) cpu.mem.writeCString(buf, dir);
        return dir.size();
    });
    reg("GetCurrentDirectoryW", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t buf = apiArg(cpu, 1);
        std::string dir = "C:\\macemu";
        if (buf) cpu.mem.writeWideString(buf, dir);
        return dir.size();
    });
}

} // namespace macemu
