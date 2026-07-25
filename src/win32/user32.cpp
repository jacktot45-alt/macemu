// macemu - user32-stubs (Fase 4)
//
// Dit is de laag waar het echt interessant wordt: de gast registreert een
// window class met een *callback in gastcode* (de WndProc), en verwacht dat
// wij die aanroepen. Dat betekent dat de host de CPU opnieuw moet starten
// midden in een host-functie: zie Cpu::callGuest.
//
//   gast: DispatchMessage(&msg)
//     -> host: user32!DispatchMessageW
//        -> host: cpu.callGuest(wndProc, {hwnd, msg, wParam, lParam})
//           -> gast: WndProc(...)   <- draait weer als geëmuleerde x86-code
//              -> host: gdi32!FillRect ...
#include <chrono>
#include <thread>

#include "macemu/emulator.h"
#include "macemu/win32.h"

namespace macemu {

namespace {

uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void writeRect(Cpu& cpu, uint64_t p, int l, int t, int r, int b) {
    if (!p) return;
    cpu.mem.write32(p + 0, static_cast<uint32_t>(l));
    cpu.mem.write32(p + 4, static_cast<uint32_t>(t));
    cpu.mem.write32(p + 8, static_cast<uint32_t>(r));
    cpu.mem.write32(p + 12, static_cast<uint32_t>(b));
}

void writeMsg(Cpu& cpu, uint64_t p, const GuestMessage& m) {
    if (!p) return;
    cpu.mem.write64(p + 0, m.hwnd);
    cpu.mem.write32(p + 8, m.message);
    cpu.mem.write64(p + 16, m.wParam);
    cpu.mem.write64(p + 24, m.lParam);
    cpu.mem.write32(p + 32, m.time);
    cpu.mem.write32(p + 36, static_cast<uint32_t>(m.ptX));
    cpu.mem.write32(p + 40, static_cast<uint32_t>(m.ptY));
}

// Timers laten aflopen en zo nodig WM_TIMER posten.
void serviceTimers(Win32& w) {
    uint64_t now = nowMs();
    for (auto& t : w.timers()) {
        if (t.intervalMs == 0) continue;
        if (now >= t.nextFireMs) {
            t.nextFireMs = now + t.intervalMs;
            w.postMessage(t.hwnd, WM_TIMER_, t.id, t.callback);
        }
    }
}

// Vensters die opnieuw getekend moeten worden krijgen een WM_PAINT.
void queuePaints(Win32& w) {
    for (auto& kv : w.windowMap()) {
        WindowObject& win = kv.second;
        if (win.visible && !win.destroyed && win.needsPaint) {
            win.needsPaint = false;
            w.postMessage(win.hwnd, WM_PAINT_, 0, 0);
        }
    }
}

} // namespace

void Win32::registerUser32() {
    auto reg = [this](const char* name, ApiHandler fn) {
        registerApi("user32.dll", name, std::move(fn));
    };

    // ------------------------------------------------------ window classes
    auto registerClass = [](bool wide, bool ex) {
        return [wide, ex](Emulator& emu, Cpu& cpu) -> uint64_t {
            uint64_t p = apiArg(cpu, 0);
            if (!p) return 0;
            // WNDCLASSW en WNDCLASSEXW zijn op x64 identiek vanaf offset 8;
            // alleen het style-veld schuift op omdat EX met cbSize begint.
            WndClassInfo c;
            c.style = cpu.mem.read32(p + (ex ? 4 : 0));
            c.wndProc = cpu.mem.read64(p + 8);
            c.cbWndExtra = static_cast<int>(cpu.mem.read32(p + 20));
            c.hInstance = cpu.mem.read64(p + 24);
            c.hIcon = cpu.mem.read64(p + 32);
            c.hCursor = cpu.mem.read64(p + 40);
            c.hbrBackground = cpu.mem.read64(p + 48);
            uint64_t namePtr = cpu.mem.read64(p + 64);
            c.name = namePtr ? (wide ? cpu.mem.readWideString(namePtr)
                                     : cpu.mem.readCString(namePtr))
                             : "";
            if (c.name.empty()) c.name = strFormat("class_%llx", (unsigned long long)c.wndProc);
            emu.win32().classes()[c.name] = c;
            MACEMU_LOG_DEBUG("RegisterClass(\"%s\") wndProc=0x%llx", c.name.c_str(),
                             (unsigned long long)c.wndProc);
            return 0xC000; // ATOM
        };
    };
    reg("RegisterClassA", registerClass(false, false));
    reg("RegisterClassW", registerClass(true, false));
    reg("RegisterClassExA", registerClass(false, true));
    reg("RegisterClassExW", registerClass(true, true));
    reg("UnregisterClassA", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("UnregisterClassW", [](Emulator&, Cpu&) -> uint64_t { return 1; });

    // ------------------------------------------------------------ vensters
    auto createWindowEx = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            uint64_t classPtr = apiArg(cpu, 1);
            uint64_t titlePtr = apiArg(cpu, 2);
            uint32_t style = static_cast<uint32_t>(apiArg(cpu, 3));
            int32_t x = static_cast<int32_t>(apiArg(cpu, 4));
            int32_t y = static_cast<int32_t>(apiArg(cpu, 5));
            int32_t w = static_cast<int32_t>(apiArg(cpu, 6));
            int32_t h = static_cast<int32_t>(apiArg(cpu, 7));
            uint64_t hInstance = apiArg(cpu, 10);
            uint64_t param = apiArg(cpu, 11);

            std::string className;
            if (classPtr > 0xFFFF)
                className = wide ? cpu.mem.readWideString(classPtr) : cpu.mem.readCString(classPtr);
            std::string title;
            if (titlePtr)
                title = wide ? cpu.mem.readWideString(titlePtr) : cpu.mem.readCString(titlePtr);

            Win32& W = emu.win32();
            auto it = W.classes().find(className);
            if (it == W.classes().end()) {
                if (W.classes().empty()) {
                    MACEMU_LOG_WARN("CreateWindowEx voor onbekende class \"%s\"", className.c_str());
                    return 0;
                }
                it = W.classes().begin(); // bv. ingebouwde controls: pak iets
            }
            // CW_USEDEFAULT
            if (x == static_cast<int32_t>(0x80000000)) x = 100;
            if (y == static_cast<int32_t>(0x80000000)) y = 100;
            if (w == static_cast<int32_t>(0x80000000) || w <= 0) w = 640;
            if (h == static_cast<int32_t>(0x80000000) || h <= 0) h = 480;

            uint64_t hwnd = W.createWindow(it->second, title, x, y, w, h, style, hInstance);
            W.setFocusWindow(hwnd);
            MACEMU_LOG_INFO("venster gemaakt: \"%s\" (%dx%d, class \"%s\")", title.c_str(), w, h,
                            className.c_str());

            // WM_NCCREATE + WM_CREATE, zoals Windows dat doet.
            WindowObject* win = W.window(hwnd);
            if (win && win->wndProc) {
                uint64_t cs = emu.allocScratch(0x60, 16);
                cpu.mem.fill(cs, 0, 0x60);
                cpu.mem.write64(cs + 0x00, param);      // lpCreateParams
                cpu.mem.write64(cs + 0x08, hInstance);  // hInstance
                cpu.mem.write32(cs + 0x1C, static_cast<uint32_t>(h));
                cpu.mem.write32(cs + 0x20, static_cast<uint32_t>(w));
                cpu.mem.write32(cs + 0x24, static_cast<uint32_t>(x));
                cpu.mem.write32(cs + 0x28, static_cast<uint32_t>(y));
                cpu.callGuest(win->wndProc, {hwnd, WM_NCCREATE_, 0, cs});
                cpu.callGuest(win->wndProc, {hwnd, WM_CREATE_, 0, cs});
            }
            return hwnd;
        };
    };
    reg("CreateWindowExA", createWindowEx(false));
    reg("CreateWindowExW", createWindowEx(true));

