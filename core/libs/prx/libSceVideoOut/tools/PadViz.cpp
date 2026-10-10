#define SDL_MAIN_HANDLED
#include "SDL.h"

#include "prx/libScePad/include/InputMapping.hpp"
#include "prx/libScePad/include/KeyboardMouse.hpp"
#include "prx/libSceVideoOut/tests/PadSimulation.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "stb_easy_font.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = Pad::KeyboardMouse::Clock;

struct Color {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

namespace Palette {
constexpr Color Background{0x0c, 0x0d, 0x10};
constexpr Color Surface{0x15, 0x17, 0x1b};
constexpr Color Raised{0x21, 0x24, 0x29};
constexpr Color Gate{0x6a, 0x6f, 0x76};
constexpr Color TextSecondary{0xa0, 0xa5, 0xac};
constexpr Color TextPrimary{0xe9, 0xeb, 0xee};
constexpr Color Accent{0x59, 0xa0, 0xf9};
constexpr Color AccentText{0x08, 0x0e, 0x16};
constexpr Color Warning{0xee, 0xb1, 0x54};
}

constexpr double WindowWidth = 1120.0;
constexpr double WindowHeight = 740.0;
constexpr double Margin = 24.0;
constexpr double PanelGap = 16.0;
constexpr double PanelPadding = 16.0;
constexpr double PanelRadius = 12.0;
constexpr double TrailSeconds = 0.3;
constexpr double ChartSeconds = 3.0;
constexpr int SelftestFrames = 30;

class Canvas {
public:
    Canvas(double width, double height, double scale)
        : scale(scale),
          surface(SDL_CreateRGBSurfaceWithFormat(0, static_cast<int>(std::lround(width * scale)), static_cast<int>(std::lround(height * scale)), 32, SDL_PIXELFORMAT_ARGB8888)) {
        if (surface == nullptr) throw std::runtime_error(std::string("pad_viz: cannot create canvas: ") + SDL_GetError());
    }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    ~Canvas() { SDL_FreeSurface(surface); }

    SDL_Surface* Surface() const { return surface; }
    double Scale() const { return scale; }

    void Clear(Color color) {
        SDL_FillRect(surface, nullptr, SDL_MapRGB(surface->format, color.r, color.g, color.b));
    }

    void Rect(double x, double y, double width, double height, double radius, Color color, double alpha = 1.0) {
        const double cx = (x + width / 2.0) * scale;
        const double cy = (y + height / 2.0) * scale;
        const double halfWidth = width * scale / 2.0;
        const double halfHeight = height * scale / 2.0;
        const double corner = std::min(radius * scale, std::min(halfWidth, halfHeight));
        paint(x * scale, y * scale, (x + width) * scale, (y + height) * scale, color, alpha, [=](double px, double py) {
            const double qx = std::abs(px - cx) - (halfWidth - corner);
            const double qy = std::abs(py - cy) - (halfHeight - corner);
            const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - corner;
            return std::clamp(0.5 - outside, 0.0, 1.0);
        });
    }

    void Disc(double x, double y, double radius, Color color, double alpha = 1.0) {
        const double cx = x * scale;
        const double cy = y * scale;
        const double r = radius * scale;
        paint(cx - r - 1.0, cy - r - 1.0, cx + r + 1.0, cy + r + 1.0, color, alpha, [=](double px, double py) {
            return std::clamp(0.5 - (std::hypot(px - cx, py - cy) - r), 0.0, 1.0);
        });
    }

    void Ring(double x, double y, double radius, double width, Color color, double alpha = 1.0, double dash = 0.0) {
        const double cx = x * scale;
        const double cy = y * scale;
        const double r = radius * scale;
        const double half = width * scale / 2.0;
        const double dashLength = dash * scale;
        paint(cx - r - half - 1.0, cy - r - half - 1.0, cx + r + half + 1.0, cy + r + half + 1.0, color, alpha, [=](double px, double py) {
            if (dashLength > 0.0) {
                const double along = (std::atan2(py - cy, px - cx) + 3.14159265358979323846) * r;
                if (std::fmod(along, dashLength * 2.0) > dashLength) return 0.0;
            }
            return std::clamp(0.5 - (std::abs(std::hypot(px - cx, py - cy) - r) - half), 0.0, 1.0);
        });
    }

    void Line(double x0, double y0, double x1, double y1, double width, Color color, double alpha = 1.0) {
        const double ax = x0 * scale;
        const double ay = y0 * scale;
        const double bx = x1 * scale;
        const double by = y1 * scale;
        const double half = width * scale / 2.0;
        const double dx = bx - ax;
        const double dy = by - ay;
        const double length = dx * dx + dy * dy;
        paint(std::min(ax, bx) - half - 1.0, std::min(ay, by) - half - 1.0, std::max(ax, bx) + half + 1.0, std::max(ay, by) + half + 1.0, color, alpha,
            [=](double px, double py) {
                const double t = length > 0.0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / length, 0.0, 1.0) : 0.0;
                return std::clamp(0.5 - (std::hypot(px - (ax + t * dx), py - (ay + t * dy)) - half), 0.0, 1.0);
            });
    }

    void Coverage(int deviceX, int deviceY, Color color, double alpha) {
        blend(deviceX, deviceY, color, alpha);
    }

    std::size_t Count(Color color) const {
        std::size_t count = 0;
        for (int y = 0; y < surface->h; ++y) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(surface->pixels) + y * surface->pitch);
            for (int x = 0; x < surface->w; ++x) {
                if ((row[x] & 0xFFFFFFu) == (static_cast<std::uint32_t>(color.r) << 16 | static_cast<std::uint32_t>(color.g) << 8 | color.b)) ++count;
            }
        }
        return count;
    }

