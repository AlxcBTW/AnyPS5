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
inline constexpr double MouseRateWindowSeconds = 0.25;
inline constexpr double MouseRateGapSeconds = 0.05;
inline constexpr double MouseSmoothingReports = 3.0;
inline constexpr double MouseSmoothingLimitMs = 30.0;

class MouseStick {
public:
    explicit MouseStick(const InputSettings& settings);
    void Move(int deltaX, int deltaY);
    void Advance(double seconds);
    void Reset();
    double Speed() const;
    double ReportRate() const;
    double SmoothingMs() const;
    std::array<double, 2> Deflection() const;

private:
    void measureReportRate(double step, bool moved);

    InputSettings settings;
    double pendingX = 0.0;
    double pendingY = 0.0;
    double pendingSeconds = 0.0;
    double pendingCounts = 0.0;
    int pendingReports = 0;
    double speedX = 0.0;
    double speedY = 0.0;
    double idleSeconds = 0.0;
    double rateSeconds = 0.0;
    double rateCounts = 0.0;
    int rateReports = 0;
    double reportRate = 0.0;
};

struct InputRequests {
    bool toggleFullscreen = false;
    bool toggleMouse = false;
    bool releaseMouse = false;
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
    double MouseReportRate() const;
    double MouseSmoothingMs() const;
    PadInputState Compose(const PadInputState& controller) const;
    const InputConfiguration& Configuration() const;

private:
    std::array<double, 4> keyTargets() const;
    std::array<double, 2> triggerTargets() const;
    void advanceKeys(Clock::time_point now);

    InputConfiguration configuration;
    std::vector<bool> pressed;
    std::vector<Clock::time_point> wheelReleaseTimes;
    MouseStick mouse;
    std::array<double, 4> keyAxes{};
    std::array<double, 2> triggerAxes{};
    std::array<double, 2> wheelTriggers{};
    Clock::time_point lastAdvance{};
    Clock::time_point lastKeyAdvance{};
    bool advanced = false;
    bool keysAdvanced = false;
    bool mouseMode = false;
};

std::uint8_t StickByte(double deflection);

}

#endif
