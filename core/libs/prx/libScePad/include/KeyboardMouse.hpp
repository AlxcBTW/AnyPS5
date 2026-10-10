#ifndef CORE_LIBS_PRX_LIBSCEPAD_KEYBOARDMOUSE_HPP
#define CORE_LIBS_PRX_LIBSCEPAD_KEYBOARDMOUSE_HPP

#include <array>
#include <chrono>
#include <vector>
#include "SDL_events.h"

#include "InputMapping.hpp"
#include "PadState.hpp"

namespace Pad {

inline constexpr double MouseFullTiltCountsPerSecond = 3000.0;
inline constexpr double MouseMinimumStepSeconds = 0.0005;

class MouseStick {
public:
    explicit MouseStick(const InputSettings& settings);
    void Move(int deltaX, int deltaY);
    void Advance(double seconds);
    void Reset();
    double Speed() const;
    std::array<double, 2> Deflection() const;

private:
    InputSettings settings;
    double pendingX = 0.0;
    double pendingY = 0.0;
    double pendingSeconds = 0.0;
    double speedX = 0.0;
    double speedY = 0.0;
    double idleSeconds = 0.0;
};

struct InputRequests {
    bool toggleFullscreen = false;
    bool toggleMouse = false;
};

class KeyboardMouse {
public:
    using Clock = std::chrono::steady_clock;

    explicit KeyboardMouse(InputConfiguration configuration);
    InputRequests HandleEvent(const SDL_Event& event, Clock::time_point now);
    void Advance(Clock::time_point now);
    void SetMouseMode(bool enabled);
    void Release();
    bool MouseMode() const;
    double MouseSpeed() const;
    PadInputState Compose(const PadInputState& controller) const;
    const InputConfiguration& Configuration() const;

private:
    std::array<double, 4> keyTargets() const;
    void advanceKeys(Clock::time_point now);

    InputConfiguration configuration;
    std::vector<bool> pressed;
    std::vector<Clock::time_point> wheelReleaseTimes;
    MouseStick mouse;
    std::array<double, 4> keyAxes{};
    Clock::time_point lastAdvance{};
    Clock::time_point lastKeyAdvance{};
    bool advanced = false;
    bool keysAdvanced = false;
    bool mouseMode = false;
};

std::uint8_t StickByte(double deflection);

}

#endif
