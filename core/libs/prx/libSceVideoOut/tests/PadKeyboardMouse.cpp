#include "prx/libScePad/include/InputMapping.hpp"
#include "prx/libScePad/include/KeyboardMouse.hpp"
#include "PadSimulation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using PadSimulation::At;
using PadSimulation::KeyEvent;
using PadSimulation::Sample;
using PadSimulation::Simulate;
using PadSimulation::TimedEvent;

constexpr double Polls[] = {33.0, 8.0, 1.0};

void check(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

template<typename TAction>
std::string expectFailure(TAction action) {
    try { action(); }
    catch (const std::runtime_error& error) { return error.what(); }
    throw std::runtime_error("expected an exception");
}

Pad::InputConfiguration parse(const std::string& text) {
    std::istringstream stream(text);
    return Pad::ParseInputConfiguration(stream, "test.ini");
}

Pad::InputConfiguration defaults() {
    return parse("");
}

double axis(std::uint8_t value) {
    return value < 128 ? (static_cast<double>(value) - 128.0) / 128.0 : (static_cast<double>(value) - 128.0) / 127.0;
}

double rightMagnitude(const Sample& sample) {
    return std::hypot(axis(sample.state.sticks[2]), axis(sample.state.sticks[3]));
}

double meanRightX(const std::vector<Sample>& samples, double fromMs, double toMs) {
    double sum = 0.0;
    int count = 0;
    for (const auto& sample : samples) {
        if (sample.ms <= fromMs || sample.ms > toMs) continue;
        sum += axis(sample.state.sticks[2]);
        ++count;
    }
    check(count > 0, "no samples in window");
    return sum / count;
}

double heldMeanRightX(const std::vector<Sample>& samples, double pollMs, double fromMs, double toMs) {
    double sum = 0.0;
    for (const auto& sample : samples) {
        if (sample.ms >= fromMs - 1e-9 && sample.ms < toMs - 1e-9) sum += axis(sample.state.sticks[2]) * pollMs;
    }
    return sum / (toMs - fromMs);
}

std::vector<TimedEvent> mouseOnThenMotion(double durationMs, double countsPerSecondX, double countsPerSecondY = 0.0) {
    std::vector<TimedEvent> events{PadSimulation::MouseModeAt(0.0, true)};
    PadSimulation::AppendMotion(events, 0.0, durationMs, countsPerSecondX, countsPerSecondY);
    return events;
}

double expectedDeflection(const Pad::InputSettings& settings, double normalizedSpeed) {
    const double clamped = std::min(normalizedSpeed, settings.mouseMaxSpeed);
    return settings.mouseDeadzoneOffset + (1.0 - settings.mouseDeadzoneOffset) * std::pow(std::min(clamped, 1.0), settings.mouseCurve);
}

void testDefaults() {
    const auto configuration = defaults();
    check(configuration.settings.mouseSensitivity == 1.0, "default sensitivity");
    check(configuration.settings.mouseDeadzoneOffset == 0.2, "default deadzone offset");
    check(configuration.settings.mouseSmoothingMs == 10.0, "default smoothing");
    check(configuration.settings.mouseDecayMs == 30.0, "default decay");
    check(configuration.settings.stickModifierScale == 0.5, "default modifier scale");
    check(configuration.settings.keyboardRampMs == 0.0, "keys tilt at once by default");
    check(configuration.bindings.size() == Pad::InputMapping.size(), "default bindings");
}

void testSettingsParse() {
    const auto configuration = parse(
        "\xEF\xBB\xBFmousesensitivity = 2.5\n"
        "MouseCurve=0.7 ; softer start\n"
        "MouseDeadzoneOffset = 0.15\n"
        "MouseMaxSpeed = 1.5\n"
        "MouseSmoothingMs = 0\n"
        "MouseDecayMs = 50\n"
        "MouseVerticalScale = 0.5\n"
        "Cross = KEY:F\n");
    check(configuration.settings.mouseSensitivity == 2.5, "sensitivity parsed");
    check(configuration.settings.mouseCurve == 0.7, "curve parsed");
    check(configuration.settings.mouseDeadzoneOffset == 0.15, "offset parsed");
    check(configuration.settings.mouseMaxSpeed == 1.5, "max speed parsed");
    check(configuration.settings.mouseSmoothingMs == 0.0, "smoothing parsed");
    check(configuration.settings.mouseDecayMs == 50.0, "decay parsed");
    check(configuration.settings.mouseVerticalScale == 0.5, "vertical scale parsed");
    check(std::any_of(configuration.bindings.begin(), configuration.bindings.end(), [](const Pad::InputBinding& binding) {
        return binding.key == SDL_SCANCODE_F && binding.button == Pad::PadButton::Cross;
    }), "bindings still parse next to settings");
    check(expectFailure([] { parse("MouseSensitivity = 0\n"); }).find("test.ini:1: MouseSensitivity must be between 0.01 and 100") != std::string::npos, "sensitivity range");
    check(expectFailure([] { parse("\nMouseCurve = fast\n"); }).find("test.ini:2: MouseCurve must be a number") != std::string::npos, "non-numeric value");
    check(expectFailure([] { parse("MouseDeadzoneOffset = 0.95\n"); }).find("between 0 and 0.9") != std::string::npos, "offset range");
    check(expectFailure([] { parse("MouseDecayMs =\n"); }).find("must be a number") != std::string::npos, "empty value");
    check(expectFailure([] { parse("MouseDecayMs = 20\nmousedecayms = 30\n"); }).find("test.ini:2: MouseDecayMs is given twice") != std::string::npos, "duplicate setting");
    check(expectFailure([] { parse("MouseSpeed = 2\n"); }).find("unknown action or setting 'MouseSpeed'") != std::string::npos, "unknown setting");
}

void testCenteredWithoutMotion() {
    for (const double poll : Polls) {
        const auto samples = Simulate(defaults(), {PadSimulation::MouseModeAt(0.0, true)}, poll, 300.0);
        for (const auto& sample : samples) {
            check(sample.state.sticks[2] == 128 && sample.state.sticks[3] == 128, "a resting mouse leaves the stick centered");
        }
    }
}

void testSteadyDeflectionFollowsCurve(const Pad::InputConfiguration& configuration, const char* name) {
    const auto& settings = configuration.settings;
    for (const double speed : {0.05, 0.25, 0.5, 0.75, 0.95, 3.0}) {
        const double countsPerSecond = speed * Pad::MouseFullTiltCountsPerSecond / settings.mouseSensitivity;
        const double expected = expectedDeflection(settings, speed);
        for (const double poll : Polls) {
            const auto samples = Simulate(configuration, mouseOnThenMotion(600.0, countsPerSecond), poll, 600.0);
            const double measured = meanRightX(samples, 300.0, 600.0);
            if (std::abs(measured - expected) > 0.02) {
                throw std::runtime_error(std::string(name) + ": speed " + std::to_string(speed) + " at " + std::to_string(poll) + " ms polls gives " +
                    std::to_string(measured) + ", expected " + std::to_string(expected));
            }
        }
    }
}

void testDeflectionCurve() {
    testSteadyDeflectionFollowsCurve(defaults(), "defaults");
    testSteadyDeflectionFollowsCurve(parse("MouseCurve = 2\nMouseDeadzoneOffset = 0.1\nMouseSensitivity = 2\n"), "quadratic");
    testSteadyDeflectionFollowsCurve(parse("MouseCurve = 0.5\nMouseDeadzoneOffset = 0\n"), "square root");
}

void testDeadzoneOffset() {
    const auto configuration = defaults();
    for (const double poll : Polls) {
        const auto samples = Simulate(configuration, mouseOnThenMotion(400.0, 40.0), poll, 400.0);
        bool moved = false;
        for (const auto& sample : samples) {
            const double magnitude = rightMagnitude(sample);
            if (magnitude == 0.0) continue;
            moved = true;
            check(magnitude >= configuration.settings.mouseDeadzoneOffset - 0.01, "the slowest movement still clears the deadzone offset at " + std::to_string(poll) + " ms polls");
        }
        check(moved, "slow movement moves the stick");
    }
    const auto samples = Simulate(parse("MouseDeadzoneOffset = 0\n"), mouseOnThenMotion(400.0, 40.0), 8.0, 400.0);
    check(meanRightX(samples, 200.0, 400.0) < 0.05, "without an offset slow movement stays small");
}

double centeredAfter(const std::vector<Sample>& samples, double stopMs) {
    double lastMoving = stopMs;
    double previous = 2.0;
    for (const auto& sample : samples) {
        if (sample.ms <= stopMs) continue;
        const double magnitude = rightMagnitude(sample);
        check(magnitude <= previous + 1e-9, "the stick never grows after the mouse stops");
        previous = magnitude;
        if (magnitude != 0.0) lastMoving = sample.ms;
    }
    return lastMoving - stopMs;
}

void testDecay() {
    const auto configuration = defaults();
    for (const double speed : {0.3, 1.0}) {
        for (const double poll : Polls) {
            const auto samples = Simulate(configuration, mouseOnThenMotion(300.0, speed * Pad::MouseFullTiltCountsPerSecond), poll, 600.0);
            const double tail = centeredAfter(samples, 300.0);
            check(tail < configuration.settings.mouseDecayMs + poll, "stick centered within decay plus one poll at " + std::to_string(poll) + " ms polls, took " + std::to_string(tail));
            check(samples.back().state.sticks[2] == 128 && samples.back().state.sticks[3] == 128, "stick returns exactly to center");
        }
    }
    const auto slow = parse("MouseDecayMs = 120\n");
    const auto samples = Simulate(slow, mouseOnThenMotion(300.0, Pad::MouseFullTiltCountsPerSecond), 1.0, 600.0);
    const double tail = centeredAfter(samples, 300.0);
    check(tail >= 100.0 && tail < 121.0, "a longer decay keeps the stick moving longer");
}

void testMaxSpeedClamp() {
    const auto configuration = defaults();
    constexpr double StopMs = 264.0;
    for (const double poll : Polls) {
        const auto flick = Simulate(configuration, mouseOnThenMotion(StopMs, 10.0 * Pad::MouseFullTiltCountsPerSecond), poll, 500.0);
        const auto full = Simulate(configuration, mouseOnThenMotion(StopMs, 1.5 * Pad::MouseFullTiltCountsPerSecond), poll, 500.0);
        check(flick[flick.size() / 3].state.sticks[2] == 255, "a fast flick reaches full tilt");
        check(centeredAfter(flick, StopMs) <= centeredAfter(full, StopMs) + 1e-9, "a flick faster than full tilt does not extend the tail");
        double flickSum = 0.0;
        double fullSum = 0.0;
        for (std::size_t index = 0; index < flick.size(); ++index) {
            if (flick[index].ms <= StopMs) continue;
            flickSum += rightMagnitude(flick[index]);
            fullSum += rightMagnitude(full[index]);
        }
        check(std::abs(flickSum - fullSum) <= 0.02 * std::max(1.0, fullSum), "the stick decays from a flick like from full tilt at " + std::to_string(poll) + " ms polls");
    }
    const auto headroom = parse("MouseMaxSpeed = 3\n");
    const auto flick = Simulate(headroom, mouseOnThenMotion(StopMs, 10.0 * Pad::MouseFullTiltCountsPerSecond), 1.0, 500.0);
    const auto full = Simulate(defaults(), mouseOnThenMotion(StopMs, 10.0 * Pad::MouseFullTiltCountsPerSecond), 1.0, 500.0);
    check(rightMagnitude(flick[275]) > rightMagnitude(full[275]), "a higher max speed lets a flick hold full tilt longer");
}

void testSmoothing() {
    const auto instant = Simulate(parse("MouseSmoothingMs = 0\n"), mouseOnThenMotion(100.0, Pad::MouseFullTiltCountsPerSecond / 2.0), 8.0, 100.0);
    check(std::abs(axis(instant[1].state.sticks[2]) - expectedDeflection(defaults().settings, 0.5)) < 0.02, "without smoothing the stick follows at once");
    const auto smoothed = Simulate(defaults(), mouseOnThenMotion(100.0, Pad::MouseFullTiltCountsPerSecond / 2.0), 1.0, 100.0);
    const double early = (axis(smoothed[9].state.sticks[2]) - 0.2) / 0.8;
    check(early > 0.5 * 0.55 && early < 0.5 * 0.72, "smoothing reaches about 63 percent after one time constant");
}

std::vector<TimedEvent> waveMotion(double base, double swing) {
    constexpr double Pi = 3.14159265358979323846;
    std::vector<TimedEvent> events{PadSimulation::MouseModeAt(0.0, true)};
    for (int segment = 0; segment < 200; ++segment) {
        const double speed = (base + swing * std::sin(2.0 * Pi * segment * 8.0 / 800.0)) * Pad::MouseFullTiltCountsPerSecond;
        PadSimulation::AppendMotion(events, segment * 8.0, 8.0, speed, 0.3 * speed);
    }
    PadSimulation::AppendMotion(events, 1700.0, 200.0, 250.0, -120.0);
    return events;
}

double integratedRightMagnitude(const std::vector<Sample>& samples, double pollMs) {
    double sum = 0.0;
    for (const auto& sample : samples) sum += rightMagnitude(sample) * pollMs / 1000.0;
    return sum;
}

void testPollRateIndependence() {
    for (const auto& [name, events] : {std::pair{"one-way wave", waveMotion(0.5, 0.3)}, std::pair{"back-and-forth wave", waveMotion(0.0, 0.6)}}) {
        std::vector<std::vector<Sample>> runs;
        for (const double poll : Polls) runs.push_back(Simulate(defaults(), events, poll, 2112.0));
        const double reference = integratedRightMagnitude(runs[2], Polls[2]);
        for (std::size_t run = 0; run < runs.size(); ++run) {
            const double integrated = integratedRightMagnitude(runs[run], Polls[run]);
            check(std::abs(integrated - reference) <= 0.02 * reference, std::string(name) + ": total deflection at " + std::to_string(Polls[run]) + " ms polls is " +
                std::to_string(integrated) + ", 1 ms gives " + std::to_string(reference));
            const double tolerance = 0.03 + 0.0025 * Polls[run];
            for (double window = 0.0; window + 264.0 <= 2112.0; window += 264.0) {
                const double mean = heldMeanRightX(runs[run], Polls[run], window, window + 264.0);
                const double expected = heldMeanRightX(runs[2], Polls[2], window, window + 264.0);
                check(std::abs(mean - expected) < tolerance, std::string(name) + ": held deflection at " + std::to_string(Polls[run]) + " ms polls from " +
                    std::to_string(window) + " ms is " + std::to_string(mean) + ", 1 ms gives " + std::to_string(expected));
            }
        }
    }
}

void testVerticalScale() {
    const auto configuration = parse("MouseVerticalScale = 0.5\nMouseDeadzoneOffset = 0\n");
    const auto samples = Simulate(configuration, mouseOnThenMotion(300.0, 0.0, 0.8 * Pad::MouseFullTiltCountsPerSecond), 8.0, 300.0);
    const double y = axis(samples.back().state.sticks[3]);
    check(std::abs(y - 0.4) < 0.02, "vertical scale halves the vertical deflection");
}

void testMouseModeOff() {
    std::vector<TimedEvent> events;
    PadSimulation::AppendMotion(events, 0.0, 300.0, Pad::MouseFullTiltCountsPerSecond, 0.0);
    for (const auto& sample : Simulate(defaults(), events, 8.0, 300.0)) {
        check(sample.state.sticks[2] == 128, "motion without mouse mode leaves the stick alone");
    }
    events.insert(events.begin(), At(0.0, PadSimulation::ButtonEvent(SDL_BUTTON_MIDDLE, true)));
    events.insert(events.begin() + 1, At(0.0, PadSimulation::ButtonEvent(SDL_BUTTON_MIDDLE, false)));
    const auto samples = Simulate(defaults(), events, 8.0, 300.0);
    check(samples.back().state.sticks[2] == 255, "middle click turns mouse mode on");
}

void testKeyboardSticksUnchanged() {
    const std::vector<TimedEvent> events{
        At(0.0, KeyEvent(SDL_SCANCODE_W, true)),
        At(10.0, KeyEvent(SDL_SCANCODE_D, true)),
        At(20.0, KeyEvent(SDL_SCANCODE_A, true)),
        At(30.0, KeyEvent(SDL_SCANCODE_W, false)),
        At(30.0, KeyEvent(SDL_SCANCODE_D, false)),
        At(40.0, KeyEvent(SDL_SCANCODE_A, false))};
    const auto samples = Simulate(defaults(), events, 1.0, 50.0);
    check(samples[4].state.sticks[1] == 0 && samples[4].state.sticks[0] == 128, "W tilts the left stick fully up");
    check(samples[14].state.sticks[0] == 255, "D tilts the left stick fully right");
    check(samples[24].state.sticks[0] == 128, "A and D together cancel");
    check(samples[34].state.sticks[0] == 0 && samples[34].state.sticks[1] == 128, "A alone tilts fully left");
    check(samples[44].state.sticks[0] == 128, "released keys center the stick");
}

void testKeyboardSettingsParse() {
    const auto configuration = parse("StickModifierScale = 0.3\nKeyboardRampMs = 120\nStickModifier = KEY:Z\n");
    check(configuration.settings.stickModifierScale == 0.3, "modifier scale parsed");
    check(configuration.settings.keyboardRampMs == 120.0, "ramp parsed");
    const auto modifiers = std::count_if(configuration.bindings.begin(), configuration.bindings.end(), [](const Pad::InputBinding& binding) {
        return binding.control == Pad::InputControl::StickModifier;
    });
    check(modifiers == 1, "a StickModifier line replaces the built-in modifier key");
    check(expectFailure([] { parse("StickModifierScale = 0\n"); }).find("between 0.05 and 1") != std::string::npos, "modifier scale range");
    check(expectFailure([] { parse("KeyboardRampMs = -5\n"); }).find("between 0 and 2000") != std::string::npos, "ramp range");
}

void testStickModifier() {
    const std::vector<TimedEvent> events{
        At(0.0, KeyEvent(SDL_SCANCODE_LALT, true)),
        At(0.0, KeyEvent(SDL_SCANCODE_W, true)),
        At(10.0, KeyEvent(SDL_SCANCODE_D, true)),
        At(20.0, KeyEvent(SDL_SCANCODE_LALT, false)),
        At(30.0, KeyEvent(SDL_SCANCODE_T, true))};
    const auto samples = Simulate(defaults(), events, 1.0, 40.0);
    check(samples[4].state.sticks[1] == Pad::StickByte(-0.5), "the modifier halves an up tilt");
    check(samples[14].state.sticks[0] == Pad::StickByte(0.5) && samples[14].state.sticks[1] == Pad::StickByte(-0.5), "the modifier halves both axes");
    check(samples[24].state.sticks[0] == 255 && samples[24].state.sticks[1] == 0, "releasing the modifier restores full tilt");
    check(samples[34].state.sticks[3] == 0, "the right stick keys tilt fully without the modifier");
    const auto custom = Simulate(parse("StickModifierScale = 0.25\n"), events, 1.0, 10.0);
    check(custom[4].state.sticks[1] == Pad::StickByte(-0.25), "the modifier scale is configurable");
}

void testKeyboardRamp() {
    const auto configuration = parse("KeyboardRampMs = 100\n");
    const std::vector<TimedEvent> events{
        At(0.0, KeyEvent(SDL_SCANCODE_D, true)),
        At(264.0, KeyEvent(SDL_SCANCODE_A, true)),
        At(528.0, KeyEvent(SDL_SCANCODE_D, false)),
        At(792.0, KeyEvent(SDL_SCANCODE_A, false))};
    for (const double poll : Polls) {
        const auto samples = Simulate(configuration, events, poll, 1056.0);
        double previous = 0.0;
        for (const auto& sample : samples) {
            const double value = axis(sample.state.sticks[0]);
            const auto expectedAt = [](double ms) {
                if (ms < 264.0) return std::min(1.0, ms / 100.0);
                if (ms < 528.0) return std::max(0.0, 1.0 - (ms - 264.0) / 100.0);
                if (ms < 792.0) return std::max(-1.0, -(ms - 528.0) / 100.0);
                return std::min(0.0, -1.0 + (ms - 792.0) / 100.0);
            };
            const double latest = expectedAt(sample.ms);
            const double earliest = expectedAt(std::max(0.0, sample.ms - poll));
            const double low = std::min(latest, earliest) - 0.02;
            const double high = std::max(latest, earliest) + 0.02;
            check(value >= low && value <= high, "ramp at " + std::to_string(poll) + " ms polls is " + std::to_string(value) + " at " + std::to_string(sample.ms) + " ms");
            check(std::abs(value - previous) <= poll / 100.0 + 0.02, "ramp never jumps faster than its rate");
            previous = value;
        }
        check(samples.back().state.sticks[0] == 128, "ramp returns to center");
    }
}

void testRampWithModifierAndRelease() {
    const auto configuration = parse("KeyboardRampMs = 200\n");
    Pad::KeyboardMouse keyboardMouse(configuration);
    const auto origin = Pad::KeyboardMouse::Clock::time_point{} + std::chrono::hours(1);
    keyboardMouse.Advance(origin);
    keyboardMouse.HandleEvent(KeyEvent(SDL_SCANCODE_LALT, true), origin);
    keyboardMouse.HandleEvent(KeyEvent(SDL_SCANCODE_S, true), origin);
    keyboardMouse.Advance(origin + std::chrono::milliseconds(50));
    check(std::abs(axis(keyboardMouse.Compose({}).sticks[1]) - 0.25) < 0.01, "the ramp climbs toward the modifier tilt");
    keyboardMouse.Advance(origin + std::chrono::milliseconds(400));
    check(keyboardMouse.Compose({}).sticks[1] == Pad::StickByte(0.5), "the ramp stops at the modifier tilt");
    keyboardMouse.Release();
    check(keyboardMouse.Compose({}).sticks[1] == 128, "losing focus centers a ramped stick at once");
    PadInputState controller;
    controller.sticks = {10, 20, 30, 40};
    check(keyboardMouse.Compose(controller).sticks == controller.sticks, "an idle keyboard leaves the controller sticks alone");
}

class TemporaryDirectory {
public:
    TemporaryDirectory() : path(std::filesystem::temp_directory_path() / ("anyps5-pad-profiles-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    void Write(const std::string& name, const std::string& text) const {
        std::ofstream file(path / name, std::ios::binary);
        file << text;
        check(static_cast<bool>(file), "cannot write " + name);
    }
    const std::filesystem::path path;
};

void setConfigEnvironment(const std::string& value) {
#ifdef _WIN32
    check(_putenv_s("ANYPS5_INPUT_CONFIG", value.c_str()) == 0, "cannot set ANYPS5_INPUT_CONFIG");
#else
    check((value.empty() ? unsetenv("ANYPS5_INPUT_CONFIG") : setenv("ANYPS5_INPUT_CONFIG", value.c_str(), 1)) == 0, "cannot set ANYPS5_INPUT_CONFIG");
#endif
}

bool binds(const Pad::InputConfiguration& configuration, SDL_Scancode key, Pad::PadButton button) {
    return std::any_of(configuration.bindings.begin(), configuration.bindings.end(), [key, button](const Pad::InputBinding& binding) {
        return binding.key == key && binding.control == Pad::InputControl::Button && binding.button == button;
    });
}

void testProfiles() {
    setConfigEnvironment("");
    check(Pad::InputProfileName("TEST00001") == "anyps5-input.TEST00001.ini", "profile name");
    check(Pad::InputProfileName("").empty() && Pad::InputProfileName("../TEST").empty() && Pad::InputProfileName("A B").empty(), "unsafe title IDs have no profile");

    const TemporaryDirectory directory;
    auto configuration = Pad::LoadInputConfiguration(directory.path, "TEST00001");
    check(configuration.sources.empty() && configuration.bindings.size() == Pad::InputMapping.size(), "no files keep the built-in configuration");

    directory.Write("anyps5-input.TEST00001.ini", "MouseCurve = 1.5\n");
    configuration = Pad::LoadInputConfiguration(directory.path, "TEST00001");
    check(configuration.sources.size() == 1 && configuration.settings.mouseCurve == 1.5, "a profile applies without a shared file");

    directory.Write("anyps5-input.ini", "MouseSensitivity = 2\nMouseCurve = 0.8\nCross = KEY:F\n");
    directory.Write("anyps5-input.TEST00001.ini", "MouseCurve = 1.5\nCross = KEY:G\nCircle = KEY:H\n");
    configuration = Pad::LoadInputConfiguration(directory.path, "TEST00001");
    check(configuration.sources.size() == 2 && configuration.sources[1].filename() == "anyps5-input.TEST00001.ini", "the profile is read after the shared file");
    check(configuration.settings.mouseSensitivity == 2.0, "shared settings carry into the profile");
    check(configuration.settings.mouseCurve == 1.5, "profile settings replace shared ones");
    check(binds(configuration, SDL_SCANCODE_G, Pad::PadButton::Cross) && !binds(configuration, SDL_SCANCODE_F, Pad::PadButton::Cross), "a profile binding replaces the shared binding");
    check(binds(configuration, SDL_SCANCODE_H, Pad::PadButton::Circle) && !binds(configuration, SDL_SCANCODE_C, Pad::PadButton::Circle), "a profile binding replaces the built-in binding");

    const auto other = Pad::LoadInputConfiguration(directory.path, "TEST00002");
    check(other.sources.size() == 1 && other.settings.mouseCurve == 0.8, "another title uses only the shared file");

    directory.Write("anyps5-input.TEST00001.ini", "MouseCurve = 9\n");
    check(expectFailure([&directory] { Pad::LoadInputConfiguration(directory.path, "TEST00001"); }).find("anyps5-input.TEST00001.ini:1: MouseCurve") != std::string::npos,
        "profile errors name the profile file");

    directory.Write("anyps5-input.TEST00001.ini", "KeyboardRampMs = 80\n");
    directory.Write("custom.ini", "MouseSensitivity = 3\n");
    setConfigEnvironment((directory.path / "custom.ini").string());
    configuration = Pad::LoadInputConfiguration(directory.path, "TEST00001");
    check(configuration.settings.mouseSensitivity == 3.0 && configuration.settings.mouseCurve == 1.0, "ANYPS5_INPUT_CONFIG replaces the shared file");
    check(configuration.settings.keyboardRampMs == 80.0, "the profile still applies with ANYPS5_INPUT_CONFIG");
    setConfigEnvironment((directory.path / "missing.ini").string());
    check(expectFailure([&directory] { Pad::LoadInputConfiguration(directory.path, "TEST00001"); }).find("cannot read input mapping") != std::string::npos,
        "a missing ANYPS5_INPUT_CONFIG file is an error");
    setConfigEnvironment("");
}

void testOppositeKeysWithAlternates() {
    const auto configuration = parse("LeftStickLeft = KEY:A\nLeftStickLeft = KEY:Left\n");
    const std::vector<TimedEvent> events{
        At(0.0, KeyEvent(SDL_SCANCODE_A, true)),
        At(0.0, KeyEvent(SDL_SCANCODE_LEFT, true)),
        At(10.0, KeyEvent(SDL_SCANCODE_D, true)),
        At(20.0, KeyEvent(SDL_SCANCODE_A, false))};
    const auto samples = Simulate(configuration, events, 1.0, 30.0);
    check(samples[4].state.sticks[0] == 0, "two keys for the same direction tilt fully, not further");
    check(samples[14].state.sticks[0] == 128, "two keys against one opposite key still center the axis");
    check(samples[24].state.sticks[0] == 128, "one remaining key on each side still centers the axis");
}

void testSlowReportingMouse() {
    const auto speed = 0.5 * Pad::MouseFullTiltCountsPerSecond;
    for (const double reportHz : {125.0, 250.0}) {
        std::vector<TimedEvent> events{PadSimulation::MouseModeAt(0.0, true)};
        PadSimulation::AppendMotion(events, 0.0, 1584.0, speed, 0.0, reportHz);
        const auto smoothed = parse("MouseSmoothingMs = 25\n");
        const double expected = expectedDeflection(defaults().settings, 0.5);
        for (const double poll : {33.0, 16.0, 8.0, 1.0}) {
            const auto samples = Simulate(defaults(), events, poll, 1584.0);
            check(std::abs(heldMeanRightX(samples, poll, 528.0, 1584.0) - expected) < 0.02,
                "a " + std::to_string(reportHz) + " Hz mouse keeps the mean tilt at " + std::to_string(poll) + " ms polls");
            double low = 1.0;
            double high = 0.0;
            for (const auto& sample : Simulate(smoothed, events, poll, 1584.0)) {
                if (sample.ms < 528.0) continue;
                low = std::min(low, axis(sample.state.sticks[2]));
                high = std::max(high, axis(sample.state.sticks[2]));
            }
            check(high - low < 0.16, "25 ms smoothing evens out a " + std::to_string(reportHz) + " Hz mouse at " + std::to_string(poll) + " ms polls, spread " +
                std::to_string(high - low));
        }
    }
}

void testReplayRelease() {
    std::istringstream trace("0 key 26 down W\n0 mouse on\n5 move 40 0\n10 release\n");
    const auto events = PadSimulation::ReadTrace(trace);
    check(events.back().release, "release line");
    const auto samples = Simulate(defaults(), events, 1.0, 20.0);
    check(samples[5].state.sticks[1] == 0, "W held before the release");
    check(samples[12].state.sticks[1] == 128, "a release line lets go of every key");
}

void testDefaultBindings() {
    struct Expectation {
        SDL_Event event;
        Pad::PadButton button;
        const char* name;
    };
    const std::vector<Expectation> expectations{
        {KeyEvent(SDL_SCANCODE_ESCAPE, true), Pad::PadButton::Options, "Escape pauses with Options"},
        {KeyEvent(SDL_SCANCODE_SPACE, true), Pad::PadButton::Cross, "Space is Cross"},
        {KeyEvent(SDL_SCANCODE_X, true), Pad::PadButton::Cross, "X is Cross"},
        {KeyEvent(SDL_SCANCODE_C, true), Pad::PadButton::Circle, "C is Circle"},
        {KeyEvent(SDL_SCANCODE_Z, true), Pad::PadButton::Square, "Z is Square"},
        {KeyEvent(SDL_SCANCODE_V, true), Pad::PadButton::Triangle, "V is Triangle"},
        {KeyEvent(SDL_SCANCODE_I, true), Pad::PadButton::Triangle, "I stays Triangle"},
        {KeyEvent(SDL_SCANCODE_Q, true), Pad::PadButton::L1, "Q is L1"},
        {KeyEvent(SDL_SCANCODE_E, true), Pad::PadButton::R1, "E is R1"},
        {KeyEvent(SDL_SCANCODE_R, true), Pad::PadButton::L2, "R is L2"},
        {KeyEvent(SDL_SCANCODE_LSHIFT, true), Pad::PadButton::L3, "Left Shift is L3"},
        {KeyEvent(SDL_SCANCODE_LCTRL, true), Pad::PadButton::R3, "Left Ctrl is R3"},
        {KeyEvent(SDL_SCANCODE_UP, true), Pad::PadButton::Up, "the arrow keys are the D-pad"},
        {PadSimulation::ButtonEvent(SDL_BUTTON_LEFT, true), Pad::PadButton::Square, "left click is Square"},
        {PadSimulation::ButtonEvent(SDL_BUTTON_RIGHT, true), Pad::PadButton::R2, "right click is R2"},
        {PadSimulation::ButtonEvent(SDL_BUTTON_X1, true), Pad::PadButton::L2, "mouse button 4 is L2"},
        {PadSimulation::ButtonEvent(SDL_BUTTON_X2, true), Pad::PadButton::L1, "mouse button 5 is L1"},
        {PadSimulation::WheelEvent(1), Pad::PadButton::Up, "wheel up is D-pad up"},
    };
    for (const auto& expectation : expectations) {
        const auto samples = Simulate(defaults(), {At(0.0, expectation.event)}, 1.0, 2.0);
        check(samples.back().state.buttons == static_cast<std::uint32_t>(expectation.button), expectation.name);
    }
    const auto touch = Simulate(defaults(), {At(0.0, KeyEvent(SDL_SCANCODE_TAB, true))}, 1.0, 2.0);
    check(touch.back().state.touchRight, "Tab touches the touch pad");
    const auto walk = Simulate(defaults(), {At(0.0, KeyEvent(SDL_SCANCODE_LALT, true)), At(0.0, KeyEvent(SDL_SCANCODE_W, true))}, 1.0, 2.0);
    check(walk.back().state.buttons == 0 && walk.back().state.sticks[1] == Pad::StickByte(-0.5), "Left Alt walks at half tilt and presses nothing");
    const auto overridden = Simulate(parse("L2 = KEY:F1\nStickModifier = KEY:CapsLock\n"), {At(0.0, KeyEvent(SDL_SCANCODE_R, true)), At(0.0, KeyEvent(SDL_SCANCODE_LALT, true)),
        At(0.0, KeyEvent(SDL_SCANCODE_W, true))}, 1.0, 2.0);
    check(overridden.back().state.buttons == 0 && overridden.back().state.sticks[1] == 0, "new defaults can be replaced in the ini file");
}

void testDescribeBindings() {
    const auto lines = Pad::DescribeBindings(defaults());
    const std::vector<std::string> expected{
        "Sticks: left W A S D, right T F G H; Middle click/F7 puts the mouse on the right stick; hold Left Alt for 50% tilt",
        "Buttons: Cross Return/Space/X, Circle C, Square Z/Left click, Triangle I/V, L1 Q/Mouse 5, R1 E/Right Alt, L2 R/Mouse 4, R2 Right click, "
        "L3 Left Shift/Right Shift, R3 Left Ctrl/Right Ctrl, Options Escape",
        "D-pad: up Up/Wheel up, right Right, down Down/Wheel down, left Left; touch pad: left Backspace, right Tab; fullscreen F11"};
    check(lines.size() == expected.size(), "three summary lines");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        check(lines[index] == expected[index], "summary line " + std::to_string(index + 1) + " reads '" + lines[index] + "'");
    }
    const auto custom = Pad::DescribeBindings(parse("LeftStickUp = KEY:Up\nRightStickUp = KEY:I\nRightStickUp = KEY:Keypad 8\nStickModifierScale = 0.3\nL2 = MOUSE:Right\n"));
    check(custom[0] == "Sticks: left Up A S D, right I F G H; Middle click/F7 puts the mouse on the right stick; hold Left Alt for 30% tilt", "summary follows the ini file: " + custom[0]);
    check(custom[1].find("L2 Right click") != std::string::npos, "summary shows a replaced binding");
}

void testMouseCaptureHint() {
    check(Pad::MouseCaptureHint(defaults(), false) == "Middle-click or press F7 to capture the mouse", "hint while free: " + Pad::MouseCaptureHint(defaults(), false));
    check(Pad::MouseCaptureHint(defaults(), true) == "Mouse captured, middle-click or press F7 to release", "hint while captured");
    const auto key = parse("ToggleMouse = KEY:F8\n");
    check(Pad::MouseCaptureHint(key, false) == "Press F8 to capture the mouse", "hint for a key");
    check(Pad::MouseCaptureHint(key, true) == "Mouse captured, press F8 to release", "captured hint for a key");
    check(Pad::MouseCaptureHint(parse("ToggleMouse = MOUSE:X1\n"), false) == "Press mouse button 4 to capture the mouse", "hint for a side button");
}

void testCaptureHotkeyAndSafety() {
    const auto speed = Pad::MouseFullTiltCountsPerSecond;
    std::vector<TimedEvent> events{At(0.0, KeyEvent(SDL_SCANCODE_F7, true)), At(1.0, KeyEvent(SDL_SCANCODE_F7, false))};
    PadSimulation::AppendMotion(events, 0.0, 100.0, speed, 0.0);
    events.push_back(At(100.0, KeyEvent(SDL_SCANCODE_F7, true)));
    PadSimulation::AppendMotion(events, 100.0, 100.0, speed, 0.0);
    const auto toggled = Simulate(defaults(), events, 1.0, 200.0);
    check(toggled[90].state.sticks[2] == 255, "F7 captures the mouse for the right stick");
    check(toggled[150].state.sticks[2] == 128, "F7 again releases it");

    std::vector<TimedEvent> focus{At(0.0, PadSimulation::ButtonEvent(SDL_BUTTON_MIDDLE, true)), At(0.0, KeyEvent(SDL_SCANCODE_W, true))};
    PadSimulation::AppendMotion(focus, 0.0, 200.0, speed, 0.0);
    focus.push_back(At(100.0, PadSimulation::FocusLostEvent()));
    std::stable_sort(focus.begin(), focus.end(), [](const TimedEvent& left, const TimedEvent& right) { return left.ms < right.ms; });
    const auto lost = Simulate(defaults(), focus, 1.0, 200.0);
    check(lost[90].state.sticks[2] == 255 && lost[90].state.sticks[1] == 0, "captured and walking before the focus loss");
    check(lost[150].state.sticks[2] == 128 && lost[150].state.sticks[1] == 128, "losing focus releases the mouse and every key");

    std::vector<TimedEvent> altTab{At(0.0, PadSimulation::ButtonEvent(SDL_BUTTON_MIDDLE, true)), At(100.0, KeyEvent(SDL_SCANCODE_LALT, true))};
    PadSimulation::AppendMotion(altTab, 0.0, 200.0, speed, 0.0);
    auto tab = KeyEvent(SDL_SCANCODE_TAB, true);
    tab.key.keysym.mod = KMOD_LALT;
    altTab.push_back(At(100.0, tab));
    std::stable_sort(altTab.begin(), altTab.end(), [](const TimedEvent& left, const TimedEvent& right) { return left.ms < right.ms; });
    const auto switched = Simulate(defaults(), altTab, 1.0, 200.0);
    check(switched[90].state.sticks[2] == 255, "captured before Alt+Tab");
    check(switched[150].state.sticks[2] == 128 && !switched[150].state.touchRight, "Alt+Tab releases the mouse and does not touch the touch pad");

    const auto key = parse("ToggleMouse = KEY:F8\n");
    check(Pad::MouseCaptureHint(key, false) == "Press F8 to capture the mouse", "a ToggleMouse line replaces both defaults");
    const auto both = parse("ToggleMouse = KEY:F8\nToggleMouse = MOUSE:X1\n");
    check(Pad::MouseCaptureHint(both, true) == "Mouse captured, press F8 or press mouse button 4 to release", "the hint lists every capture binding");
}

void testWheelPress() {
    const auto samples = Simulate(defaults(), {At(0.0, PadSimulation::WheelEvent(1))}, 1.0, 200.0);
    const auto up = static_cast<std::uint32_t>(Pad::PadButton::Up);
    check((samples[40].state.buttons & up) != 0, "wheel up presses Up");
    check((samples[120].state.buttons & up) == 0, "wheel press releases after its duration");
}

void testTraceRoundTrip() {
    std::ostringstream written;
    PadSimulation::WriteTraceLine(written, 0.0, PadSimulation::ButtonEvent(SDL_BUTTON_MIDDLE, true));
    PadSimulation::WriteTraceLine(written, 1.5, PadSimulation::MotionEvent(3, -2));
    PadSimulation::WriteTraceLine(written, 2.0, KeyEvent(SDL_SCANCODE_LSHIFT, true));
    PadSimulation::WriteTraceLine(written, 3.0, PadSimulation::WheelEvent(-1));
    std::istringstream read(written.str() + "# comment\n\n4 mouse off\n");
    const auto events = PadSimulation::ReadTrace(read);
    check(events.size() == 5, "trace keeps every event");
    check(events[1].event.type == SDL_MOUSEMOTION && events[1].event.motion.xrel == 3 && events[1].event.motion.yrel == -2 && events[1].ms == 1.5, "motion round trip");
    check(events[2].event.key.keysym.scancode == SDL_SCANCODE_LSHIFT, "key round trip");
    check(events[3].event.wheel.y == -1, "wheel round trip");
    check(events[4].setMouseMode && !events[4].mouseMode, "mouse mode line");
    std::istringstream bad("5 move 1\n");
    check(expectFailure([&bad] { PadSimulation::ReadTrace(bad); }).find("trace line 1") != std::string::npos, "malformed trace line");
}

int printBindings(int argc, char** argv) {
    constexpr const char* Usage = "usage: pad_keyboard_mouse_tests --bindings [--config ini | --dir directory [--title id]]";
    std::string configPath;
    std::string directory;
    std::string title;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--config" && index + 1 < argc) configPath = argv[++index];
        else if (argument == "--dir" && index + 1 < argc) directory = argv[++index];
        else if (argument == "--title" && index + 1 < argc) title = argv[++index];
        else throw std::runtime_error(Usage);
    }
    Pad::InputConfiguration configuration = Pad::DefaultInputConfiguration();
    if (!configPath.empty()) {
        std::ifstream file(configPath);
        if (!file) throw std::runtime_error("cannot read " + configPath);
        configuration = Pad::ParseInputConfiguration(file, configPath);
    } else if (!directory.empty()) {
        configuration = Pad::LoadInputConfiguration(directory, title);
    }
    for (const auto& source : configuration.sources) std::printf("Pad: input configuration %s\n", source.string().c_str());
    for (const auto& line : Pad::DescribeBindings(configuration)) std::printf("Pad: %s\n", line.c_str());
    std::printf("Title: %s\n", Pad::MouseCaptureHint(configuration, false).c_str());
    std::printf("Title while captured: %s\n", Pad::MouseCaptureHint(configuration, true).c_str());
    return 0;
}