    reg("ShowWindow", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t hwnd = apiArg(cpu, 0);
        int32_t cmd = static_cast<int32_t>(apiArg(cpu, 1));
        WindowObject* w = emu.win32().window(hwnd);
        if (!w) return 0;
        bool was = w->visible;
        w->visible = (cmd != 0); // SW_HIDE = 0
        if (w->visible) {
            emu.win32().ensureHostWindow(*w);
            emu.win32().setFocusWindow(hwnd);
            w->needsPaint = true;
        }
        return was ? 1 : 0;
    });
    reg("UpdateWindow", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        WindowObject* w = emu.win32().window(apiArg(cpu, 0));
        if (w && w->visible && w->wndProc) {
            w->needsPaint = false;
            cpu.callGuest(w->wndProc, {w->hwnd, WM_PAINT_, 0, 0});
            emu.win32().presentAll();
        }
        return 1;
    });
    reg("DestroyWindow", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t hwnd = apiArg(cpu, 0);
        emu.win32().destroyWindow(hwnd);
        emu.win32().postMessage(hwnd, WM_DESTROY_, 0, 0);
        return 1;
    });
    reg("IsWindow", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        return emu.win32().window(apiArg(cpu, 0)) ? 1 : 0;
    });
    reg("SetFocus", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        emu.win32().setFocusWindow(apiArg(cpu, 0));
        return 0;
    });
    reg("GetFocus", [](Emulator& emu, Cpu&) -> uint64_t { return emu.win32().focusWindow(); });
    reg("SetForegroundWindow", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("SetActiveWindow", [](Emulator&, Cpu&) -> uint64_t { return 0; });

    // ------------------------------------------------------ message loop
    auto getMessage = [](bool /*wide*/) {
        return [](Emulator& emu, Cpu& cpu) -> uint64_t {
            uint64_t msgPtr = apiArg(cpu, 0);
            Win32& W = emu.win32();
            uint64_t idleStart = nowMs();
            double limitSec = emu.options().headless ? emu.options().headlessSeconds : 0.0;

            for (;;) {
                W.pumpHostEvents();
                serviceTimers(W);
                if (!W.hasMessages()) queuePaints(W);

                GuestMessage m;
                if (W.popMessage(m)) {
                    m.time = static_cast<uint32_t>(nowMs());
                    writeMsg(cpu, msgPtr, m);
                    return m.message == WM_QUIT_ ? 0 : 1;
                }
                if (W.quitRequested) {
                    GuestMessage q;
                    q.message = WM_QUIT_;
                    q.wParam = static_cast<uint64_t>(W.quitCode);
                    writeMsg(cpu, msgPtr, q);
                    return 0;
                }
                W.presentAll();
                if (W.hostWindow() && !W.hostWindow()->isOpen()) {
                    W.quitRequested = true;
                    continue;
                }
                if (limitSec > 0 && (nowMs() - idleStart) > limitSec * 1000) {
                    MACEMU_LOG_INFO("headless: tijdslimiet bereikt, venster wordt gesloten");
                    for (auto& kv : W.windowMap())
                        if (kv.second.visible) W.postMessage(kv.first, WM_CLOSE_, 0, 0);
                    W.quitRequested = true;
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        };
    };
    reg("GetMessageA", getMessage(false));
    reg("GetMessageW", getMessage(true));

    auto peekMessage = [](bool /*wide*/) {
        return [](Emulator& emu, Cpu& cpu) -> uint64_t {
            uint64_t msgPtr = apiArg(cpu, 0);
            uint32_t removeFlag = static_cast<uint32_t>(apiArg(cpu, 4));
            Win32& W = emu.win32();
            W.pumpHostEvents();
            serviceTimers(W);
            if (!W.hasMessages()) queuePaints(W);
            W.presentAll();

            GuestMessage m;
            if (!W.popMessage(m)) return 0;
            m.time = static_cast<uint32_t>(nowMs());
            writeMsg(cpu, msgPtr, m);
            if ((removeFlag & 1) == 0) W.postMessage(m.hwnd, m.message, m.wParam, m.lParam);
            return 1;
        };
    };
    reg("PeekMessageA", peekMessage(false));
    reg("PeekMessageW", peekMessage(true));

    reg("TranslateMessage", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (!p) return 0;
        uint32_t msg = cpu.mem.read32(p + 8);
        if (msg == WM_KEYDOWN_) {
            uint64_t vk = cpu.mem.read64(p + 16);
            // Heel ruwe vertaling: alleen ASCII-toetsen.
            if (vk >= 0x20 && vk < 0x7F)
                emu.win32().postMessage(cpu.mem.read64(p), WM_CHAR_, vk, 1);
        }
        return 1;
    });

    auto dispatchMessage = [](bool /*wide*/) {
        return [](Emulator& emu, Cpu& cpu) -> uint64_t {
            uint64_t p = apiArg(cpu, 0);
            if (!p) return 0;
            GuestMessage m;
            m.hwnd = cpu.mem.read64(p + 0);
            m.message = cpu.mem.read32(p + 8);
            m.wParam = cpu.mem.read64(p + 16);
            m.lParam = cpu.mem.read64(p + 24);

            Win32& W = emu.win32();
            WindowObject* w = W.window(m.hwnd);
            uint64_t proc = w ? w->wndProc : 0;
            if (!proc) return W.defWindowProc(cpu, m.hwnd, m.message, m.wParam, m.lParam);
            uint64_t r = cpu.callGuest(proc, {m.hwnd, m.message, m.wParam, m.lParam});
            if (m.message == WM_PAINT_) W.presentAll();
            return r;
        };
    };
    reg("DispatchMessageA", dispatchMessage(false));
    reg("DispatchMessageW", dispatchMessage(true));

    auto defWindowProc = [](bool /*wide*/) {
        return [](Emulator& emu, Cpu& cpu) -> uint64_t {
            return emu.win32().defWindowProc(cpu, apiArg(cpu, 0),
                                             static_cast<uint32_t>(apiArg(cpu, 1)),
                                             apiArg(cpu, 2), apiArg(cpu, 3));
        };
    };
    reg("DefWindowProcA", defWindowProc(false));
    reg("DefWindowProcW", defWindowProc(true));

    reg("PostQuitMessage", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        emu.win32().quitRequested = true;
        emu.win32().quitCode = static_cast<int>(apiArg(cpu, 0));
        emu.win32().postMessage(0, WM_QUIT_, apiArg(cpu, 0), 0);
        return 0;
    });
    auto postMessage = [](Emulator& emu, Cpu& cpu) -> uint64_t {
        emu.win32().postMessage(apiArg(cpu, 0), static_cast<uint32_t>(apiArg(cpu, 1)),
                                apiArg(cpu, 2), apiArg(cpu, 3));
        return 1;
    };
    reg("PostMessageA", postMessage);
    reg("PostMessageW", postMessage);

    auto sendMessage = [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t hwnd = apiArg(cpu, 0);
        uint32_t msg = static_cast<uint32_t>(apiArg(cpu, 1));
        WindowObject* w = emu.win32().window(hwnd);
        if (w && w->wndProc)
            return cpu.callGuest(w->wndProc, {hwnd, msg, apiArg(cpu, 2), apiArg(cpu, 3)});
        return emu.win32().defWindowProc(cpu, hwnd, msg, apiArg(cpu, 2), apiArg(cpu, 3));
    };
    reg("SendMessageA", sendMessage);
    reg("SendMessageW", sendMessage);

    // ----------------------------------------------------------- tekenen
    reg("BeginPaint", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t hwnd = apiArg(cpu, 0);
        uint64_t ps = apiArg(cpu, 1);
        WindowObject* w = emu.win32().window(hwnd);
        if (!w) return 0;
        DeviceContext d;
        d.hwnd = hwnd;
        d.target = &w->client;
        d.isPaintDc = true;
        uint64_t hdc = emu.win32().createDc(d);
        if (ps) {
            cpu.mem.fill(ps, 0, 72);
            cpu.mem.write64(ps + 0, hdc);
            cpu.mem.write32(ps + 8, 0); // fErase
            writeRect(cpu, ps + 12, 0, 0, w->clientWidth, w->clientHeight);
        }
        return hdc;
    });
    reg("EndPaint", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t ps = apiArg(cpu, 1);
        if (ps) emu.win32().destroyDc(cpu.mem.read64(ps + 0));
        emu.win32().presentAll();
        return 1;
    });
    reg("GetDC", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t hwnd = apiArg(cpu, 0);
        WindowObject* w = emu.win32().window(hwnd);
        if (!w) {
            if (emu.win32().windowMap().empty()) return 0;
            w = &emu.win32().windowMap().begin()->second;
        }
        DeviceContext d;
        d.hwnd = w->hwnd;
        d.target = &w->client;
        return emu.win32().createDc(d);
    });
    reg("ReleaseDC", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        emu.win32().destroyDc(apiArg(cpu, 1));
        emu.win32().presentAll();
        return 1;
    });
    reg("InvalidateRect", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        WindowObject* w = emu.win32().window(apiArg(cpu, 0));
        if (w) w->needsPaint = true;
        return 1;
    });
    reg("ValidateRect", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        WindowObject* w = emu.win32().window(apiArg(cpu, 0));
        if (w) w->needsPaint = false;
        return 1;
    });
    reg("RedrawWindow", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        WindowObject* w = emu.win32().window(apiArg(cpu, 0));
        if (w) w->needsPaint = true;
        return 1;
    });

    reg("FillRect", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t hdc = apiArg(cpu, 0);
        uint64_t rectPtr = apiArg(cpu, 1);
        uint64_t brush = apiArg(cpu, 2);
        DeviceContext* d = emu.win32().dc(hdc);
        if (!d || !d->target || !rectPtr) return 0;
        int l = static_cast<int32_t>(cpu.mem.read32(rectPtr + 0));
        int t = static_cast<int32_t>(cpu.mem.read32(rectPtr + 4));
        int r = static_cast<int32_t>(cpu.mem.read32(rectPtr + 8));
        int b = static_cast<int32_t>(cpu.mem.read32(rectPtr + 12));
        uint32_t color = 0xFFFFFF;
        if (GdiObject* g = emu.win32().gdiObject(brush)) color = g->color;
        d->target->fillRect(l, t, r - l, b - t, color);
        return 1;
    });

    reg("GetClientRect", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        WindowObject* w = emu.win32().window(apiArg(cpu, 0));
        if (!w) return 0;
        writeRect(cpu, apiArg(cpu, 1), 0, 0, w->clientWidth, w->clientHeight);
        return 1;
    });
    reg("GetWindowRect", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        WindowObject* w = emu.win32().window(apiArg(cpu, 0));
        if (!w) return 0;
        writeRect(cpu, apiArg(cpu, 1), w->x, w->y, w->x + w->width, w->y + w->height);
        return 1;
    });
    reg("AdjustWindowRect", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (!p) return 0;
        int l = static_cast<int32_t>(cpu.mem.read32(p + 0));
        int t = static_cast<int32_t>(cpu.mem.read32(p + 4));
        int r = static_cast<int32_t>(cpu.mem.read32(p + 8));
        int b = static_cast<int32_t>(cpu.mem.read32(p + 12));
        writeRect(cpu, p, l - 8, t - 31, r + 8, b + 8);
        return 1;
    });
    reg("AdjustWindowRectEx", registry_["user32.dll!AdjustWindowRect"]);
    reg("ClientToScreen", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("ScreenToClient", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("MoveWindow", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        WindowObject* w = emu.win32().window(apiArg(cpu, 0));
        if (!w) return 0;
        w->x = static_cast<int32_t>(apiArg(cpu, 1));
        w->y = static_cast<int32_t>(apiArg(cpu, 2));
        return 1;
    });
    reg("SetWindowPos", [](Emulator&, Cpu&) -> uint64_t { return 1; });

    // ---------------------------------------------------------- resources
    auto loadResourceStub = [](Emulator&, Cpu&) -> uint64_t { return 0x7001; };
    reg("LoadIconA", loadResourceStub);
    reg("LoadIconW", loadResourceStub);
    reg("LoadCursorA", loadResourceStub);
    reg("LoadCursorW", loadResourceStub);
    reg("LoadImageA", loadResourceStub);
    reg("LoadImageW", loadResourceStub);
    reg("LoadBitmapA", loadResourceStub);
    reg("LoadBitmapW", loadResourceStub);
    reg("LoadMenuA", loadResourceStub);
    reg("LoadMenuW", loadResourceStub);
    reg("SetMenu", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("SetCursor", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("ShowCursor", [](Emulator&, Cpu&) -> uint64_t { return 1; });

    auto messageBox = [](bool wide) {
        return [wide](Emulator&, Cpu& cpu) -> uint64_t {
            std::string text = apiArg(cpu, 1)
                                   ? (wide ? cpu.mem.readWideString(apiArg(cpu, 1))
                                           : cpu.mem.readCString(apiArg(cpu, 1)))
                                   : "";
            std::string caption = apiArg(cpu, 2)
                                      ? (wide ? cpu.mem.readWideString(apiArg(cpu, 2))
                                              : cpu.mem.readCString(apiArg(cpu, 2)))
                                      : "";
            std::printf("\n[MessageBox] %s\n             %s\n\n", caption.c_str(), text.c_str());
            return 1; // IDOK
        };
    };
    reg("MessageBoxA", messageBox(false));
    reg("MessageBoxW", messageBox(true));

    auto setWindowText = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            WindowObject* w = emu.win32().window(apiArg(cpu, 0));
            if (!w) return 0;
            uint64_t p = apiArg(cpu, 1);
            w->title = p ? (wide ? cpu.mem.readWideString(p) : cpu.mem.readCString(p)) : "";
            if (emu.win32().hostWindow()) emu.win32().hostWindow()->setTitle(w->title);
            return 1;
        };
    };
    reg("SetWindowTextA", setWindowText(false));
    reg("SetWindowTextW", setWindowText(true));

    // --------------------------------------------------------------- timers
    reg("SetTimer", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        TimerInfo t;
        t.hwnd = apiArg(cpu, 0);
        t.id = apiArg(cpu, 1);
        t.intervalMs = static_cast<uint32_t>(apiArg(cpu, 2));
        if (t.intervalMs < 10) t.intervalMs = 10;
        t.callback = apiArg(cpu, 3);
        t.nextFireMs = nowMs() + t.intervalMs;
        emu.win32().timers().push_back(t);
        return t.id ? t.id : 1;
    });
    reg("KillTimer", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        uint64_t id = apiArg(cpu, 1);
        auto& v = emu.win32().timers();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (it->id == id) { v.erase(it); return 1; }
        return 0;
    });

    // ----------------------------------------------------- window long ptr
    auto getWindowLongPtr = [](Emulator& emu, Cpu& cpu) -> uint64_t {
        WindowObject* w = emu.win32().window(apiArg(cpu, 0));
        int32_t idx = static_cast<int32_t>(apiArg(cpu, 1));
        if (!w) return 0;
        if (idx >= 0 && static_cast<size_t>(idx / 8) < w->extra.size())
            return w->extra[static_cast<size_t>(idx / 8)];
        if (idx == -21) return w->extra.empty() ? 0 : w->extra.back(); // GWLP_USERDATA
        if (idx == -16) return w->style;
        if (idx == -6) return w->hInstance;
        return 0;
    };
    auto setWindowLongPtr = [](Emulator& emu, Cpu& cpu) -> uint64_t {
        WindowObject* w = emu.win32().window(apiArg(cpu, 0));
        int32_t idx = static_cast<int32_t>(apiArg(cpu, 1));
        uint64_t val = apiArg(cpu, 2);
        if (!w) return 0;
        if (idx >= 0 && static_cast<size_t>(idx / 8) < w->extra.size()) {
            uint64_t old = w->extra[static_cast<size_t>(idx / 8)];
            w->extra[static_cast<size_t>(idx / 8)] = val;
            return old;
        }
        if (idx == -21 && !w->extra.empty()) {
            uint64_t old = w->extra.back();
            w->extra.back() = val;
            return old;
        }
        return 0;
    };
    reg("GetWindowLongPtrA", getWindowLongPtr);
    reg("GetWindowLongPtrW", getWindowLongPtr);
    reg("GetWindowLongA", getWindowLongPtr);
    reg("GetWindowLongW", getWindowLongPtr);
    reg("SetWindowLongPtrA", setWindowLongPtr);
    reg("SetWindowLongPtrW", setWindowLongPtr);
    reg("SetWindowLongA", setWindowLongPtr);
    reg("SetWindowLongW", setWindowLongPtr);

    reg("GetSystemMetrics", [](Emulator&, Cpu& cpu) -> uint64_t {
        switch (static_cast<int32_t>(apiArg(cpu, 0))) {
            case 0: return 1920;  // SM_CXSCREEN
            case 1: return 1080;  // SM_CYSCREEN
            case 4: return 31;    // SM_CYCAPTION
            case 5: return 8;     // SM_CXBORDER
            case 6: return 8;
            case 7: return 8;     // SM_CXDLGFRAME
            case 8: return 8;
            case 15: return 19;   // SM_CYMENU
            case 32: return 8;    // SM_CXSIZEFRAME
            case 33: return 8;
            default: return 0;
        }
    });
    reg("GetSysColor", [](Emulator&, Cpu& cpu) -> uint64_t {
        switch (static_cast<int32_t>(apiArg(cpu, 0))) {
            case 15: return 0x00C0C0C0; // COLOR_3DFACE (BGR)
            case 5: return 0x00FFFFFF;  // COLOR_WINDOW
            case 8: return 0x00000000;  // COLOR_WINDOWTEXT
            default: return 0x00C0C0C0;
        }
    });
    reg("GetSysColorBrush", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        GdiObject o;
        o.kind = GdiObject::Kind::Brush;
        int idx = static_cast<int32_t>(apiArg(cpu, 0));
        o.color = (idx == 5) ? 0xFFFFFF : 0xC0C0C0;
        return emu.win32().createGdiObject(o);
    });
    reg("GetCursorPos", [](Emulator&, Cpu& cpu) -> uint64_t {
        uint64_t p = apiArg(cpu, 0);
        if (p) { cpu.mem.write32(p, 0); cpu.mem.write32(p + 4, 0); }
        return 1;
    });
    reg("GetKeyState", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("GetAsyncKeyState", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("SetCapture", [](Emulator&, Cpu&) -> uint64_t { return 0; });
    reg("ReleaseCapture", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("wsprintfA", [](Emulator&, Cpu& cpu) -> uint64_t {
        // Zeer beperkt: alleen het formaat zonder argumenten doorgeven.
        std::string fmt = cpu.mem.readCString(apiArg(cpu, 1));
        cpu.mem.writeCString(apiArg(cpu, 0), fmt);
        return fmt.size();
    });
}

} // namespace macemu