private:
    template<typename TCoverage>
    void paint(double left, double top, double right, double bottom, Color color, double alpha, TCoverage coverage) {
        const int x0 = std::max(0, static_cast<int>(std::floor(left)));
        const int y0 = std::max(0, static_cast<int>(std::floor(top)));
        const int x1 = std::min(surface->w, static_cast<int>(std::ceil(right)));
        const int y1 = std::min(surface->h, static_cast<int>(std::ceil(bottom)));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const double amount = coverage(x + 0.5, y + 0.5);
                if (amount > 0.0) blend(x, y, color, amount * alpha);
            }
        }
    }

    void blend(int x, int y, Color color, double alpha) {
        if (x < 0 || y < 0 || x >= surface->w || y >= surface->h || alpha <= 0.0) return;
        auto* pixel = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(surface->pixels) + y * surface->pitch) + x;
        const double keep = 1.0 - std::min(alpha, 1.0);
        const auto mix = [keep, alpha](std::uint32_t under, std::uint8_t over) {
            return static_cast<std::uint32_t>(std::lround(under * keep + over * std::min(alpha, 1.0)));
        };
        const std::uint32_t r = mix((*pixel >> 16) & 0xFF, color.r);
        const std::uint32_t g = mix((*pixel >> 8) & 0xFF, color.g);
        const std::uint32_t b = mix(*pixel & 0xFF, color.b);
        *pixel = 0xFF000000u | r << 16 | g << 8 | b;
    }

    double scale;
    SDL_Surface* surface;
};

std::vector<std::uint32_t> decodeUtf8(std::string_view text) {
    std::vector<std::uint32_t> codepoints;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        const int extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
        std::uint32_t codepoint = extra == 0 ? lead : lead & (0x3Fu >> extra);
        for (int part = 1; part <= extra && index + part < text.size(); ++part) {
            codepoint = codepoint << 6 | (static_cast<unsigned char>(text[index + part]) & 0x3Fu);
        }
        codepoints.push_back(codepoint);
        index += static_cast<std::size_t>(extra) + 1;
    }
    return codepoints;
}

class Font {
public:
    Font(std::shared_ptr<const std::vector<unsigned char>> data, double size, double scale, double tracking = 0.0)
        : data(std::move(data)), size(size), scale(scale), tracking(tracking) {
        if (this->data && stbtt_InitFont(&info, this->data->data(), stbtt_GetFontOffsetForIndex(this->data->data(), 0)) != 0) {
            pixelScale = stbtt_ScaleForMappingEmToPixels(&info, static_cast<float>(size * scale));
            int ascent = 0;
            int descent = 0;
            int gap = 0;
            stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
            ascentPixels = ascent * pixelScale;
            descentPixels = descent * pixelScale;
            loaded = true;
        }
    }

    double Width(std::string_view text) {
        if (!loaded) return stb_easy_font_width(const_cast<char*>(std::string(text).c_str())) * fallbackScale();
        double width = 0.0;
        const auto codepoints = decodeUtf8(text);
        for (std::size_t index = 0; index < codepoints.size(); ++index) {
            width += glyph(codepoints[index]).advance + (index + 1 < codepoints.size() ? tracking * scale : 0.0);
            if (index + 1 < codepoints.size()) width += stbtt_GetCodepointKernAdvance(&info, static_cast<int>(codepoints[index]), static_cast<int>(codepoints[index + 1])) * pixelScale;
        }
        return width / scale;
    }

    double Draw(Canvas& canvas, double x, double y, std::string_view text, Color color) {
        if (!loaded) return drawFallback(canvas, x, y, text, color);
        double penX = x * canvas.Scale();
        const double baseline = std::round(y * canvas.Scale() + (LineHeight() * canvas.Scale() - (ascentPixels - descentPixels)) / 2.0 + ascentPixels);
        const auto codepoints = decodeUtf8(text);
        for (std::size_t index = 0; index < codepoints.size(); ++index) {
            const auto& shape = glyph(codepoints[index]);
            const int left = static_cast<int>(std::lround(penX)) + shape.offsetX;
            const int top = static_cast<int>(baseline) + shape.offsetY;
            for (int row = 0; row < shape.height; ++row) {
                for (int column = 0; column < shape.width; ++column) {
                    const auto amount = shape.bitmap[static_cast<std::size_t>(row * shape.width + column)];
                    if (amount != 0) canvas.Coverage(left + column, top + row, color, amount / 255.0);
                }
            }
            penX += shape.advance + tracking * canvas.Scale();
            if (index + 1 < codepoints.size()) penX += stbtt_GetCodepointKernAdvance(&info, static_cast<int>(codepoints[index]), static_cast<int>(codepoints[index + 1])) * pixelScale;
        }
        return penX / canvas.Scale() - x;
    }

    double LineHeight() const { return size * 1.4; }

private:
    struct Glyph {
        int width = 0;
        int height = 0;
        int offsetX = 0;
        int offsetY = 0;
        double advance = 0.0;
        std::vector<unsigned char> bitmap;
    };

    const Glyph& glyph(std::uint32_t codepoint) {
        const auto found = glyphs.find(codepoint);
        if (found != glyphs.end()) return found->second;
        Glyph shape;
        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(&info, static_cast<int>(codepoint), &advance, &bearing);
        shape.advance = advance * pixelScale;
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetCodepointBitmapBox(&info, static_cast<int>(codepoint), pixelScale, pixelScale, &x0, &y0, &x1, &y1);
        shape.width = std::max(0, x1 - x0);
        shape.height = std::max(0, y1 - y0);
        shape.offsetX = x0;
        shape.offsetY = y0;
        shape.bitmap.resize(static_cast<std::size_t>(shape.width * shape.height));
        if (!shape.bitmap.empty()) stbtt_MakeCodepointBitmap(&info, shape.bitmap.data(), shape.width, shape.height, shape.width, pixelScale, pixelScale, static_cast<int>(codepoint));
        return glyphs.emplace(codepoint, std::move(shape)).first->second;
    }

    double fallbackScale() const { return size / 9.0; }