int replay(int argc, char** argv) {
    constexpr const char* Usage = "usage: pad_keyboard_mouse_tests --replay <trace> [--poll ms] [--config ini | --dir directory [--title id]]";
    std::string tracePath;
    std::string configPath;
    std::string directory;
    std::string title;
    double poll = 8.0;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--poll" && index + 1 < argc) poll = std::stod(argv[++index]);
        else if (argument == "--config" && index + 1 < argc) configPath = argv[++index];
        else if (argument == "--dir" && index + 1 < argc) directory = argv[++index];
        else if (argument == "--title" && index + 1 < argc) title = argv[++index];
        else if (tracePath.empty()) tracePath = argv[index];
        else throw std::runtime_error(Usage);
    }
    if (tracePath.empty() || !(poll > 0.0) || (!configPath.empty() && !directory.empty())) throw std::runtime_error(Usage);
    Pad::InputConfiguration configuration = Pad::DefaultInputConfiguration();
    if (!configPath.empty()) {
        std::ifstream file(configPath);
        if (!file) throw std::runtime_error("cannot read " + configPath);
        configuration = Pad::ParseInputConfiguration(file, configPath);
    } else if (!directory.empty()) {
        configuration = Pad::LoadInputConfiguration(directory, title);
    }
    for (const auto& source : configuration.sources) std::fprintf(stderr, "input configuration %s\n", source.string().c_str());
    std::ifstream trace(tracePath);
    if (!trace) throw std::runtime_error("cannot read " + tracePath);
    const auto events = PadSimulation::ReadTrace(trace);
    const double end = events.empty() ? poll : events.back().ms + 500.0;
    std::printf("ms,lx,ly,rx,ry,l2,r2,buttons\n");
    for (const auto& sample : Simulate(configuration, events, poll, end)) {
        const auto& state = sample.state;
        std::printf("%.3f,%u,%u,%u,%u,%u,%u,0x%06x\n", sample.ms, state.sticks[0], state.sticks[1], state.sticks[2], state.sticks[3],
            state.analogButtonsL2, state.analogButtonsR2, static_cast<unsigned>(state.buttons));
    }
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--replay") return replay(argc, argv);
        if (argc > 1 && std::string_view(argv[1]) == "--bindings") return printBindings(argc, argv);
        const std::pair<const char*, std::function<void()>> tests[] = {
            {"defaults", testDefaults},
            {"settings parse", testSettingsParse},
            {"centered without motion", testCenteredWithoutMotion},
            {"deflection curve", testDeflectionCurve},
            {"deadzone offset", testDeadzoneOffset},
            {"decay", testDecay},
            {"max speed clamp", testMaxSpeedClamp},
            {"smoothing", testSmoothing},
            {"poll rate independence", testPollRateIndependence},
            {"vertical scale", testVerticalScale},
            {"mouse mode off", testMouseModeOff},
            {"keyboard sticks unchanged", testKeyboardSticksUnchanged},
            {"keyboard settings parse", testKeyboardSettingsParse},
            {"stick modifier", testStickModifier},
            {"keyboard ramp", testKeyboardRamp},
            {"ramp with modifier and release", testRampWithModifierAndRelease},
            {"profiles", testProfiles},
            {"opposite keys with alternates", testOppositeKeysWithAlternates},
            {"slow reporting mouse", testSlowReportingMouse},
            {"replay release", testReplayRelease},
            {"default bindings", testDefaultBindings},
            {"describe bindings", testDescribeBindings},
            {"mouse capture hint", testMouseCaptureHint},
            {"capture hotkey and safety", testCaptureHotkeyAndSafety},
            {"wheel press", testWheelPress},
            {"trace round trip", testTraceRoundTrip},
        };
        for (const auto& [name, test] : tests) {
            test();
            std::printf("pad_keyboard_mouse: %s passed\n", name);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pad_keyboard_mouse: %s\n", error.what());
        return 1;
    }
    return 0;
}
