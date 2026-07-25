// macemu - win32.h
//
// Fase 3/4: de Win32-emulatielaag.
//
// Kernidee: we emuleren GEEN echte Windows-DLL's. Elke geïmporteerde functie
// krijgt een adres in een speciaal "HLE"-gebied (High Level Emulation). Zodra
// de CPU daar naartoe springt, roept hij een C++-functie aan die het gedrag
// nabootst en netjes volgens de Win64-conventie terugkeert.
//
// Voordeel: geen DLL-bestanden nodig, geen ntdll, geen syscalls.
// Nadeel: elke functie die de gast gebruikt moet met de hand geschreven worden.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "macemu/cpu.h"
#include "macemu/host_window.h"
#include "macemu/memory.h"

namespace macemu {

class Emulator;
class Win32;

// ---------------------------------------------------------------------------
// Guest heap: een simpele allocator in het gastgeheugen.
// ---------------------------------------------------------------------------
class GuestHeap {
public:
    GuestHeap(Memory& mem, uint64_t base, uint64_t maxSize, const std::string& name);

    uint64_t allocate(uint64_t size, bool zeroMemory);
    bool free(uint64_t addr);
    uint64_t sizeOf(uint64_t addr) const;
    uint64_t reallocate(uint64_t addr, uint64_t newSize, bool zeroMemory);
    bool owns(uint64_t addr) const { return addr >= base_ && addr < base_ + maxSize_; }
    uint64_t base() const { return base_; }
    uint64_t bytesInUse() const { return inUse_; }

private:
    struct Block {
        uint64_t size;
        bool free;
    };
    void commitUpTo(uint64_t addr);

    Memory& mem_;
    uint64_t base_;
    uint64_t maxSize_;
    uint64_t cursor_;    // eerste nog nooit uitgedeelde byte
    uint64_t committed_; // hoever we pagina's hebben aangemaakt
    uint64_t inUse_ = 0;
    std::string name_;
    std::map<uint64_t, Block> blocks_; // adres -> blok (adres = gebruikerspointer)
};

// ---------------------------------------------------------------------------
// Kernel-objecten achter een HANDLE
// ---------------------------------------------------------------------------
struct KernelObject {
    virtual ~KernelObject() = default;
    virtual const char* typeName() const = 0;
};

struct FileObject : KernelObject {
    const char* typeName() const override { return "File"; }
    FILE* fp = nullptr;
    bool isConsole = false;
    int consoleStream = 1; // 1 = stdout, 2 = stderr
    bool ownsFp = false;
    std::string path;
    ~FileObject() override;
};

struct EventObject : KernelObject {
    const char* typeName() const override { return "Event"; }
    bool manualReset = false;
    bool signaled = false;
    std::string name;
};

struct MutexObject : KernelObject {
    const char* typeName() const override { return "Mutex"; }
    bool owned = false;
    std::string name;
};

struct ModuleObject : KernelObject {
    const char* typeName() const override { return "Module"; }
    std::string name;
    uint64_t base = 0;
};

struct HeapObject : KernelObject {
    const char* typeName() const override { return "Heap"; }
    std::shared_ptr<GuestHeap> heap;
};

struct ThreadObject : KernelObject {
    const char* typeName() const override { return "Thread"; }
    uint32_t tid = 0;
};

// ---------------------------------------------------------------------------
// GDI-objecten
// ---------------------------------------------------------------------------
struct GdiObject {
    enum class Kind { Brush, Pen, Font, Bitmap, Region, Palette } kind = Kind::Brush;
    uint32_t color = 0;
    int width = 1;
    int style = 0;
    int height = 8;      // font
    bool stock = false;
    std::string faceName;
};

struct DeviceContext {
    uint64_t hwnd = 0;
    Framebuffer* target = nullptr;
    uint64_t currentBrush = 0;
    uint64_t currentPen = 0;
    uint64_t currentFont = 0;
    uint32_t textColor = 0x000000;
    uint32_t bkColor = 0xFFFFFF;
    int bkMode = 2; // OPAQUE
    int originX = 0, originY = 0;
    int curX = 0, curY = 0;
    bool isPaintDc = false;
};

// ---------------------------------------------------------------------------
// Vensters
// ---------------------------------------------------------------------------
struct WndClassInfo {
    std::string name;
    uint64_t wndProc = 0;
    uint64_t hbrBackground = 0;
    uint32_t style = 0;
    uint64_t hIcon = 0, hCursor = 0;
    uint64_t hInstance = 0;
    int cbWndExtra = 0;
};

struct WindowObject {
    uint64_t hwnd = 0;
    std::string className;
    std::string title;
    int x = 0, y = 0, width = 0, height = 0;             // buitenmaat
    int clientWidth = 0, clientHeight = 0;
    uint64_t wndProc = 0;
    uint64_t hInstance = 0;
    uint64_t hMenu = 0;
    uint64_t parent = 0;
    uint32_t style = 0, exStyle = 0;
    bool visible = false;
    bool destroyed = false;
    Framebuffer client;
    std::vector<uint64_t> extra;   // GWLP_USERDATA e.d.
    bool needsPaint = true;
};

struct GuestMessage {
    uint64_t hwnd = 0;
    uint32_t message = 0;
    uint64_t wParam = 0;
    uint64_t lParam = 0;
    uint32_t time = 0;
    int32_t ptX = 0, ptY = 0;
};

struct TimerInfo {
    uint64_t hwnd = 0;
    uint64_t id = 0;
    uint32_t intervalMs = 0;
    uint64_t nextFireMs = 0;
    uint64_t callback = 0;
};

// ---------------------------------------------------------------------------
// De Win32-laag zelf
// ---------------------------------------------------------------------------
using ApiHandler = std::function<uint64_t(Emulator&, Cpu&)>;

struct ApiEntry {
    std::string dll;
    std::string name;
    ApiHandler handler;
    bool implemented = false;
    uint64_t callCount = 0;
};

class Win32 : public HostCallSink {
public:
    explicit Win32(Emulator& emu);
    ~Win32() override;

