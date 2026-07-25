// macemu - host-backends voor het venster (Fase 4)
//
// SDL2 is optioneel. Zonder SDL2 (of met --headless) draait alles door in de
// "null"-backend: er wordt gewoon in het geheugen getekend en je kunt het
// resultaat als .ppm wegschrijven. Handig voor tests en CI.
#include "macemu/host_window.h"

#include <cstdio>

#include "macemu/util.h"

#if MACEMU_HAVE_SDL2
#include <SDL.h>
#endif

namespace macemu {

namespace {

// ---------------------------------------------------------------- null
class NullWindow : public HostWindow {
public:
    NullWindow(const std::string& title, int w, int h) : title_(title) {
        last_.resize(w, h);
        MACEMU_LOG_INFO("headless venster \"%s\" (%dx%d) - geen scherm, wel tekenen",
                        title.c_str(), w, h);
    }
    void present(const Framebuffer& fb) override {
        last_ = fb;
        ++frames_;
    }
    bool pollEvent(HostEvent&) override { return false; }
    void setTitle(const std::string& t) override { title_ = t; }
    bool isOpen() const override { return open_; }
    void close() override { open_ = false; }
    const char* backendName() const override { return "null (headless)"; }

    const Framebuffer& lastFrame() const { return last_; }
    uint64_t frames() const { return frames_; }

private:
    std::string title_;
    Framebuffer last_;
    bool open_ = true;
    uint64_t frames_ = 0;
};

NullWindow* g_lastNullWindow = nullptr;

#if MACEMU_HAVE_SDL2
int sdlKeyToVk(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (k - SDLK_0);
    switch (k) {
        case SDLK_LEFT: return 0x25;
        case SDLK_UP: return 0x26;
        case SDLK_RIGHT: return 0x27;
        case SDLK_DOWN: return 0x28;
        case SDLK_SPACE: return 0x20;
        case SDLK_RETURN: return 0x0D;
        case SDLK_ESCAPE: return 0x1B;
        case SDLK_F1: return 0x70;
        case SDLK_F2: return 0x71;
        default: return 0;
    }
}

class SdlWindow : public HostWindow {
public:
    SdlWindow(const std::string& title, int w, int h) : width_(w), height_(h) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
            throw EmuError(std::string("SDL_Init mislukt: ") + SDL_GetError());
        window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   w, h, SDL_WINDOW_SHOWN);
        if (!window_) throw EmuError(std::string("SDL_CreateWindow mislukt: ") + SDL_GetError());
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
    }
    ~SdlWindow() override {
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    void present(const Framebuffer& fb) override {
        if (!texture_ || fb.pixels.empty()) return;
        if (fb.width != width_ || fb.height != height_) {
            SDL_DestroyTexture(texture_);
            width_ = fb.width;
            height_ = fb.height;
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, width_, height_);
            SDL_SetWindowSize(window_, width_, height_);
        }
        SDL_UpdateTexture(texture_, nullptr, fb.pixels.data(),
                          static_cast<int>(fb.width * sizeof(uint32_t)));
        SDL_RenderClear(renderer_);
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
    }

    bool pollEvent(HostEvent& out) override {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    open_ = false;
                    out.type = HostEvent::Type::Quit;
                    return true;
                case SDL_MOUSEMOTION:
                    out.type = HostEvent::Type::MouseMove;
                    out.x = e.motion.x;
                    out.y = e.motion.y;
                    return true;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                    out.type = (e.type == SDL_MOUSEBUTTONDOWN) ? HostEvent::Type::MouseDown
                                                               : HostEvent::Type::MouseUp;
                    out.x = e.button.x;
                    out.y = e.button.y;
                    out.button = (e.button.button == SDL_BUTTON_RIGHT) ? 2 : 1;
                    return true;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    int vk = sdlKeyToVk(e.key.keysym.sym);
                    if (!vk) continue;
                    out.type = (e.type == SDL_KEYDOWN) ? HostEvent::Type::KeyDown
                                                       : HostEvent::Type::KeyUp;
                    out.key = vk;
                    return true;
                }
                case SDL_TEXTINPUT:
                    out.type = HostEvent::Type::Char;
                    out.ch = static_cast<unsigned char>(e.text.text[0]);
                    return true;
                default:
                    break;
            }
        }
        return false;
    }

    void setTitle(const std::string& t) override {
        if (window_) SDL_SetWindowTitle(window_, t.c_str());
    }
    bool isOpen() const override { return open_; }
    void close() override { open_ = false; }
    const char* backendName() const override { return "SDL2"; }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int width_, height_;
    bool open_ = true;
};
#endif // MACEMU_HAVE_SDL2

} // namespace

bool HostWindow::sdlAvailable() {
#if MACEMU_HAVE_SDL2
    return true;
#else
    return false;
#endif
}

std::unique_ptr<HostWindow> HostWindow::create(const std::string& title, int width, int height,
                                               bool headless) {
#if MACEMU_HAVE_SDL2
    if (!headless) {
        try {
            return std::unique_ptr<HostWindow>(new SdlWindow(title, width, height));
        } catch (const std::exception& e) {
            MACEMU_LOG_WARN("SDL2-venster mislukt (%s), terugvallen op headless", e.what());
        }
    }
#else
    if (!headless)
        MACEMU_LOG_WARN("zonder SDL2 gebouwd: er verschijnt geen venster. Installeer SDL2 "
                        "(brew install sdl2) en bouw opnieuw voor een echt venster.");
#endif
    auto w = std::unique_ptr<NullWindow>(new NullWindow(title, width, height));
    g_lastNullWindow = w.get();
    return std::unique_ptr<HostWindow>(w.release());
}

// Wordt gebruikt door --screenshot in headless modus.
bool saveLastHeadlessFrame(const std::string& path) {
    if (!g_lastNullWindow) return false;
    return g_lastNullWindow->lastFrame().writePpm(path);
}

} // namespace macemu