    double drawFallback(Canvas& canvas, double x, double y, std::string_view text, Color color) {
        std::string ascii;
        for (const auto codepoint : decodeUtf8(text)) ascii.push_back(codepoint == 0x2212 ? '-' : codepoint < 128 ? static_cast<char>(codepoint) : ' ');
        std::vector<float> vertices(ascii.size() * 128 + 64);
        const int quads = stb_easy_font_print(0.0f, 0.0f, ascii.data(), nullptr, vertices.data(), static_cast<int>(vertices.size() * sizeof(float)));
        const double factor = fallbackScale();
        const double top = y + (LineHeight() - 7.0 * factor) / 2.0;
        for (int quad = 0; quad < quads; ++quad) {
            const float* corner = vertices.data() + quad * 16;
            const double left = corner[0];
            const double upper = corner[1];
            const double right = corner[8];
            const double lower = corner[9];
            canvas.Rect(x + left * factor, top + upper * factor, (right - left) * factor, (lower - upper) * factor, 0.0, color);
        }
        return Width(text);
    }

    std::shared_ptr<const std::vector<unsigned char>> data;
    double size;
    double scale;
    double tracking;
    stbtt_fontinfo info{};
    float pixelScale = 1.0f;
    double ascentPixels = 0.0;
    double descentPixels = 0.0;
    bool loaded = false;
    std::map<std::uint32_t, Glyph> glyphs;
};

std::shared_ptr<const std::vector<unsigned char>> readFont(const std::vector<std::filesystem::path>& candidates) {
    for (const auto& path : candidates) {
        std::ifstream file(path, std::ios::binary);
        if (!file) continue;
        auto bytes = std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (!bytes->empty()) return bytes;
    }
    return nullptr;
}

std::vector<std::filesystem::path> fontCandidates(const char* windowsName, const char* dejaVuName) {
    std::vector<std::filesystem::path> candidates;
    if (const char* windows = std::getenv("WINDIR")) candidates.push_back(std::filesystem::path(windows) / "Fonts" / windowsName);
    for (const char* directory : {"/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/TTF", "/usr/share/fonts/dejavu", "/usr/local/share/fonts"}) {
        candidates.push_back(std::filesystem::path(directory) / dejaVuName);
    }
    return candidates;
}

struct Fonts {
    explicit Fonts(double scale)
        : regularData(readFont(fontCandidates("segoeui.ttf", "DejaVuSans.ttf"))),
          semiboldData(readFont(fontCandidates("seguisb.ttf", "DejaVuSans-Bold.ttf"))),
          monoData(readFont(fontCandidates("consola.ttf", "DejaVuSansMono.ttf"))),
          title(semiboldData ? semiboldData : regularData, 17.0, scale),
          heading(semiboldData ? semiboldData : regularData, 15.0, scale),
          body(regularData, 13.0, scale),
          mono(monoData ? monoData : regularData, 13.0, scale),
          button(semiboldData ? semiboldData : regularData, 12.0, scale),
          label(semiboldData ? semiboldData : regularData, 12.0, scale, 0.6) {}

    std::shared_ptr<const std::vector<unsigned char>> regularData;
    std::shared_ptr<const std::vector<unsigned char>> semiboldData;
    std::shared_ptr<const std::vector<unsigned char>> monoData;
    Font title;
    Font heading;
    Font body;
    Font mono;
    Font button;
    Font label;
};

double axisValue(std::uint8_t value) {
    return value < 128 ? (static_cast<double>(value) - 128.0) / 128.0 : (static_cast<double>(value) - 128.0) / 127.0;
}

std::string format(const char* pattern, double value) {
    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(), pattern, value);
    return buffer.data();
}

std::string signedValue(double value) {
    const double rounded = std::round(value * 1000.0) / 1000.0;
    return rounded < 0.0 ? format("%.3f", rounded) : " " + format("%.3f", std::abs(rounded));
}

std::string sourceName(const Pad::InputBinding& binding) {
    if (binding.key != SDL_SCANCODE_UNKNOWN) return SDL_GetScancodeName(binding.key);
    switch (binding.mouseButton) {
        case Pad::MouseButton::Left: return "Left click";
        case Pad::MouseButton::Middle: return "Middle click";
        case Pad::MouseButton::Right: return "Right click";
        case Pad::MouseButton::X1: return "Mouse X1";
        case Pad::MouseButton::X2: return "Mouse X2";
        case Pad::MouseButton::None: break;
    }
    if (binding.wheelDirection != 0) return binding.wheelDirection > 0 ? "Wheel up" : "Wheel down";
    return "unbound";
}

std::string boundTo(const Pad::InputConfiguration& configuration, Pad::InputControl control) {
    for (const auto& binding : configuration.bindings) {
        if (binding.control == control) return sourceName(binding);
    }
    return "unbound";
}

struct Options {
    std::filesystem::path directory;
    std::string title;
    std::filesystem::path config;
    std::filesystem::path record;
    std::filesystem::path screenshot;
    double gameDeadzone = 0.1;
    int frames = 0;
    bool selftest = false;
};

struct Snapshot {
    double seconds = 0.0;
    std::array<double, 4> axes{};
};

class Visualiser {
public:
    explicit Visualiser(Options options) : options(std::move(options)) {}

    int Run() {
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "pad_viz: SDL video is unavailable: %s\n", SDL_GetError());
            return options.selftest ? 77 : 1;
        }
        const int status = runWindow();
        SDL_Quit();
        return status;
    }

