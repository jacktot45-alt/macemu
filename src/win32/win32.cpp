// macemu - kern van de Win32-laag: dispatcher, heap, handles, TEB/PEB.
#include "macemu/win32.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "macemu/emulator.h"

namespace macemu {

FileObject::~FileObject() {
    if (ownsFp && fp) std::fclose(fp);
}

// ---------------------------------------------------------------------------
// GuestHeap
// ---------------------------------------------------------------------------
GuestHeap::GuestHeap(Memory& mem, uint64_t base, uint64_t maxSize, const std::string& name)
    : mem_(mem), base_(base), maxSize_(maxSize), cursor_(base), committed_(base), name_(name) {}

void GuestHeap::commitUpTo(uint64_t addr) {
    if (addr <= committed_) return;
    uint64_t need = ((addr - committed_) + 0xFFFF) & ~0xFFFFull;
    if (committed_ + need > base_ + maxSize_)
        throw EmuError(strFormat("heap '%s' is vol (max %llu bytes)", name_.c_str(),
                                 (unsigned long long)maxSize_));
    mem_.map(committed_, need, PROT_RW, "heap " + name_);
    committed_ += need;
}

uint64_t GuestHeap::allocate(uint64_t size, bool zeroMemory) {
    if (size == 0) size = 1;
    uint64_t need = (size + 15) & ~15ull;

    // Eerst kijken of er een vrij blok past (first fit).
    for (auto& kv : blocks_) {
        if (!kv.second.free || kv.second.size < need) continue;
        uint64_t addr = kv.first;
        uint64_t rest = kv.second.size - need;
        kv.second.free = false;
        if (rest >= 32) {
            kv.second.size = need;
            blocks_[addr + need] = Block{rest, true};
        }
        inUse_ += kv.second.size;
        if (zeroMemory) mem_.fill(addr, 0, static_cast<size_t>(kv.second.size));
        return addr;
    }

    uint64_t addr = cursor_;
    cursor_ += need;
    commitUpTo(cursor_);
    blocks_[addr] = Block{need, false};
    inUse_ += need;
    if (zeroMemory) mem_.fill(addr, 0, static_cast<size_t>(need));
    return addr;
}

bool GuestHeap::free(uint64_t addr) {
    auto it = blocks_.find(addr);
    if (it == blocks_.end() || it->second.free) return false;
    it->second.free = true;
    inUse_ -= it->second.size;

    // Samenvoegen met de buur erna.
    auto next = std::next(it);
    if (next != blocks_.end() && next->second.free && it->first + it->second.size == next->first) {
        it->second.size += next->second.size;
        blocks_.erase(next);
    }
    // En met de buur ervoor.
    if (it != blocks_.begin()) {
        auto prev = std::prev(it);
        if (prev->second.free && prev->first + prev->second.size == it->first) {
            prev->second.size += it->second.size;
            blocks_.erase(it);
        }
    }
    return true;
}

uint64_t GuestHeap::sizeOf(uint64_t addr) const {
    auto it = blocks_.find(addr);
    return it == blocks_.end() ? 0 : it->second.size;
}

uint64_t GuestHeap::reallocate(uint64_t addr, uint64_t newSize, bool zeroMemory) {
    if (addr == 0) return allocate(newSize, zeroMemory);
    uint64_t old = sizeOf(addr);
    if (old == 0) return 0;
    if (newSize <= old) return addr;
    uint64_t fresh = allocate(newSize, zeroMemory);
    std::vector<uint8_t> tmp(static_cast<size_t>(old));
    mem_.readBlock(addr, tmp.data(), tmp.size());
    mem_.writeBlock(fresh, tmp.data(), tmp.size());
    free(addr);
    return fresh;
}

// ---------------------------------------------------------------------------
// Hulpjes
// ---------------------------------------------------------------------------
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Veel Windows-DLL's zijn tegenwoordig alleen doorgeefluiken. We beelden ze af
// op de DLL waar we de implementatie wél hebben staan.
std::string canonicalDll(const std::string& raw) {
    std::string d = toLower(raw);
    if (d == "kernelbase.dll") return "kernel32.dll";
    if (d == "ntdll.dll") return "ntdll.dll";
    if (d.rfind("api-ms-win-crt-", 0) == 0) return "ucrtbase.dll";
    if (d.rfind("api-ms-win-core-", 0) == 0) return "kernel32.dll";
    if (d.rfind("ext-ms-win-", 0) == 0) return "kernel32.dll";
    if (d == "ucrtbased.dll") return "ucrtbase.dll";
    if (d == "msvcr120.dll" || d == "msvcr110.dll" || d == "msvcr100.dll" ||
        d == "msvcr90.dll" || d == "msvcr80.dll")
        return "msvcrt.dll";
    if (d == "vcruntime140d.dll") return "vcruntime140.dll";
    return d;
}

} // namespace

