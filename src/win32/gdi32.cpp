// macemu - gdi32-stubs (Fase 4)
//
// GDI-tekenopdrachten worden vertaald naar operaties op een software-
// framebuffer (Framebuffer). Die framebuffer gaat daarna in één keer naar het
// scherm via de host-backend (SDL2 of headless).
//
// Let op de kleurvolgorde: Win32 COLORREF is 0x00BBGGRR, onze framebuffer
// gebruikt 0x00RRGGBB. Dat omdraaien is de meest gemaakte fout in deze laag.
#include "macemu/emulator.h"
#include "macemu/win32.h"

namespace macemu {

namespace {

// COLORREF (0x00BBGGRR) -> framebuffer (0x00RRGGBB)
uint32_t fromColorRef(uint64_t cr) {
    uint32_t v = static_cast<uint32_t>(cr);
    return ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
}

uint32_t toColorRef(uint32_t rgb) {
    return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

} // namespace

void Win32::registerGdi32() {
    auto reg = [this](const char* name, ApiHandler fn) {
        registerApi("gdi32.dll", name, std::move(fn));
    };

    reg("GetStockObject", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        return emu.win32().stockObject(static_cast<int>(apiArg(cpu, 0)));
    });

    reg("CreateSolidBrush", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        GdiObject o;
        o.kind = GdiObject::Kind::Brush;
        o.color = fromColorRef(apiArg(cpu, 0));
        return emu.win32().createGdiObject(o);
    });
    reg("CreatePen", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        GdiObject o;
        o.kind = GdiObject::Kind::Pen;
        o.style = static_cast<int>(apiArg(cpu, 0));
        o.width = static_cast<int>(apiArg(cpu, 1));
        if (o.width < 1) o.width = 1;
        o.color = fromColorRef(apiArg(cpu, 2));
        return emu.win32().createGdiObject(o);
    });
    reg("CreateFontA", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        GdiObject o;
        o.kind = GdiObject::Kind::Font;
        o.height = static_cast<int>(static_cast<int32_t>(apiArg(cpu, 0)));
        if (o.height < 0) o.height = -o.height;
        if (o.height <= 0) o.height = 8;
        return emu.win32().createGdiObject(o);
    });
    reg("CreateFontW", registry_["gdi32.dll!CreateFontA"]);
    reg("CreateFontIndirectA", [](Emulator& emu, Cpu&) -> uint64_t {
        GdiObject o;
        o.kind = GdiObject::Kind::Font;
        return emu.win32().createGdiObject(o);
    });
    reg("CreateFontIndirectW", registry_["gdi32.dll!CreateFontIndirectA"]);
    reg("DeleteObject", [](Emulator&, Cpu&) -> uint64_t { return 1; });

    reg("SelectObject", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
        uint64_t h = apiArg(cpu, 1);
        if (!d) return 0;
        GdiObject* o = emu.win32().gdiObject(h);
        if (!o) return 0;
        uint64_t old = 0;
        switch (o->kind) {
            case GdiObject::Kind::Brush: old = d->currentBrush; d->currentBrush = h; break;
            case GdiObject::Kind::Pen: old = d->currentPen; d->currentPen = h; break;
            case GdiObject::Kind::Font: old = d->currentFont; d->currentFont = h; break;
            default: break;
        }
        return old;
    });

    reg("SetTextColor", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
        if (!d) return 0;
        uint32_t old = d->textColor;
        d->textColor = fromColorRef(apiArg(cpu, 1));
        return toColorRef(old);
    });
    reg("SetBkColor", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
        if (!d) return 0;
        uint32_t old = d->bkColor;
        d->bkColor = fromColorRef(apiArg(cpu, 1));
        return toColorRef(old);
    });
    reg("SetBkMode", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
        if (!d) return 0;
        int old = d->bkMode;
        d->bkMode = static_cast<int>(apiArg(cpu, 1));
        return static_cast<uint64_t>(old);
    });

    reg("SetPixel", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
        if (!d || !d->target) return 0;
        uint32_t c = fromColorRef(apiArg(cpu, 3));
        d->target->setPixel(static_cast<int>(apiArg(cpu, 1)), static_cast<int>(apiArg(cpu, 2)), c);
        return apiArg(cpu, 3);
    });
    reg("GetPixel", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
        if (!d || !d->target) return 0xFFFFFFFF;
        return toColorRef(d->target->getPixel(static_cast<int>(apiArg(cpu, 1)),
                                              static_cast<int>(apiArg(cpu, 2))));
    });

    reg("Rectangle", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
        if (!d || !d->target) return 0;
        int l = static_cast<int32_t>(apiArg(cpu, 1));
        int t = static_cast<int32_t>(apiArg(cpu, 2));
        int r = static_cast<int32_t>(apiArg(cpu, 3));
        int b = static_cast<int32_t>(apiArg(cpu, 4));
        GdiObject* brush = emu.win32().gdiObject(d->currentBrush);
        if (brush && brush->style != 1) d->target->fillRect(l, t, r - l, b - t, brush->color);
        GdiObject* pen = emu.win32().gdiObject(d->currentPen);
        if (pen && pen->style != 5) d->target->frameRect(l, t, r - l, b - t, pen->color);
        return 1;
    });

    reg("Ellipse", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
        if (!d || !d->target) return 0;
        int l = static_cast<int32_t>(apiArg(cpu, 1));
        int t = static_cast<int32_t>(apiArg(cpu, 2));
        int r = static_cast<int32_t>(apiArg(cpu, 3));
        int b = static_cast<int32_t>(apiArg(cpu, 4));
        double cx = (l + r) / 2.0, cy = (t + b) / 2.0;
        double rx = (r - l) / 2.0, ry = (b - t) / 2.0;
        if (rx < 1 || ry < 1) return 1;
        GdiObject* brush = emu.win32().gdiObject(d->currentBrush);
        GdiObject* pen = emu.win32().gdiObject(d->currentPen);
        for (int y = t; y < b; ++y) {
            for (int x = l; x < r; ++x) {
                double dx = (x + 0.5 - cx) / rx, dy = (y + 0.5 - cy) / ry;
                double v = dx * dx + dy * dy;
                if (v <= 1.0) {
                    if (brush && brush->style != 1) d->target->setPixel(x, y, brush->color);
                } else if (v <= 1.15 && pen && pen->style != 5) {
                    d->target->setPixel(x, y, pen->color);
                }
            }
        }
        return 1;
    });

    reg("MoveToEx", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
        if (!d) return 0;
        uint64_t oldPt = apiArg(cpu, 3);
        if (oldPt) {
            cpu.mem.write32(oldPt, static_cast<uint32_t>(d->curX));
            cpu.mem.write32(oldPt + 4, static_cast<uint32_t>(d->curY));
        }
        d->curX = static_cast<int32_t>(apiArg(cpu, 1));
        d->curY = static_cast<int32_t>(apiArg(cpu, 2));
        return 1;
    });
    reg("LineTo", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
        if (!d || !d->target) return 0;
        int x = static_cast<int32_t>(apiArg(cpu, 1));
        int y = static_cast<int32_t>(apiArg(cpu, 2));
        uint32_t color = 0;
        if (GdiObject* pen = emu.win32().gdiObject(d->currentPen)) color = pen->color;
        d->target->drawLine(d->curX, d->curY, x, y, color);
        d->curX = x;
        d->curY = y;
        return 1;
    });

    auto textOut = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
            if (!d || !d->target) return 0;
            int x = static_cast<int32_t>(apiArg(cpu, 1));
            int y = static_cast<int32_t>(apiArg(cpu, 2));
            uint64_t strPtr = apiArg(cpu, 3);
            int32_t count = static_cast<int32_t>(apiArg(cpu, 4));
            std::string text;
            if (wide) {
                std::vector<uint16_t> w;
                for (int32_t i = 0; i < count; ++i) w.push_back(cpu.mem.read16(strPtr + i * 2));
                text = utf16ToUtf8(w);
            } else {
                std::vector<uint8_t> b(static_cast<size_t>(count < 0 ? 0 : count));
                if (!b.empty()) cpu.mem.readBlock(strPtr, b.data(), b.size());
                text.assign(b.begin(), b.end());
            }
            int scale = 1;
            if (GdiObject* f = emu.win32().gdiObject(d->currentFont))
                scale = f->height >= 24 ? 3 : (f->height >= 16 ? 2 : 1);
            d->target->drawText(x, y, text, d->textColor, scale, d->bkMode == 2, d->bkColor);
            return 1;
        };
    };
    reg("TextOutA", textOut(false));
    reg("TextOutW", textOut(true));

    auto extTextOut = [](bool wide) {
        return [wide](Emulator& emu, Cpu& cpu) -> uint64_t {
            DeviceContext* d = emu.win32().dc(apiArg(cpu, 0));
            if (!d || !d->target) return 0;
            int x = static_cast<int32_t>(apiArg(cpu, 1));
            int y = static_cast<int32_t>(apiArg(cpu, 2));
            uint64_t strPtr = apiArg(cpu, 5);
            int32_t count = static_cast<int32_t>(apiArg(cpu, 6));
            std::string text;
            if (wide) {
                std::vector<uint16_t> w;
                for (int32_t i = 0; i < count; ++i) w.push_back(cpu.mem.read16(strPtr + i * 2));
                text = utf16ToUtf8(w);
            } else {
                std::vector<uint8_t> b(static_cast<size_t>(count < 0 ? 0 : count));
                if (!b.empty()) cpu.mem.readBlock(strPtr, b.data(), b.size());
                text.assign(b.begin(), b.end());
            }
            d->target->drawText(x, y, text, d->textColor, 1, d->bkMode == 2, d->bkColor);
            return 1;
        };
    };
    reg("ExtTextOutA", extTextOut(false));
    reg("ExtTextOutW", extTextOut(true));

    reg("GetTextExtentPoint32A", [](Emulator&, Cpu& cpu) -> uint64_t {
        int32_t count = static_cast<int32_t>(apiArg(cpu, 2));
        uint64_t sizePtr = apiArg(cpu, 3);
        if (sizePtr) {
            cpu.mem.write32(sizePtr, static_cast<uint32_t>(count * 8));
            cpu.mem.write32(sizePtr + 4, 8);
        }
        return 1;
    });
    reg("GetTextExtentPoint32W", registry_["gdi32.dll!GetTextExtentPoint32A"]);

    reg("BitBlt", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("StretchBlt", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("CreateCompatibleDC", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        DeviceContext* src = emu.win32().dc(apiArg(cpu, 0));
        DeviceContext d;
        if (src) d = *src;
        return emu.win32().createDc(d);
    });
    reg("CreateCompatibleBitmap", [](Emulator& emu, Cpu&) -> uint64_t {
        GdiObject o;
        o.kind = GdiObject::Kind::Bitmap;
        return emu.win32().createGdiObject(o);
    });
    reg("DeleteDC", [](Emulator& emu, Cpu& cpu) -> uint64_t {
        emu.win32().destroyDc(apiArg(cpu, 0));
        return 1;
    });
    reg("SaveDC", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("RestoreDC", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("SetMapMode", [](Emulator&, Cpu&) -> uint64_t { return 1; });
    reg("GetDeviceCaps", [](Emulator&, Cpu& cpu) -> uint64_t {
        switch (static_cast<int32_t>(apiArg(cpu, 1))) {
            case 8: return 1920;  // HORZRES
            case 10: return 1080; // VERTRES
            case 12: return 32;   // BITSPIXEL
            case 14: return 1;    // PLANES
            case 88: case 90: return 96; // LOGPIXELSX/Y
            default: return 0;
        }
    });
}

} // namespace macemu