private:
    int runWindow() {
        const double scale = displayScale();
        window = SDL_CreateWindow("AnyPS5 pad visualiser", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            static_cast<int>(std::lround(WindowWidth * scale)), static_cast<int>(std::lround(WindowHeight * scale)), SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
        if (window == nullptr) {
            std::fprintf(stderr, "pad_viz: cannot open a window: %s\n", SDL_GetError());
            return options.selftest ? 77 : 1;
        }
        canvas = std::make_unique<Canvas>(WindowWidth, WindowHeight, scale);
        fonts = std::make_unique<Fonts>(scale);
        auto configuration = Pad::DefaultInputConfiguration();
        if (!options.selftest) {
            try {
                configuration = load();
            } catch (const std::exception& error) {
                showLoadError(error);
            }
        }
        keyboardMouse = std::make_unique<Pad::KeyboardMouse>(std::move(configuration));
        watched = signature();
        if (!options.record.empty()) {
            recording.open(options.record);
            if (!recording) throw std::runtime_error("pad_viz: cannot write " + options.record.string());
        }
        start = Clock::now();
        nextReloadCheck = start;
        keyboardMouse->Advance(start);
        int status = 0;
        for (int frame = 1; running; ++frame) {
            const auto frameStart = Clock::now();
            if (options.selftest) pushSelftestEvents(frame);
            pumpEvents();
            const auto now = Clock::now();
            if (keyboardMouse->MouseMode() && SDL_GetKeyboardFocus() == nullptr) releaseInput();
            keyboardMouse->Advance(now);
            state = keyboardMouse->Compose(PadInputState{});
            remember(now);
            if (!options.selftest && now >= nextReloadCheck) reloadIfChanged(now);
            draw(now);
            present();
            if (options.selftest && !checkSelftest(frame)) status = 1;
            if ((options.frames > 0 && frame >= options.frames) || status != 0) running = false;
            const auto spent = Clock::now() - frameStart;
            if (spent < std::chrono::milliseconds(8)) SDL_Delay(static_cast<Uint32>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::milliseconds(8) - spent).count()));
        }
        if (!options.screenshot.empty() && SDL_SaveBMP(canvas->Surface(), options.screenshot.string().c_str()) != 0) {
            std::fprintf(stderr, "pad_viz: cannot save %s: %s\n", options.screenshot.string().c_str(), SDL_GetError());
            status = 1;
        }
        if (keyboardMouse->MouseMode()) setMouseMode(false);
        SDL_DestroyWindow(window);
        return status;
    }

    double displayScale() const {
        float dpi = 0.0f;
        double scale = SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) == 0 && dpi > 0.0f ? dpi / 96.0 : 1.0;
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
            scale = std::min({scale, usable.w * 0.95 / WindowWidth, usable.h * 0.9 / WindowHeight});
        }
        return std::clamp(scale, 0.75, 3.0);
    }

    Pad::InputConfiguration load() const {
        if (!options.config.empty()) {
            std::ifstream file(options.config);
            if (!file) throw std::runtime_error("cannot read " + options.config.string());
            return Pad::ParseInputConfiguration(file, options.config);
        }
        return Pad::LoadInputConfiguration(options.directory, options.title);
    }

    std::vector<std::filesystem::path> watchedPaths() const {
        if (!options.config.empty()) return {options.config};
        const char* configured = std::getenv("ANYPS5_INPUT_CONFIG");
        std::vector<std::filesystem::path> paths{configured != nullptr && configured[0] != '\0' ? std::filesystem::path(configured) : options.directory / "anyps5-input.ini"};
        const auto profile = Pad::InputProfileName(options.title);
        if (!profile.empty()) paths.push_back(options.directory / profile);
        return paths;
    }

    std::string signature() const {
        std::string result;
        for (const auto& path : watchedPaths()) {
            std::error_code error;
            const auto time = std::filesystem::last_write_time(path, error);
            const auto size = error ? 0 : std::filesystem::file_size(path, error);
            result += path.string() + (error ? "|missing;" : "|" + std::to_string(time.time_since_epoch().count()) + "|" + std::to_string(size) + ";");
        }
        return result;
    }

    void reloadIfChanged(Clock::time_point now) {
        nextReloadCheck = now + std::chrono::milliseconds(500);
        const auto current = signature();
        if (current == watched) return;
        watched = current;
        try {
            const bool mouseMode = keyboardMouse->MouseMode();
            auto reloaded = std::make_unique<Pad::KeyboardMouse>(load());
            reloaded->Advance(now);
            reloaded->SetMouseMode(mouseMode);
            keyboardMouse = std::move(reloaded);
            reloadError.clear();
            reloadedAt = now;
        } catch (const std::exception& error) {
            showLoadError(error);
        }
    }

    void setMouseMode(bool enabled) {
        if (!options.selftest) {
            SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
            SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
            if (SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) != 0) {
                std::fprintf(stderr, "pad_viz: relative mouse mode failed: %s\n", SDL_GetError());
                return;
            }
        }
        keyboardMouse->SetMouseMode(enabled);
        mouseCounts.clear();
    }

    void releaseInput() {
        if (recording) {
            const double ms = elapsedMs(Clock::now());
            recording << std::fixed << ms << " release\n";
            if (keyboardMouse->MouseMode()) recording << std::fixed << ms << " mouse off\n";
            recording.flush();
        }
        keyboardMouse->Release();
        if (keyboardMouse->MouseMode()) setMouseMode(false);
    }

    double elapsedMs(Clock::time_point now) const {
        return std::chrono::duration<double, std::milli>(now - start).count();
    }

    void pumpEvents() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
                continue;
            }
            if (event.type == SDL_WINDOWEVENT && (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST || event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                releaseInput();
                continue;
            }
            const bool input = event.type == SDL_MOUSEWHEEL || event.type == SDL_MOUSEMOTION || event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
                event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP;
            if (!input) continue;
            const auto now = Clock::now();
            if (recording && (event.type != SDL_MOUSEMOTION || keyboardMouse->MouseMode()) && PadSimulation::WriteTraceLine(recording, elapsedMs(now), event)) recording.flush();
            if (event.type == SDL_MOUSEMOTION && keyboardMouse->MouseMode()) mouseCounts.push_back({now, std::hypot(event.motion.xrel, event.motion.yrel)});
            const auto requests = keyboardMouse->HandleEvent(event, now);
            if (requests.toggleMouse) setMouseMode(!keyboardMouse->MouseMode());
        }
    }

    void remember(Clock::time_point now) {
        const double seconds = std::chrono::duration<double>(now - start).count();
        Snapshot snapshot{seconds, {}};
        for (std::size_t axis = 0; axis < 4; ++axis) snapshot.axes[axis] = axisValue(state.sticks[axis]);
        history.push_back(snapshot);
        while (!history.empty() && history.front().seconds < seconds - ChartSeconds) history.pop_front();
        while (!mouseCounts.empty() && mouseCounts.front().first < now - std::chrono::milliseconds(200)) mouseCounts.pop_front();
    }

    double mouseSpeed() const {
        double counts = 0.0;
        for (const auto& [time, amount] : mouseCounts) counts += amount;
        return counts / 0.2;
    }

    void present() {
        SDL_Surface* target = SDL_GetWindowSurface(window);
        if (target == nullptr) return;
        SDL_BlitSurface(canvas->Surface(), nullptr, target, nullptr);
        SDL_UpdateWindowSurface(window);
    }

    void panel(double x, double y, double width, double height) {
        canvas->Rect(x, y, width, height, PanelRadius, Palette::Surface);
    }

    void draw(Clock::time_point now) {
        canvas->Clear(Palette::Background);
        const auto& configuration = keyboardMouse->Configuration();
        constexpr double StickWidth = 300.0;
        constexpr double Top = 72.0;
        constexpr double StickHeight = 456.0;
        constexpr double ButtonsHeight = 172.0;
        drawHeader(configuration);
        drawStick(Margin, Top, StickWidth, StickHeight, 0, "Left stick", keysFor(configuration, 0));
        drawStick(Margin + StickWidth + PanelGap, Top, StickWidth, StickHeight, 2, "Right stick",
            keyboardMouse->MouseMode() ? "Mouse, captured" : keysFor(configuration, 2) + ", or the mouse");
        drawButtons(Margin, Top + StickHeight + PanelGap, StickWidth * 2.0 + PanelGap, ButtonsHeight);
        const double settingsX = Margin + (StickWidth + PanelGap) * 2.0;
        drawSettings(settingsX, Top, WindowWidth - Margin - settingsX, StickHeight + PanelGap + ButtonsHeight, configuration, now);
    }

    std::string keysFor(const Pad::InputConfiguration& configuration, int axis) const {
        const auto first = axis == 0 ? Pad::InputControl::LeftStickUp : Pad::InputControl::RightStickUp;
        const std::array controls{first, static_cast<Pad::InputControl>(static_cast<int>(first) - 2), static_cast<Pad::InputControl>(static_cast<int>(first) + 1),
            static_cast<Pad::InputControl>(static_cast<int>(first) - 1)};
        std::string keys = "Keys";
        for (const auto control : controls) keys += " " + boundTo(configuration, control);
        return keys;
    }

    void drawHeader(const Pad::InputConfiguration& configuration) {
        fonts->title.Draw(*canvas, Margin, 22.0, "Pad visualiser", Palette::TextPrimary);
        const bool captured = keyboardMouse->MouseMode();
        const std::string toggle = boundTo(configuration, Pad::InputControl::ToggleMouse);
        const std::string text = captured ? "Mouse drives the right stick. " + toggle + " releases it." : toggle + " captures the mouse for the right stick.";
        const double textX = WindowWidth - Margin - fonts->body.Width(text);
        canvas->Disc(textX - 8.0 - 4.0, 22.0 + fonts->title.LineHeight() / 2.0, 4.0, captured ? Palette::Accent : Palette::Gate);
        fonts->body.Draw(*canvas, textX, 22.0 + (fonts->title.LineHeight() - fonts->body.LineHeight()) / 2.0, text, captured ? Palette::TextPrimary : Palette::TextSecondary);
    }

    void drawStick(double x, double y, double width, double height, std::size_t axis, const char* name, const std::string& keys) {
        panel(x, y, width, height);
        const double inner = x + PanelPadding;
        fonts->heading.Draw(*canvas, inner, y + PanelPadding, name, Palette::TextPrimary);
        fonts->body.Draw(*canvas, inner, y + PanelPadding + 22.0, keys, Palette::TextSecondary);

        constexpr double Radius = 100.0;
        const double centerX = x + width / 2.0;
        const double centerY = y + 66.0 + Radius;
        const auto& settings = keyboardMouse->Configuration().settings;
        const bool rightStick = axis == 2;
        const bool mouseStick = rightStick && keyboardMouse->MouseMode();
        canvas->Disc(centerX, centerY, Radius, Palette::Background);
        canvas->Line(centerX - Radius, centerY, centerX + Radius, centerY, 1.0, Palette::Raised);
        canvas->Line(centerX, centerY - Radius, centerX, centerY + Radius, 1.0, Palette::Raised);
        canvas->Disc(centerX, centerY, Radius * options.gameDeadzone, Palette::Raised);
        canvas->Ring(centerX, centerY, Radius * options.gameDeadzone, 1.0, Palette::Gate);
        canvas->Ring(centerX, centerY, Radius, 1.5, Palette::Gate);
        if (rightStick) canvas->Ring(centerX, centerY, Radius * settings.mouseDeadzoneOffset, 1.0, Palette::Gate, mouseStick ? 1.0 : 0.5, 3.0);

        const double valueX = axisValue(state.sticks[axis]);
        const double valueY = axisValue(state.sticks[axis + 1]);
        if (std::hypot(valueX, valueY) > 1.001) {
            for (const auto& [fromX, fromY, toX, toY] : std::array<std::array<double, 4>, 4>{{{-1, -1, 1, -1}, {1, -1, 1, 1}, {1, 1, -1, 1}, {-1, 1, -1, -1}}}) {
                for (double step = 0.0; step < 1.0; step += 0.05) {
                    canvas->Line(centerX + (fromX + (toX - fromX) * step) * Radius, centerY + (fromY + (toY - fromY) * step) * Radius,
                        centerX + (fromX + (toX - fromX) * (step + 0.025)) * Radius, centerY + (fromY + (toY - fromY) * (step + 0.025)) * Radius, 1.0, Palette::Gate);
                }
            }
        }

        const double seconds = history.empty() ? 0.0 : history.back().seconds;
        for (const auto& snapshot : history) {
            const double age = seconds - snapshot.seconds;
            if (age > TrailSeconds) continue;
            canvas->Disc(centerX + snapshot.axes[axis] * Radius, centerY + snapshot.axes[axis + 1] * Radius, 2.5, Palette::Accent, 0.45 * (1.0 - age / TrailSeconds));
        }
        const double pointX = centerX + valueX * Radius;
        const double pointY = centerY + valueY * Radius;
        if (valueX != 0.0 || valueY != 0.0) canvas->Line(centerX, centerY, pointX, pointY, 2.0, Palette::Accent, 0.6);
        canvas->Disc(pointX, pointY, 7.0, Palette::Background);
        canvas->Disc(pointX, pointY, 5.5, Palette::Accent);

        const double readoutY = centerY + Radius + 14.0;
        const double valueColumn = inner + 28.0;
        const double byteColumn = x + width - PanelPadding;
        const std::array<const char*, 2> labels{"X", "Y"};
        for (std::size_t row = 0; row < 2; ++row) {
            const double rowY = readoutY + row * 20.0;
            const std::uint8_t byte = state.sticks[axis + row];
            fonts->body.Draw(*canvas, inner, rowY, labels[row], Palette::TextSecondary);
            fonts->mono.Draw(*canvas, valueColumn, rowY, signedValue(axisValue(byte)), Palette::TextPrimary);
            const std::string raw = std::to_string(byte);
            const double rawWidth = fonts->mono.Width(raw);
            fonts->mono.Draw(*canvas, byteColumn - rawWidth, rowY, raw, Palette::TextPrimary);
            fonts->body.Draw(*canvas, byteColumn - rawWidth - 8.0 - fonts->body.Width("byte"), rowY, "byte", Palette::TextSecondary);
        }

        const double chartY = readoutY + 2.0 * 20.0 + 8.0;
        drawChart(inner, chartY, width - PanelPadding * 2.0, 48.0, axis, rightStick, mouseStick);
        double legendY = chartY + 48.0 + 4.0;
        fonts->body.Draw(*canvas, inner, legendY, "Tilt over the last 3 s", Palette::TextSecondary);
        legendY += 20.0;
        fonts->body.Draw(*canvas, inner, legendY, "Shaded: game deadzone " + format("%.2f", options.gameDeadzone), Palette::TextSecondary);
        if (rightStick) fonts->body.Draw(*canvas, inner, legendY + 18.0, "Dashed: smallest mouse tilt " + format("%.2f", settings.mouseDeadzoneOffset), Palette::TextSecondary);
    }

    void drawChart(double x, double y, double width, double height, std::size_t axis, bool rightStick, bool mouseStick) {
        canvas->Rect(x, y, width, height, 6.0, Palette::Background);
        const double deadzoneHeight = height * options.gameDeadzone;
        canvas->Rect(x, y + height - deadzoneHeight, width, deadzoneHeight, 0.0, Palette::Raised);
        canvas->Line(x, y + height - deadzoneHeight, x + width, y + height - deadzoneHeight, 1.0, Palette::Gate);
        if (rightStick) {
            const double offsetY = y + height - height * keyboardMouse->Configuration().settings.mouseDeadzoneOffset;
            for (double dashX = x; dashX < x + width; dashX += 8.0) {
                canvas->Line(dashX, offsetY, std::min(dashX + 4.0, x + width), offsetY, 1.0, Palette::Gate, mouseStick ? 1.0 : 0.5);
            }
        }
        if (history.size() >= 2) {
            const double newest = history.back().seconds;
            const auto point = [&](const Snapshot& snapshot) {
                const double magnitude = std::min(1.0, std::hypot(snapshot.axes[axis], snapshot.axes[axis + 1]));
                return std::array<double, 2>{x + width - (newest - snapshot.seconds) / ChartSeconds * width, y + height - magnitude * (height - 2.0) - 1.0};
            };
            for (std::size_t index = 1; index < history.size(); ++index) {
                const auto from = point(history[index - 1]);
                const auto to = point(history[index]);
                canvas->Line(from[0], from[1], to[0], to[1], 1.5, Palette::Accent);
            }
        }
    }

    void drawButtons(double x, double y, double width, double height) {
        panel(x, y, width, height);
        const double inner = x + PanelPadding;
        const double contentY = y + PanelPadding + 26.0;
        constexpr double BarWidth = 168.0;
        groupLabel(inner, y + PanelPadding, "TRIGGERS");
        const std::array<std::pair<const char*, std::uint8_t>, 2> triggers{{{"L2", state.analogButtonsL2}, {"R2", state.analogButtonsR2}}};
        for (std::size_t index = 0; index < triggers.size(); ++index) {
            const double rowY = contentY + index * 48.0;
            fonts->body.Draw(*canvas, inner, rowY, triggers[index].first, Palette::TextSecondary);
            const std::string value = std::to_string(triggers[index].second);
            fonts->mono.Draw(*canvas, inner + BarWidth - fonts->mono.Width(value), rowY, value, Palette::TextPrimary);
            canvas->Rect(inner, rowY + 24.0, BarWidth, 8.0, 4.0, Palette::Background);
            if (triggers[index].second > 0) canvas->Rect(inner, rowY + 24.0, BarWidth * triggers[index].second / 255.0, 8.0, 4.0, Palette::Accent);
        }

        struct Indicator {
            const char* label;
            Pad::PadButton button;
        };
        constexpr std::array<std::array<Indicator, 4>, 4> rows{{
            {{{"Cross", Pad::PadButton::Cross}, {"Circle", Pad::PadButton::Circle}, {"Square", Pad::PadButton::Square}, {"Triangle", Pad::PadButton::Triangle}}},
            {{{"L1", Pad::PadButton::L1}, {"R1", Pad::PadButton::R1}, {"L3", Pad::PadButton::L3}, {"R3", Pad::PadButton::R3}}},
            {{{"Up", Pad::PadButton::Up}, {"Down", Pad::PadButton::Down}, {"Left", Pad::PadButton::Left}, {"Right", Pad::PadButton::Right}}},
            {{{"Options", Pad::PadButton::Options}, {"Touch pad", Pad::PadButton::TouchPad}, {nullptr, Pad::PadButton::None}, {nullptr, Pad::PadButton::None}}},
        }};
        constexpr double CellGap = 8.0;
        constexpr double RowGap = 6.0;
        constexpr double CellHeight = 24.0;
        const double gridX = inner + BarWidth + 32.0;
        groupLabel(gridX, y + PanelPadding, "BUTTONS");
        const double cellWidth = (x + width - PanelPadding - gridX - CellGap * 3.0) / 4.0;
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (std::size_t column = 0; column < rows[row].size(); ++column) {
                const auto& indicator = rows[row][column];
                if (indicator.label == nullptr) continue;
                const double cellX = gridX + static_cast<double>(column) * (cellWidth + CellGap);
                const double cellY = contentY + static_cast<double>(row) * (CellHeight + RowGap);
                const bool touch = indicator.button == Pad::PadButton::TouchPad && (state.touchLeft || state.touchRight);
                const bool down = touch || (state.buttons & static_cast<std::uint32_t>(indicator.button)) != 0;
                canvas->Rect(cellX, cellY, cellWidth, CellHeight, 6.0, down ? Palette::Accent : Palette::Raised);
                const double labelWidth = fonts->button.Width(indicator.label);
                fonts->button.Draw(*canvas, cellX + (cellWidth - labelWidth) / 2.0, cellY + (CellHeight - fonts->button.LineHeight()) / 2.0, indicator.label,
                    down ? Palette::AccentText : Palette::TextSecondary);
            }
        }
    }

    void settingRow(double x, double y, double width, const std::string& label, const std::string& value) {
        fonts->body.Draw(*canvas, x, y, label, Palette::TextSecondary);
        fonts->mono.Draw(*canvas, x + width - fonts->mono.Width(value), y, value, Palette::TextPrimary);
    }

    void groupLabel(double x, double y, const char* label) {
        fonts->label.Draw(*canvas, x, y, label, Palette::TextSecondary);
    }

    void drawSettings(double x, double y, double width, double height, const Pad::InputConfiguration& configuration, Clock::time_point now) {
        panel(x, y, width, height);
        const double inner = x + PanelPadding;
        const double innerWidth = width - PanelPadding * 2.0;
        constexpr double Row = 21.0;
        fonts->heading.Draw(*canvas, inner, y + PanelPadding, "Settings", Palette::TextPrimary);
        double rowY = y + PanelPadding + 26.0;
        if (configuration.sources.empty()) {
            fonts->body.Draw(*canvas, inner, rowY, reloadError.empty() ? "Built-in defaults: no anyps5-input.ini" : "Built-in defaults", Palette::TextSecondary);
            rowY += 20.0;
        }
        for (const auto& source : configuration.sources) {
            fonts->body.Draw(*canvas, inner, rowY, source.filename().string(), Palette::TextSecondary);
            rowY += 20.0;
        }

        const auto& settings = configuration.settings;
        rowY += 14.0;
        groupLabel(inner, rowY, "MOUSE");
        rowY += 22.0;
        settingRow(inner, rowY, innerWidth, "Sensitivity", format("%.2f", settings.mouseSensitivity));
        settingRow(inner, rowY += Row, innerWidth, "Vertical scale", format("%.2f", settings.mouseVerticalScale));
        settingRow(inner, rowY += Row, innerWidth, "Curve", format("%.2f", settings.mouseCurve));
        settingRow(inner, rowY += Row, innerWidth, "Deadzone offset", format("%.2f", settings.mouseDeadzoneOffset));
        settingRow(inner, rowY += Row, innerWidth, "Max speed", format("%.2f", settings.mouseMaxSpeed));
        settingRow(inner, rowY += Row, innerWidth, "Smoothing", format("%.0f ms", settings.mouseSmoothingMs));
        settingRow(inner, rowY += Row, innerWidth, "Decay", format("%.0f ms", settings.mouseDecayMs));
        const double fullTilt = Pad::MouseFullTiltCountsPerSecond / settings.mouseSensitivity;
        settingRow(inner, rowY += Row, innerWidth, "Full tilt, horizontal", format("%.0f counts/s", fullTilt));
        settingRow(inner, rowY += Row, innerWidth, "Full tilt, vertical", format("%.0f counts/s", fullTilt / settings.mouseVerticalScale));

        rowY += Row + 14.0;
        groupLabel(inner, rowY, "KEYBOARD");
        rowY += 22.0;
        settingRow(inner, rowY, innerWidth, "Modifier (" + boundTo(configuration, Pad::InputControl::StickModifier) + ")", format("%.2f", settings.stickModifierScale));
        settingRow(inner, rowY += Row, innerWidth, "Ramp", format("%.0f ms", settings.keyboardRampMs));

        rowY += Row + 14.0;
        groupLabel(inner, rowY, "LIVE");
        rowY += 22.0;
        const bool captured = keyboardMouse->MouseMode();
        settingRow(inner, rowY, innerWidth, "Mouse speed", captured ? format("%.0f counts/s", mouseSpeed()) : std::string("not captured"));
        settingRow(inner, rowY += Row, innerWidth, "Tracked speed", captured ? format("%.2f × full tilt", keyboardMouse->MouseSpeed()) : std::string("not captured"));

        double footerY = y + height - PanelPadding - 2.0 * 20.0;
        if (!reloadError.empty()) {
            const auto lines = wrap(reloadError, innerWidth, 3);
            double lineY = footerY - 20.0 * static_cast<double>(lines.size() + 1);
            canvas->Disc(inner + 4.0, lineY + fonts->body.LineHeight() / 2.0, 4.0, Palette::Warning);
            fonts->body.Draw(*canvas, inner + 14.0, lineY, "Settings not applied:", Palette::Warning);
            for (const auto& line : lines) fonts->body.Draw(*canvas, inner, lineY += 20.0, line, Palette::Warning);
        }
        const bool fresh = reloadedAt != Clock::time_point{} && now - reloadedAt < std::chrono::seconds(2);
        fonts->body.Draw(*canvas, inner, footerY, fresh ? "Reloaded." : "Save an .ini file to reload it here.", fresh ? Palette::TextPrimary : Palette::TextSecondary);
        fonts->body.Draw(*canvas, inner, footerY + 20.0, "Close the window to quit.", Palette::TextSecondary);
    }

    std::vector<std::string> wrap(const std::string& text, double width, std::size_t maximumLines) {
        std::vector<std::string> lines;
        std::string line;
        std::size_t start = 0;
        while (start < text.size()) {
            const auto end = text.find(' ', start);
            const std::string word = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = end == std::string::npos ? text.size() : end + 1;
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (line.empty() || fonts->body.Width(candidate) <= width) {
                line = candidate;
                continue;
            }
            lines.push_back(line);
            line = word;
        }
        if (!line.empty()) lines.push_back(line);
        if (lines.size() > maximumLines) {
            lines.resize(maximumLines);
            auto& last = lines.back();
            while (!last.empty() && fonts->body.Width(last + "…") > width) last.pop_back();
            last += "…";
        }
        return lines;
    }

    void showLoadError(const std::exception& error) {
        std::string message = error.what();
        constexpr std::string_view Prefix = "Pad: invalid input mapping ";
        if (message.starts_with(Prefix)) message.erase(0, Prefix.size());
        for (const auto& path : watchedPaths()) {
            const auto full = path.string();
            for (auto found = message.find(full); found != std::string::npos; found = message.find(full)) message.replace(found, full.size(), path.filename().string());
        }
        reloadError = message;
        std::fprintf(stderr, "pad_viz: %s\n", error.what());
    }

    void pushSelftestEvents(int frame) {
        const auto push = [](const SDL_Event& event) {
            SDL_Event copy = event;
            SDL_PushEvent(&copy);
        };
        if (frame == 2) push(PadSimulation::KeyEvent(SDL_SCANCODE_W, true));
        if (frame == 3) push(PadSimulation::KeyEvent(SDL_SCANCODE_Z, true));
        if (frame == 4) push(PadSimulation::KeyEvent(SDL_SCANCODE_SPACE, true));
        if (frame == 5) {
            push(PadSimulation::KeyEvent(SDL_SCANCODE_W, false));
            push(PadSimulation::KeyEvent(SDL_SCANCODE_Z, false));
            push(PadSimulation::KeyEvent(SDL_SCANCODE_SPACE, false));
        }
        if (frame == 6) {
            push(PadSimulation::ButtonEvent(SDL_BUTTON_MIDDLE, true));
            push(PadSimulation::ButtonEvent(SDL_BUTTON_MIDDLE, false));
        }
        if (frame >= 7) push(PadSimulation::MotionEvent(30, -12));
        if (frame == 20) {
            push(PadSimulation::KeyEvent(SDL_SCANCODE_W, true));
            push(PadSimulation::KeyEvent(SDL_SCANCODE_D, true));
            push(PadSimulation::KeyEvent(SDL_SCANCODE_Z, true));
        }
    }

    bool checkSelftest(int frame) {
        const auto expect = [frame](bool condition, const char* what) {
            if (!condition) std::fprintf(stderr, "pad_viz: frame %d: %s\n", frame, what);
            return condition;
        };
        bool passed = true;
        if (frame == 1) passed = expect(state.sticks == std::array<std::uint8_t, 4>{128, 128, 128, 128}, "sticks start centered");
        if (frame == 2) passed = expect(state.sticks[1] == 0, "W tilts the left stick up") && expect(canvas->Count(Palette::Accent) > 50, "the stick point is drawn");
        if (frame == 3) passed = expect(state.sticks[1] == Pad::StickByte(-0.5), "the stick modifier halves the tilt");
        if (frame == 4) passed = expect((state.buttons & static_cast<std::uint32_t>(Pad::PadButton::Cross)) != 0, "Space presses Cross");
        if (frame == 5) passed = expect(state.sticks[1] == 128 && state.buttons == 0, "released keys center the stick");
        if (frame == 6) passed = expect(keyboardMouse->MouseMode(), "middle click captures the mouse");
        if (frame == SelftestFrames) {
            passed = expect(state.sticks[2] >= Pad::StickByte(0.2) && state.sticks[3] < 128, "mouse motion tilts the right stick right and up") &&
                expect(state.sticks[0] == Pad::StickByte(0.5) && state.sticks[1] == Pad::StickByte(-0.5), "keys and modifier still drive the left stick");
        }
        return passed;
    }

    Options options;
    SDL_Window* window = nullptr;
    std::unique_ptr<Canvas> canvas;
    std::unique_ptr<Fonts> fonts;
    std::unique_ptr<Pad::KeyboardMouse> keyboardMouse;
    PadInputState state;
    std::deque<Snapshot> history;
    std::deque<std::pair<Clock::time_point, double>> mouseCounts;
    std::ofstream recording;
    std::string watched;
    std::string reloadError;
    Clock::time_point start{};
    Clock::time_point nextReloadCheck{};
    Clock::time_point reloadedAt{};
    bool running = true;
};

