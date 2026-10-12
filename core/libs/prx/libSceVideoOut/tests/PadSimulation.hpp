#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_TESTS_PADSIMULATION_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_TESTS_PADSIMULATION_HPP

#include "prx/libScePad/include/KeyboardMouse.hpp"

#include <chrono>
#include <cmath>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "SDL_keyboard.h"

namespace PadSimulation {

struct TimedEvent {
    double ms = 0.0;
    SDL_Event event{};
    bool setMouseMode = false;
    bool mouseMode = false;
    bool release = false;
};

struct Sample {
    double ms = 0.0;
    PadInputState state;
};

inline SDL_Event MotionEvent(int deltaX, int deltaY) {
    SDL_Event event{};
    event.type = SDL_MOUSEMOTION;
    event.motion.xrel = deltaX;
    event.motion.yrel = deltaY;
    return event;
}

inline SDL_Event KeyEvent(SDL_Scancode key, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.keysym.scancode = key;
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    return event;
}

inline SDL_Event ButtonEvent(Uint8 button, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.button = button;
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    return event;
}

inline SDL_Event FocusLostEvent() {
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    return event;
}

inline SDL_Event WheelEvent(int direction) {
    SDL_Event event{};
    event.type = SDL_MOUSEWHEEL;
    event.wheel.y = direction;
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    return event;
}

inline TimedEvent At(double ms, const SDL_Event& event) {
    return {ms, event, false, false, false};
}

inline TimedEvent MouseModeAt(double ms, bool enabled) {
    return {ms, SDL_Event{}, true, enabled, false};
}

inline TimedEvent ReleaseAt(double ms) {
    return {ms, SDL_Event{}, false, false, true};
}

inline void AppendMotion(std::vector<TimedEvent>& events, double startMs, double durationMs, double countsPerSecondX, double countsPerSecondY, double reportHz = 1000.0) {
    const double periodMs = 1000.0 / reportHz;
    double owedX = 0.0;
    double owedY = 0.0;
    for (double ms = startMs + periodMs; ms <= startMs + durationMs + 1e-9; ms += periodMs) {
        owedX += countsPerSecondX * periodMs / 1000.0;
        owedY += countsPerSecondY * periodMs / 1000.0;
        const int deltaX = static_cast<int>(std::trunc(owedX));
        const int deltaY = static_cast<int>(std::trunc(owedY));
        owedX -= deltaX;
        owedY -= deltaY;
        if (deltaX != 0 || deltaY != 0) events.push_back(At(ms, MotionEvent(deltaX, deltaY)));
    }
}

inline std::vector<Sample> Simulate(const Pad::InputConfiguration& configuration, const std::vector<TimedEvent>& events, double pollMs, double endMs) {
    using Clock = Pad::KeyboardMouse::Clock;
    const auto origin = Clock::time_point{} + std::chrono::hours(1);
    const auto at = [origin](double ms) {
        return origin + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
    };
    Pad::KeyboardMouse keyboardMouse(configuration);
    keyboardMouse.Advance(at(0.0));
    std::vector<Sample> samples;
    std::size_t next = 0;
    const auto steps = static_cast<long long>(std::floor(endMs / pollMs + 1e-9));
    for (long long step = 1; step <= steps; ++step) {
        const double ms = static_cast<double>(step) * pollMs;
        const auto now = at(ms);
        for (; next < events.size() && events[next].ms <= ms + 1e-9; ++next) {
            const auto& timed = events[next];
            if (timed.release) {
                keyboardMouse.Release();
                continue;
            }
            if (timed.setMouseMode) {
                keyboardMouse.SetMouseMode(timed.mouseMode);
                continue;
            }
            const auto requests = keyboardMouse.HandleEvent(timed.event, now);
            if (requests.toggleMouse) keyboardMouse.SetMouseMode(!keyboardMouse.MouseMode());
            if (requests.releaseMouse) keyboardMouse.SetMouseMode(false);
        }
        keyboardMouse.Advance(now);
        samples.push_back({ms, keyboardMouse.Compose(PadInputState{})});
    }
    return samples;
}

inline Uint8 ParseMouseButton(const std::string& name) {
    if (name == "Left") return SDL_BUTTON_LEFT;
    if (name == "Middle") return SDL_BUTTON_MIDDLE;
    if (name == "Right") return SDL_BUTTON_RIGHT;
    if (name == "X1") return SDL_BUTTON_X1;
    if (name == "X2") return SDL_BUTTON_X2;
    throw std::runtime_error("unknown mouse button '" + name + "'");
}

inline const char* MouseButtonName(Uint8 button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return "Left";
        case SDL_BUTTON_MIDDLE: return "Middle";
        case SDL_BUTTON_RIGHT: return "Right";
        case SDL_BUTTON_X1: return "X1";
        case SDL_BUTTON_X2: return "X2";
        default: return nullptr;
    }
}