uint64_t apiArg(Cpu& cpu, int index) {
    static const int kRegs[4] = {RCX, RDX, R8, R9};
    if (index < 4) return cpu.gpr[kRegs[index]];
    // Returnadres staat nog op de stack: stack-arg n zit op [rsp + 8 + n*8].
    return cpu.mem.read64(cpu.gpr[RSP] + 8 + static_cast<uint64_t>(index) * 8);
}

double apiArgDouble(Cpu& cpu, int index) {
    double d;
    if (index < 4) {
        uint64_t bits = cpu.xmm[index].lo;
        std::memcpy(&d, &bits, 8);
    } else {
        uint64_t bits = cpu.mem.read64(cpu.gpr[RSP] + 8 + static_cast<uint64_t>(index) * 8);
        std::memcpy(&d, &bits, 8);
    }
    return d;
}

// ---------------------------------------------------------------------------
// Win32
// ---------------------------------------------------------------------------
Win32::Win32(Emulator& emu) : emu_(emu) {}
Win32::~Win32() = default;

void Win32::registerApi(const std::string& dll, const std::string& name, ApiHandler fn) {
    registry_[canonicalDll(dll) + "!" + name] = std::move(fn);
}

bool Win32::hasApi(const std::string& dll, const std::string& name) const {
    return registry_.find(canonicalDll(dll) + "!" + name) != registry_.end();
}

void Win32::initialize(uint64_t hleBase, uint64_t hleSize) {
    hleBase_ = hleBase;
    hleSize_ = hleSize;
    // Het laatste slot is gereserveerd als return-sentinel voor Cpu::callGuest.
    maxSlots_ = static_cast<uint32_t>(hleSize / Cpu::kHleSlotSize) - 1;

    // Het HLE-gebied moet uitvoerbaar lijken: de gast doet er CALL naartoe.
    // Er staan geen echte instructies in; de CPU onderschept het adres.
    emu_.memory().map(hleBase, hleSize, PROT_RX, "win32 HLE-thunks");
    emu_.cpu().setHleRegion(hleBase, hleSize, this);

    registerKernel32();
    registerUser32();
    registerGdi32();
    registerMisc();

    // Slot 0: hier "returnt" het entry point naartoe als main() gewoon stopt.
    registerApi("macemu", "__process_exit", [](Emulator&, Cpu& cpu) -> uint64_t {
        cpu.exitCode = cpu.gpr[RAX] & 0xFFFFFFFF;
        cpu.halted = true;
        return 0;
    });
    resolveImport("macemu", "__process_exit", 0, false);

    // Proces-heap.
    processHeap_.reset(new GuestHeap(emu_.memory(), Emulator::kHeapBase,
                                     emu_.options().heapSize, "process"));
    auto ho = std::make_shared<HeapObject>();
    ho->heap = processHeap_;
    processHeapHandle_ = createHandle(ho);

    // Standaard-handles voor de console.
    auto mkConsole = [&](int stream) {
        auto f = std::make_shared<FileObject>();
        f->isConsole = true;
        f->consoleStream = stream;
        f->fp = (stream == 2) ? stderr : stdout;
        return createHandle(f);
    };
    stdOutHandle_ = mkConsole(1);
    stdErrHandle_ = mkConsole(2);
    auto fin = std::make_shared<FileObject>();
    fin->isConsole = true;
    fin->consoleStream = 0;
    fin->fp = stdin;
    stdInHandle_ = createHandle(fin);
}