    // Zet het HLE-gebied klaar en registreert alle ingebouwde functies.
    void initialize(uint64_t hleBase, uint64_t hleSize);

    // Geeft het HLE-adres voor een import terug (maakt zo nodig een slot aan).
    uint64_t resolveImport(const std::string& dll, const std::string& name, uint16_t ordinal,
                           bool byOrdinal);

    void onHostCall(Cpu& cpu, uint32_t id) override;

    // --- registratie ---
    void registerApi(const std::string& dll, const std::string& name, ApiHandler fn);
    bool hasApi(const std::string& dll, const std::string& name) const;

    // --- process-omgeving ---
    void setupProcessEnvironment(uint64_t imageBase, const std::string& exePath,
                                 const std::vector<std::string>& args);
    uint64_t tebAddress() const { return teb_; }
    uint64_t pebAddress() const { return peb_; }

    // --- handles ---
    uint64_t createHandle(std::shared_ptr<KernelObject> obj);
    std::shared_ptr<KernelObject> lookupHandle(uint64_t h) const;
    bool closeHandle(uint64_t h);
    template <typename T>
    std::shared_ptr<T> lookup(uint64_t h) const {
        return std::dynamic_pointer_cast<T>(lookupHandle(h));
    }

    // --- heap ---
    GuestHeap& processHeap() { return *processHeap_; }
    uint64_t processHeapHandle() const { return processHeapHandle_; }

    // --- modules ---
    uint64_t moduleHandleFor(const std::string& dllName);
    std::string moduleNameFor(uint64_t handle) const;

    // --- fouten ---
    void setLastError(uint32_t err);
    uint32_t lastError() const { return lastError_; }

    // --- GDI ---
    uint64_t createGdiObject(const GdiObject& o);
    GdiObject* gdiObject(uint64_t h);
    uint64_t createDc(const DeviceContext& dc);
    DeviceContext* dc(uint64_t h);
    void destroyDc(uint64_t h);
    uint64_t stockObject(int index);

    // --- proces-omgeving (adressen in gastgeheugen) ---
    uint64_t commandLineA() const { return cmdlineA_; }
    uint64_t commandLineW() const { return cmdlineW_; }
    const std::string& commandLineText() const { return cmdlineText_; }
    uint64_t environmentA() const { return environmentA_; }
    uint64_t environmentW() const { return environmentW_; }
    uint64_t stdHandle(int which) const; // -10 = in, -11 = out, -12 = err

    // atexit/_crt_atexit: functies die de gast bij ExitProcess nog aangeroepen
    // wil hebben (CRT-cleanup, o.a. stdio-flush). Worden in omgekeerde
    // volgorde van registratie aangeroepen, zoals de C-standaard voorschrijft.
    void registerAtExit(uint64_t fn) { atexitCallbacks_.push_back(fn); }
    const std::vector<uint64_t>& atexitCallbacks() const { return atexitCallbacks_; }

    // --- vensters ---
    std::map<std::string, WndClassInfo>& classes() { return classes_; }
    std::map<uint64_t, WindowObject>& windowMap() { return windows_; }
    void setFocusWindow(uint64_t hwnd) { focusWindow_ = hwnd; }
    uint64_t focusWindow() const { return focusWindow_; }
    WindowObject* window(uint64_t hwnd);
    uint64_t createWindow(const WndClassInfo& cls, const std::string& title, int x, int y, int w,
                          int h, uint32_t style, uint64_t hInstance);
    void destroyWindow(uint64_t hwnd);
    void postMessage(uint64_t hwnd, uint32_t msg, uint64_t wParam, uint64_t lParam);
    bool popMessage(GuestMessage& out);
    bool hasMessages() const { return !messageQueue_.empty(); }
    void pumpHostEvents();
    void presentAll();
    uint64_t defWindowProc(Cpu& cpu, uint64_t hwnd, uint32_t msg, uint64_t wParam,
                           uint64_t lParam);
    HostWindow* hostWindow() { return hostWindow_.get(); }
    // Schrijft de clientarea van het eerste zichtbare venster weg als .ppm.
    bool saveScreenshot(const std::string& path);
    void ensureHostWindow(WindowObject& w);
    std::vector<TimerInfo>& timers() { return timers_; }
    bool quitRequested = false;
    int quitCode = 0;

