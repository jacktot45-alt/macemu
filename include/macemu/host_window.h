// macemu - host_window.h
//
// Fase 4: de brug naar het scherm van de host. Twee backends:
//   * SDL2      - echt venster op macOS (Metal/OpenGL onder water)
//   * "null"    - headless; tekent in het geheugen en kan een .ppm dumpen
// De Win32-laag praat alleen met deze abstractie, nooit rechtstreeks met SDL.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace macemu {

// 32-bit ARGB framebuffer met een handvol tekenprimitieven. GDI-calls worden
// hier naartoe vertaald.
struct Framebuffer {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels; // 0x00RRGGBB

    void resize(int w, int h);
    void clear(uint32_t color);
    void setPixel(int x, int y, uint32_t color);
    uint32_t getPixel(int x, int y) const;
    void fillRect(int x, int y, int w, int h, uint32_t color);
    void frameRect(int x, int y, int w, int h, uint32_t color);
    void drawLine(int x0, int y0, int x1, int y1, uint32_t color);
    // Ingebouwd 8x8-font; scale >= 1. Geeft de breedte in pixels terug.
    int drawText(int x, int y, const std::string& text, uint32_t color, int scale = 1,
                 bool opaque = false, uint32_t bgColor = 0);
    static int textWidth(const std::string& text, int scale = 1);
    static int textHeight(int scale = 1);

    bool writePpm(const std::string& path) const;
};

struct HostEvent {
    enum class Type {
        None,
        Quit,
        MouseMove,
        MouseDown,
        MouseUp,
        KeyDown,
        KeyUp,
        Char,
        Resize,
    };
    Type type = Type::None;
    int x = 0, y = 0;
    int button = 0; // 1 = links, 2 = rechts, 3 = midden
    int key = 0;    // virtual-key code (Windows-stijl)
    int ch = 0;     // unicode codepoint bij Char
};

class HostWindow {
public:
    virtual ~HostWindow() = default;
    virtual void present(const Framebuffer& fb) = 0;
    virtual bool pollEvent(HostEvent& out) = 0;
    virtual void setTitle(const std::string& title) = 0;
    virtual bool isOpen() const = 0;
    virtual void close() = 0;
    virtual const char* backendName() const = 0;

    // Maakt een venster. headless=true forceert de null-backend.
    static std::unique_ptr<HostWindow> create(const std::string& title, int width, int height,
                                              bool headless);
    static bool sdlAvailable();
};

// Schrijft het laatst gepresenteerde frame van de headless backend weg als
// .ppm. Geeft false als er nooit een headless venster is gemaakt.
bool saveLastHeadlessFrame(const std::string& path);

} // namespace macemu