std::filesystem::path executableDirectory() {
    std::unique_ptr<char, decltype(&SDL_free)> basePath(SDL_GetBasePath(), SDL_free);
    return basePath ? std::filesystem::path(basePath.get()) : std::filesystem::current_path();
}

constexpr const char* Usage =
    "usage: pad_viz [--dir <game directory>] [--title <title ID>] [--config <ini>] [--deadzone <0..0.9>]\n"
    "               [--record <trace>] [--frames <n>] [--screenshot <bmp>] [--selftest]\n";

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const auto value = [&]() -> std::string {
            if (index + 1 >= argc) throw std::runtime_error(std::string(Usage));
            return argv[++index];
        };
        if (argument == "--dir") options.directory = value();
        else if (argument == "--title") options.title = value();
        else if (argument == "--config") options.config = value();
        else if (argument == "--record") options.record = value();
        else if (argument == "--screenshot") options.screenshot = value();
        else if (argument == "--deadzone") options.gameDeadzone = std::stod(value());
        else if (argument == "--frames") options.frames = std::stoi(value());
        else if (argument == "--selftest") options.selftest = true;
        else throw std::runtime_error(std::string(Usage));
    }
    if (!(options.gameDeadzone >= 0.0 && options.gameDeadzone <= 0.9)) throw std::runtime_error("pad_viz: --deadzone must be between 0 and 0.9");
    if (!options.config.empty() && !options.directory.empty()) throw std::runtime_error("pad_viz: use --config or --dir, not both");
    if (options.selftest) options.frames = SelftestFrames;
    if (options.directory.empty()) options.directory = executableDirectory();
    return options;
}

}

int main(int argc, char** argv) {
    try {
        return Visualiser(parseOptions(argc, argv)).Run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