    // --- statistiek ---
    void dumpApiUsage(std::string& out) const;
    const std::vector<ApiEntry>& slots() const { return slots_; }

    // Bij true geven onbekende imports gewoon 0 terug in plaats van te crashen.
    bool stubUnknown = false;

private:
    void registerKernel32();
    void registerUser32();
    void registerGdi32();
    void registerMisc();

    Emulator& emu_;
    uint64_t hleBase_ = 0;
    uint64_t hleSize_ = 0;
    uint32_t nextSlot_ = 0;
    uint32_t maxSlots_ = 0;

    std::vector<ApiEntry> slots_;
    std::map<std::string, uint32_t> slotByName_; // "dll!func" -> slot
    std::map<std::string, ApiHandler> registry_; // "dll!func" -> implementatie

    std::unordered_map<uint64_t, std::shared_ptr<KernelObject>> handles_;
    uint64_t nextHandle_ = 0x100;

    std::shared_ptr<GuestHeap> processHeap_;
    uint64_t processHeapHandle_ = 0;
    std::vector<std::shared_ptr<GuestHeap>> extraHeaps_;

    std::map<std::string, uint64_t> moduleHandles_;
    uint64_t nextFakeModule_ = 0x7FF700000000ull;

    uint32_t lastError_ = 0;
    uint64_t teb_ = 0;
    uint64_t peb_ = 0;

    uint64_t stdInHandle_ = 0, stdOutHandle_ = 0, stdErrHandle_ = 0;
    uint64_t cmdlineA_ = 0, cmdlineW_ = 0;
    std::string cmdlineText_;
    uint64_t environmentA_ = 0, environmentW_ = 0;
    std::vector<uint64_t> atexitCallbacks_;

    std::map<uint64_t, GdiObject> gdiObjects_;
    uint64_t nextGdiHandle_ = 0x9000;
    std::map<uint64_t, DeviceContext> dcs_;
    uint64_t nextDcHandle_ = 0xD000;
    std::map<int, uint64_t> stockObjects_;

    std::map<std::string, WndClassInfo> classes_;
    std::map<uint64_t, WindowObject> windows_;
    uint64_t nextHwnd_ = 0x10000;
    std::deque<GuestMessage> messageQueue_;
    std::vector<TimerInfo> timers_;
    std::unique_ptr<HostWindow> hostWindow_;
    uint64_t focusWindow_ = 0;

    friend uint64_t apiArg(Cpu& cpu, int index);
};

// Argument volgens de Win64-conventie ophalen. Let op: op het moment dat een
// host-call draait staat het returnadres nog op de stack, dus stack-argument
// n (n >= 4) staat op [rsp + 8 + n*8].
uint64_t apiArg(Cpu& cpu, int index);
double apiArgDouble(Cpu& cpu, int index);

// Veelgebruikte Win32-constanten.
constexpr uint32_t ERROR_SUCCESS_ = 0;
constexpr uint32_t ERROR_FILE_NOT_FOUND_ = 2;
constexpr uint32_t ERROR_INVALID_HANDLE_ = 6;
constexpr uint32_t ERROR_NOT_ENOUGH_MEMORY_ = 8;
constexpr uint32_t ERROR_INVALID_PARAMETER_ = 87;
constexpr uint32_t ERROR_CALL_NOT_IMPLEMENTED_ = 120;
constexpr uint32_t ERROR_PROC_NOT_FOUND_ = 127;

constexpr uint32_t WM_CREATE_ = 0x0001;
constexpr uint32_t WM_DESTROY_ = 0x0002;
constexpr uint32_t WM_SIZE_ = 0x0005;
constexpr uint32_t WM_PAINT_ = 0x000F;
constexpr uint32_t WM_CLOSE_ = 0x0010;
constexpr uint32_t WM_QUIT_ = 0x0012;
constexpr uint32_t WM_ERASEBKGND_ = 0x0014;
constexpr uint32_t WM_KEYDOWN_ = 0x0100;
constexpr uint32_t WM_KEYUP_ = 0x0101;
constexpr uint32_t WM_CHAR_ = 0x0102;
constexpr uint32_t WM_COMMAND_ = 0x0111;
constexpr uint32_t WM_TIMER_ = 0x0113;
constexpr uint32_t WM_MOUSEMOVE_ = 0x0200;
constexpr uint32_t WM_LBUTTONDOWN_ = 0x0201;
constexpr uint32_t WM_LBUTTONUP_ = 0x0202;
constexpr uint32_t WM_RBUTTONDOWN_ = 0x0204;
constexpr uint32_t WM_RBUTTONUP_ = 0x0205;
constexpr uint32_t WM_NCCREATE_ = 0x0081;
constexpr uint32_t WM_NCDESTROY_ = 0x0082;

} // namespace macemu