uint64_t Win32::resolveImport(const std::string& dll, const std::string& name, uint16_t ordinal,
                              bool byOrdinal) {
    std::string canon = canonicalDll(dll);
    std::string sym = byOrdinal ? strFormat("#%u", ordinal) : name;
    std::string key = canon + "!" + sym;

    auto it = slotByName_.find(key);
    if (it != slotByName_.end()) return hleBase_ + it->second * Cpu::kHleSlotSize;

    if (nextSlot_ >= maxSlots_)
        throw EmuError("HLE-gebied vol: te veel verschillende imports");

    uint32_t id = nextSlot_++;
    ApiEntry e;
    e.dll = canon;
    e.name = sym;
    auto impl = registry_.find(key);
    if (impl != registry_.end()) {
        e.handler = impl->second;
        e.implemented = true;
    }
    slots_.push_back(std::move(e));
    slotByName_[key] = id;
    return hleBase_ + id * Cpu::kHleSlotSize;
}

void Win32::onHostCall(Cpu& cpu, uint32_t id) {
    if (id >= slots_.size())
        throw EmuError(strFormat("aanroep naar onbekend HLE-slot %u", id));
    ApiEntry& e = slots_[id];
    ++e.callCount;

    if (!e.implemented) {
        if (stubUnknown) {
            MACEMU_LOG_WARN("stub: %s!%s geeft 0 terug (--stub-unknown staat aan)",
                            e.dll.c_str(), e.name.c_str());
            cpu.gpr[RAX] = 0;
            return;
        }
        throw EmuError(strFormat(
            "niet-geïmplementeerde Win32-functie: %s!%s\n"
            "  Dit is precies het Fase 3-werk: implementeer deze functie in\n"
            "  src/win32/ en probeer opnieuw. Met --stub-unknown geeft macemu\n"
            "  0 terug in plaats van te stoppen (werkt lang niet altijd).",
            e.dll.c_str(), e.name.c_str()));
    }

    MACEMU_LOG_DEBUG("-> %s!%s(0x%llx, 0x%llx, 0x%llx, 0x%llx)", e.dll.c_str(), e.name.c_str(),
                     (unsigned long long)cpu.gpr[RCX], (unsigned long long)cpu.gpr[RDX],
                     (unsigned long long)cpu.gpr[R8], (unsigned long long)cpu.gpr[R9]);
    uint64_t result = e.handler(emu_, cpu);
    cpu.gpr[RAX] = result;
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------
uint64_t Win32::createHandle(std::shared_ptr<KernelObject> obj) {
    uint64_t h = nextHandle_;
    nextHandle_ += 4;
    handles_[h] = std::move(obj);
    return h;
}

std::shared_ptr<KernelObject> Win32::lookupHandle(uint64_t h) const {
    auto it = handles_.find(h);
    return it == handles_.end() ? nullptr : it->second;
}

bool Win32::closeHandle(uint64_t h) {
    return handles_.erase(h) > 0;
}

uint64_t Win32::stdHandle(int which) const {
    switch (which) {
        case -10: return stdInHandle_;
        case -12: return stdErrHandle_;
        default: return stdOutHandle_;
    }
}

void Win32::setLastError(uint32_t err) {
    lastError_ = err;
    if (teb_) emu_.memory().write32(teb_ + 0x68, err);
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------
uint64_t Win32::moduleHandleFor(const std::string& dllName) {
    std::string key = toLower(dllName);
    if (key.empty()) return emu_.image().base;
    if (key.find('.') == std::string::npos) key += ".dll";

    std::string exeName = toLower(emu_.image().name);
    if (key == exeName) return emu_.image().base;

    auto it = moduleHandles_.find(key);
    if (it != moduleHandles_.end()) return it->second;

    // Een nep-module: één pagina met een geldige MZ/PE-header, zodat code die
    // in de header gaat neuzen niet meteen crasht.
    uint64_t base = nextFakeModule_;
    nextFakeModule_ += 0x10000;
    emu_.memory().map(base, 0x1000, PROT_R, "module " + key);
    emu_.memory().protect(base, 0x1000, PROT_RW);
    emu_.memory().write16(base, 0x5A4D);        // MZ
    emu_.memory().write32(base + 0x3C, 0x80);   // e_lfanew
    emu_.memory().write32(base + 0x80, 0x4550); // PE\0\0
    emu_.memory().write16(base + 0x84, pe::kMachineAmd64);
    emu_.memory().write16(base + 0x98, pe::kOptMagicPe32Plus);
    emu_.memory().protect(base, 0x1000, PROT_R);

    moduleHandles_[key] = base;
    return base;
}

std::string Win32::moduleNameFor(uint64_t handle) const {
    if (handle == emu_.image().base || handle == 0) return emu_.image().name;
    for (const auto& kv : moduleHandles_)
        if (kv.second == handle) return kv.first;
    return {};
}

// ---------------------------------------------------------------------------
// TEB / PEB / command line
// ---------------------------------------------------------------------------
void Win32::setupProcessEnvironment(uint64_t imageBase, const std::string& exePath,
                                    const std::vector<std::string>& args) {
    Memory& m = emu_.memory();

    teb_ = Emulator::kTebBase;
    m.map(teb_, 0x2000, PROT_RW, "TEB");
    peb_ = teb_ + 0x2000;
    m.map(peb_, 0x1000, PROT_RW, "PEB");
    uint64_t ldr = peb_ + 0x400;
    uint64_t params = peb_ + 0x600;

    uint64_t stackTop = Emulator::kStackTop;
    uint64_t stackLimit = stackTop - emu_.options().stackSize;

    // --- TEB (x64 offsets) ---
    m.write64(teb_ + 0x00, 0);          // NtTib.ExceptionList
    m.write64(teb_ + 0x08, stackTop);   // StackBase
    m.write64(teb_ + 0x10, stackLimit); // StackLimit
    m.write64(teb_ + 0x30, teb_);       // NtTib.Self
    m.write64(teb_ + 0x40, 0x1234);     // ClientId.UniqueProcess
    m.write64(teb_ + 0x48, 0x5678);     // ClientId.UniqueThread
    m.write64(teb_ + 0x58, teb_ + 0x1480); // ThreadLocalStoragePointer
    m.write64(teb_ + 0x60, peb_);       // ProcessEnvironmentBlock
    m.write32(teb_ + 0x68, 0);          // LastErrorValue

    // --- PEB ---
    m.write8(peb_ + 0x02, 0);           // BeingDebugged
    m.write64(peb_ + 0x10, imageBase);  // ImageBaseAddress
    m.write64(peb_ + 0x18, ldr);        // Ldr
    m.write64(peb_ + 0x20, params);     // ProcessParameters
    m.write64(peb_ + 0x30, processHeapHandle_);
    m.write32(peb_ + 0xBC, 0);          // NtGlobalFlag
    m.write32(peb_ + 0x118, 10);        // OSMajorVersion
    m.write32(peb_ + 0x11C, 0);         // OSMinorVersion
    m.write16(peb_ + 0x120, 19045);     // OSBuildNumber
    m.write32(peb_ + 0x124, 2);         // OSPlatformId (VER_PLATFORM_WIN32_NT)

    // --- PEB_LDR_DATA: lege, naar zichzelf wijzende lijsten ---
    m.write32(ldr + 0x00, 0x58);
    m.write8(ldr + 0x04, 1); // Initialized
    m.write64(ldr + 0x10, ldr + 0x10);  // InLoadOrderModuleList
    m.write64(ldr + 0x18, ldr + 0x10);
    m.write64(ldr + 0x20, ldr + 0x20);  // InMemoryOrderModuleList
    m.write64(ldr + 0x28, ldr + 0x20);
    m.write64(ldr + 0x30, ldr + 0x30);  // InInitializationOrderModuleList
    m.write64(ldr + 0x38, ldr + 0x30);

    // --- command line ---
    std::string cmdline = "\"" + exePath + "\"";
    for (const auto& a : args) cmdline += " " + a;
    cmdlineA_ = emu_.internCString(cmdline);
    cmdlineW_ = emu_.internWideString(cmdline);
    cmdlineText_ = cmdline;

    std::string imagePath = exePath;
    uint64_t imagePathW = emu_.internWideString(imagePath);

    auto writeUnicodeString = [&](uint64_t at, uint64_t buffer, const std::string& text) {
        uint16_t len = static_cast<uint16_t>(utf8ToUtf16(text).size() * 2);
        m.write16(at + 0, len);
        m.write16(at + 2, static_cast<uint16_t>(len + 2));
        m.write64(at + 8, buffer);
    };

    // --- RTL_USER_PROCESS_PARAMETERS (x64) ---
    m.fill(params, 0, 0x400);
    m.write32(params + 0x00, 0x400); // MaximumLength
    m.write32(params + 0x04, 0x400); // Length
    m.write64(params + 0x20, stdInHandle_);
    m.write64(params + 0x28, stdOutHandle_);
    m.write64(params + 0x30, stdErrHandle_);
    writeUnicodeString(params + 0x60, imagePathW, imagePath);
    writeUnicodeString(params + 0x70, cmdlineW_, cmdline);

    // --- environment block (UTF-16, dubbel-NUL afgesloten) ---
    std::vector<std::string> env = {"PATH=C:\\Windows\\system32", "SystemRoot=C:\\Windows",
                                    "TEMP=C:\\Windows\\Temp", "USERNAME=macemu",
                                    "COMPUTERNAME=MACEMU", "OS=Windows_NT"};
    std::vector<uint16_t> envW;
    for (const auto& e : env) {
        auto w = utf8ToUtf16(e);
        envW.insert(envW.end(), w.begin(), w.end());
        envW.push_back(0);
    }
    envW.push_back(0);
    environmentW_ = emu_.allocScratch(envW.size() * 2, 8);
    for (size_t i = 0; i < envW.size(); ++i) m.write16(environmentW_ + i * 2, envW[i]);
    m.write64(params + 0x80, environmentW_);

    std::string envA;
    for (const auto& e : env) { envA += e; envA += '\0'; }
    envA += '\0';
    environmentA_ = emu_.allocScratch(envA.size(), 8);
    m.writeBlock(environmentA_, envA.data(), envA.size());

    MACEMU_LOG_DEBUG("TEB op 0x%llx, PEB op 0x%llx, cmdline: %s", (unsigned long long)teb_,
                     (unsigned long long)peb_, cmdline.c_str());
}

// ---------------------------------------------------------------------------
// GDI-objectbeheer
// ---------------------------------------------------------------------------
uint64_t Win32::createGdiObject(const GdiObject& o) {
    uint64_t h = nextGdiHandle_;
    nextGdiHandle_ += 8;
    gdiObjects_[h] = o;
    return h;
}

GdiObject* Win32::gdiObject(uint64_t h) {
    auto it = gdiObjects_.find(h);
    return it == gdiObjects_.end() ? nullptr : &it->second;
}

uint64_t Win32::createDc(const DeviceContext& d) {
    uint64_t h = nextDcHandle_;
    nextDcHandle_ += 8;
    dcs_[h] = d;
    return h;
}

DeviceContext* Win32::dc(uint64_t h) {
    auto it = dcs_.find(h);
    return it == dcs_.end() ? nullptr : &it->second;
}

void Win32::destroyDc(uint64_t h) { dcs_.erase(h); }

uint64_t Win32::stockObject(int index) {
    auto it = stockObjects_.find(index);
    if (it != stockObjects_.end()) return it->second;

    GdiObject o;
    o.stock = true;
    switch (index) {
        case 0: o.kind = GdiObject::Kind::Brush; o.color = 0xFFFFFF; break; // WHITE_BRUSH
        case 1: o.kind = GdiObject::Kind::Brush; o.color = 0xC0C0C0; break; // LTGRAY_BRUSH
        case 2: o.kind = GdiObject::Kind::Brush; o.color = 0x808080; break; // GRAY_BRUSH
        case 3: o.kind = GdiObject::Kind::Brush; o.color = 0x404040; break; // DKGRAY_BRUSH
        case 4: o.kind = GdiObject::Kind::Brush; o.color = 0x000000; break; // BLACK_BRUSH
        case 5: o.kind = GdiObject::Kind::Brush; o.color = 0; o.style = 1; break; // NULL_BRUSH
        case 6: o.kind = GdiObject::Kind::Pen; o.color = 0xFFFFFF; break;   // WHITE_PEN
        case 7: o.kind = GdiObject::Kind::Pen; o.color = 0x000000; break;   // BLACK_PEN
        case 8: o.kind = GdiObject::Kind::Pen; o.color = 0; o.style = 5; break; // NULL_PEN
        default: o.kind = GdiObject::Kind::Font; o.height = 8; break;
    }
    uint64_t h = createGdiObject(o);
    stockObjects_[index] = h;
    return h;
}

// ---------------------------------------------------------------------------
// Vensters
// ---------------------------------------------------------------------------
WindowObject* Win32::window(uint64_t hwnd) {
    auto it = windows_.find(hwnd);
    return it == windows_.end() ? nullptr : &it->second;
}

uint64_t Win32::createWindow(const WndClassInfo& cls, const std::string& title, int x, int y,
                             int w, int h, uint32_t style, uint64_t hInstance) {
    uint64_t hwnd = nextHwnd_;
    nextHwnd_ += 0x10;

    WindowObject win;
    win.hwnd = hwnd;
    win.className = cls.name;
    win.title = title;
    win.x = x;
    win.y = y;
    win.width = w > 0 ? w : 640;
    win.height = h > 0 ? h : 480;
    // Ruwe benadering van de non-client area (titelbalk + rand).
    win.clientWidth = win.width - 16;
    win.clientHeight = win.height - 39;
    if (win.clientWidth < 1) win.clientWidth = win.width;
    if (win.clientHeight < 1) win.clientHeight = win.height;
    win.wndProc = cls.wndProc;
    win.hInstance = hInstance;
    win.style = style;
    win.client.resize(win.clientWidth, win.clientHeight);
    win.extra.assign(static_cast<size_t>(cls.cbWndExtra / 8 + 4), 0);

    uint32_t bg = 0xC0C0C0;
    if (GdiObject* b = gdiObject(cls.hbrBackground)) bg = b->color;
    win.client.clear(bg);

    windows_[hwnd] = std::move(win);
    return hwnd;
}

void Win32::destroyWindow(uint64_t hwnd) {
    auto it = windows_.find(hwnd);
    if (it == windows_.end()) return;
    it->second.destroyed = true;
}

void Win32::postMessage(uint64_t hwnd, uint32_t msg, uint64_t wParam, uint64_t lParam) {
    GuestMessage m;
    m.hwnd = hwnd;
    m.message = msg;
    m.wParam = wParam;
    m.lParam = lParam;
    messageQueue_.push_back(m);
}

bool Win32::popMessage(GuestMessage& out) {
    if (messageQueue_.empty()) return false;
    out = messageQueue_.front();
    messageQueue_.pop_front();
    return true;
}

void Win32::ensureHostWindow(WindowObject& w) {
    if (hostWindow_) return;
    hostWindow_ = HostWindow::create(w.title, w.clientWidth, w.clientHeight,
                                     emu_.options().headless);
    MACEMU_LOG_INFO("venster geopend via de %s-backend (%dx%d)", hostWindow_->backendName(),
                    w.clientWidth, w.clientHeight);
}

void Win32::pumpHostEvents() {
    if (!hostWindow_) return;
    HostEvent ev;
    while (hostWindow_->pollEvent(ev)) {
        uint64_t target = focusWindow_;
        if (!target && !windows_.empty()) target = windows_.begin()->first;
        if (!target) continue;
        uint64_t lp = (static_cast<uint64_t>(static_cast<uint16_t>(ev.y)) << 16) |
                      static_cast<uint16_t>(ev.x);
        switch (ev.type) {
            case HostEvent::Type::Quit:
                postMessage(target, WM_CLOSE_, 0, 0);
                break;
            case HostEvent::Type::MouseMove:
                postMessage(target, WM_MOUSEMOVE_, 0, lp);
                break;
            case HostEvent::Type::MouseDown:
                postMessage(target, ev.button == 2 ? WM_RBUTTONDOWN_ : WM_LBUTTONDOWN_,
                            ev.button == 2 ? 2 : 1, lp);
                break;
            case HostEvent::Type::MouseUp:
                postMessage(target, ev.button == 2 ? WM_RBUTTONUP_ : WM_LBUTTONUP_, 0, lp);
                break;
            case HostEvent::Type::KeyDown:
                postMessage(target, WM_KEYDOWN_, static_cast<uint64_t>(ev.key), 1);
                break;
            case HostEvent::Type::KeyUp:
                postMessage(target, WM_KEYUP_, static_cast<uint64_t>(ev.key), 1);
                break;
            case HostEvent::Type::Char:
                postMessage(target, WM_CHAR_, static_cast<uint64_t>(ev.ch), 1);
                break;
            default:
                break;
        }
    }
}

bool Win32::saveScreenshot(const std::string& path) {
    for (auto& kv : windows_) {
        if (!kv.second.destroyed && kv.second.client.width > 0) {
            if (kv.second.client.writePpm(path)) {
                MACEMU_LOG_INFO("screenshot geschreven naar %s (%dx%d)", path.c_str(),
                                kv.second.client.width, kv.second.client.height);
                return true;
            }
        }
    }
    return saveLastHeadlessFrame(path);
}

void Win32::presentAll() {
    if (!hostWindow_) return;
    for (auto& kv : windows_) {
        if (kv.second.visible && !kv.second.destroyed) {
            hostWindow_->present(kv.second.client);
            return;
        }
    }
}

uint64_t Win32::defWindowProc(Cpu& cpu, uint64_t hwnd, uint32_t msg, uint64_t wParam,
                              uint64_t lParam) {
    switch (msg) {
        case WM_CLOSE_:
            destroyWindow(hwnd);
            postMessage(hwnd, WM_DESTROY_, 0, 0);
            return 0;
        case WM_DESTROY_:
            return 0;
        case WM_ERASEBKGND_: {
            WindowObject* w = window(hwnd);
            if (w) {
                auto ci = classes_.find(w->className);
                uint32_t bg = 0xC0C0C0;
                if (ci != classes_.end())
                    if (GdiObject* b = gdiObject(ci->second.hbrBackground)) bg = b->color;
                w->client.clear(bg);
            }
            return 1;
        }
        case WM_PAINT_:
            return 0;
        default:
            return 0;
    }
    (void)cpu;
    (void)wParam;
    (void)lParam;
}

// ---------------------------------------------------------------------------
// Rapportage
// ---------------------------------------------------------------------------
void Win32::dumpApiUsage(std::string& out) const {
    std::vector<const ApiEntry*> sorted;
    for (const auto& e : slots_)
        if (e.callCount) sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(),
              [](const ApiEntry* a, const ApiEntry* b) { return a->callCount > b->callCount; });

    out += "\nGebruikte Win32-functies:\n";
    for (const auto* e : sorted)
        out += strFormat("  %8llu  %s!%s%s\n", (unsigned long long)e->callCount, e->dll.c_str(),
                         e->name.c_str(), e->implemented ? "" : "  (stub)");
}

} // namespace macemu