inline bool WriteTraceLine(std::ostream& stream, double ms, const SDL_Event& event) {
    std::ostringstream line;
    line.precision(3);
    line << std::fixed << ms << ' ';
    switch (event.type) {
        case SDL_MOUSEMOTION:
            line << "move " << event.motion.xrel << ' ' << event.motion.yrel;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (event.key.repeat != 0) return false;
            const char* name = SDL_GetScancodeName(event.key.keysym.scancode);
            if (name == nullptr || name[0] == '\0') return false;
            line << "key " << event.key.keysym.scancode << ' ' << (event.type == SDL_KEYDOWN ? "down" : "up") << ' ' << name;
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            const char* name = MouseButtonName(event.button.button);
            if (name == nullptr) return false;
            line << "button " << name << ' ' << (event.type == SDL_MOUSEBUTTONDOWN ? "down" : "up");
            break;
        }
        case SDL_MOUSEWHEEL: {
            int direction = (event.wheel.y > 0) - (event.wheel.y < 0);
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) direction = -direction;
            if (direction == 0) return false;
            line << "wheel " << direction;
            break;
        }
        default:
            return false;
    }
    stream << line.str() << '\n';
    return true;
}

inline std::vector<TimedEvent> ReadTrace(std::istream& stream) {
    std::vector<TimedEvent> events;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        const auto comment = line.find('#');
        std::istringstream fields(line.substr(0, comment));
        double ms = 0.0;
        std::string kind;
        if (!(fields >> ms)) {
            if (fields.eof()) continue;
            throw std::runtime_error("trace line " + std::to_string(lineNumber) + ": expected a time in milliseconds");
        }
        if (!(fields >> kind)) throw std::runtime_error("trace line " + std::to_string(lineNumber) + ": expected an event");
        if (!events.empty() && ms < events.back().ms) throw std::runtime_error("trace line " + std::to_string(lineNumber) + ": time goes backwards");
        if (kind == "move") {
            int deltaX = 0;
            int deltaY = 0;
            if (!(fields >> deltaX >> deltaY)) throw std::runtime_error("trace line " + std::to_string(lineNumber) + ": move needs two counts");
            events.push_back(At(ms, MotionEvent(deltaX, deltaY)));
        } else if (kind == "key") {
            int scancode = 0;
            std::string state;
            if (!(fields >> scancode >> state) || scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_NUM_SCANCODES || (state != "down" && state != "up")) {
                throw std::runtime_error("trace line " + std::to_string(lineNumber) + ": key needs a scancode and down or up");
            }
            events.push_back(At(ms, KeyEvent(static_cast<SDL_Scancode>(scancode), state == "down")));
        } else if (kind == "button") {
            std::string name;
            std::string state;
            if (!(fields >> name >> state) || (state != "down" && state != "up")) throw std::runtime_error("trace line " + std::to_string(lineNumber) + ": button needs a name and down or up");
            events.push_back(At(ms, ButtonEvent(ParseMouseButton(name), state == "down")));
        } else if (kind == "wheel") {
            int direction = 0;
            if (!(fields >> direction) || (direction != 1 && direction != -1)) throw std::runtime_error("trace line " + std::to_string(lineNumber) + ": wheel needs 1 or -1");
            events.push_back(At(ms, WheelEvent(direction)));
        } else if (kind == "release") {
            events.push_back(ReleaseAt(ms));
        } else if (kind == "mouse") {
            std::string state;
            if (!(fields >> state) || (state != "on" && state != "off")) throw std::runtime_error("trace line " + std::to_string(lineNumber) + ": mouse needs on or off");
            events.push_back(MouseModeAt(ms, state == "on"));
        } else {
            throw std::runtime_error("trace line " + std::to_string(lineNumber) + ": unknown event '" + kind + "'");
        }
    }
    return events;
}

}

#endif
