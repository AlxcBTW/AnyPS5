#include "prx/libScePad/include/InputMapping.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

#include "SDL_keyboard.h"
#include "SDL_filesystem.h"

namespace {

struct InputAction {
    std::string_view name;
    Pad::InputControl control;
    Pad::PadButton button;
};

constexpr auto actions = std::array{
    InputAction{"Cross", Pad::InputControl::Button, Pad::PadButton::Cross},
    InputAction{"Circle", Pad::InputControl::Button, Pad::PadButton::Circle},
    InputAction{"Triangle", Pad::InputControl::Button, Pad::PadButton::Triangle},
    InputAction{"Square", Pad::InputControl::Button, Pad::PadButton::Square},
    InputAction{"L1", Pad::InputControl::Button, Pad::PadButton::L1},
    InputAction{"R1", Pad::InputControl::Button, Pad::PadButton::R1},
    InputAction{"L2", Pad::InputControl::Button, Pad::PadButton::L2},
    InputAction{"R2", Pad::InputControl::Button, Pad::PadButton::R2},
    InputAction{"L3", Pad::InputControl::Button, Pad::PadButton::L3},
    InputAction{"R3", Pad::InputControl::Button, Pad::PadButton::R3},
    InputAction{"Options", Pad::InputControl::Button, Pad::PadButton::Options},
    InputAction{"Up", Pad::InputControl::Button, Pad::PadButton::Up},
    InputAction{"Right", Pad::InputControl::Button, Pad::PadButton::Right},
    InputAction{"Down", Pad::InputControl::Button, Pad::PadButton::Down},
    InputAction{"Left", Pad::InputControl::Button, Pad::PadButton::Left},
    InputAction{"LeftStickLeft", Pad::InputControl::LeftStickLeft, Pad::PadButton::None},
    InputAction{"LeftStickRight", Pad::InputControl::LeftStickRight, Pad::PadButton::None},
    InputAction{"LeftStickUp", Pad::InputControl::LeftStickUp, Pad::PadButton::None},
    InputAction{"LeftStickDown", Pad::InputControl::LeftStickDown, Pad::PadButton::None},
    InputAction{"RightStickLeft", Pad::InputControl::RightStickLeft, Pad::PadButton::None},
    InputAction{"RightStickRight", Pad::InputControl::RightStickRight, Pad::PadButton::None},
    InputAction{"RightStickUp", Pad::InputControl::RightStickUp, Pad::PadButton::None},
    InputAction{"RightStickDown", Pad::InputControl::RightStickDown, Pad::PadButton::None},
    InputAction{"TouchLeft", Pad::InputControl::TouchLeft, Pad::PadButton::None},
    InputAction{"TouchRight", Pad::InputControl::TouchRight, Pad::PadButton::None},
    InputAction{"StickModifier", Pad::InputControl::StickModifier, Pad::PadButton::None},
    InputAction{"ToggleMouse", Pad::InputControl::ToggleMouse, Pad::PadButton::None},
    InputAction{"ToggleFullscreen", Pad::InputControl::ToggleFullscreen, Pad::PadButton::None}
};

struct InputSetting {
    std::string_view name;
    double Pad::InputSettings::* field;
    double minimum;
    double maximum;
};

constexpr auto settings = std::array{
    InputSetting{"MouseSensitivity", &Pad::InputSettings::mouseSensitivity, 0.01, 100.0},
    InputSetting{"MouseVerticalScale", &Pad::InputSettings::mouseVerticalScale, 0.1, 10.0},
    InputSetting{"MouseCurve", &Pad::InputSettings::mouseCurve, 0.2, 5.0},
    InputSetting{"MouseDeadzoneOffset", &Pad::InputSettings::mouseDeadzoneOffset, 0.0, 0.9},
    InputSetting{"MouseMaxSpeed", &Pad::InputSettings::mouseMaxSpeed, 0.1, 10.0},
    InputSetting{"MouseSmoothingMs", &Pad::InputSettings::mouseSmoothingMs, 0.0, 500.0},
    InputSetting{"MouseDecayMs", &Pad::InputSettings::mouseDecayMs, 1.0, 1000.0},
    InputSetting{"StickModifierScale", &Pad::InputSettings::stickModifierScale, 0.05, 1.0},
    InputSetting{"KeyboardRampMs", &Pad::InputSettings::keyboardRampMs, 0.0, 2000.0}
};

std::string upper(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) value.remove_suffix(1);
    return value;
}

const InputSetting* findSetting(std::string_view name) {
    const auto normalized = upper(name);
    for (const auto& setting : settings) {
        if (upper(setting.name) == normalized) return &setting;
    }
    return nullptr;
}

std::string formatNumber(double value) {
    std::array<char, 32> buffer{};
    const auto [last, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return error == std::errc{} ? std::string(buffer.data(), last) : std::to_string(value);
}

double parseSetting(const InputSetting& setting, std::string_view value) {
    double number = 0.0;
    const auto* end = value.data() + value.size();
    const auto [last, error] = std::from_chars(value.data(), end, number);
    if (value.empty() || error != std::errc{} || last != end || !std::isfinite(number)) {
        throw std::runtime_error(std::string(setting.name) + " must be a number");
    }
    if (number < setting.minimum || number > setting.maximum) {
        throw std::runtime_error(std::string(setting.name) + " must be between " + formatNumber(setting.minimum) + " and " + formatNumber(setting.maximum));
    }
    return number;
}

const InputAction* findAction(std::string_view name) {
    const auto normalized = upper(name);
    for (const auto& action : actions) {
        if (upper(action.name) == normalized) return &action;
    }
    return nullptr;
}

bool matchesAction(const Pad::InputBinding& binding, const InputAction& action) {
    if (action.control == Pad::InputControl::Button) {
        return binding.control == Pad::InputControl::Button && binding.button == action.button;
    }
    return binding.control == action.control;
}

Pad::MouseButton parseMouseButton(std::string_view name) {
    const auto normalized = upper(name);
    if (normalized == "LEFT") return Pad::MouseButton::Left;
    if (normalized == "MIDDLE") return Pad::MouseButton::Middle;
    if (normalized == "RIGHT") return Pad::MouseButton::Right;
    if (normalized == "X1") return Pad::MouseButton::X1;
    if (normalized == "X2") return Pad::MouseButton::X2;
    throw std::runtime_error("unknown mouse button '" + std::string(name) + "'");
}

Pad::InputBinding parseBinding(const InputAction& action, std::string_view source) {
    const auto separator = source.find(':');
    if (separator == std::string_view::npos) throw std::runtime_error("source must use TYPE:VALUE format");
    const auto type = upper(trim(source.substr(0, separator)));
    const auto value = trim(source.substr(separator + 1));
    if (value.empty()) throw std::runtime_error("source value is empty");

    if (type == "KEY") {
        const std::string keyName(value);
        const auto key = SDL_GetScancodeFromName(keyName.c_str());
        if (key == SDL_SCANCODE_UNKNOWN) throw std::runtime_error("unknown SDL key name '" + keyName + "'");
        return {key, Pad::MouseButton::None, action.control, action.button, 0};
    }
    if (type == "MOUSE") {
        if (action.control == Pad::InputControl::ToggleFullscreen) {
            throw std::runtime_error("ToggleFullscreen can only use a keyboard key");
        }
        return {SDL_SCANCODE_UNKNOWN, parseMouseButton(value), action.control, action.button, 0};
    }
    if (type == "WHEEL") {
        if (action.control != Pad::InputControl::Button) throw std::runtime_error("mouse wheel can only map to a pad button");
        const auto direction = upper(value);
        if (direction == "UP") return {SDL_SCANCODE_UNKNOWN, Pad::MouseButton::None, action.control, action.button, 1};
        if (direction == "DOWN") return {SDL_SCANCODE_UNKNOWN, Pad::MouseButton::None, action.control, action.button, -1};
        throw std::runtime_error("mouse wheel direction must be UP or DOWN");
    }
    throw std::runtime_error("source type must be KEY, MOUSE or WHEEL");
}

[[noreturn]] void invalidLine(const std::filesystem::path& path, std::size_t line, const std::string& reason) {
    throw std::runtime_error("Pad: invalid input mapping " + path.string() + ":" + std::to_string(line) + ": " + reason);
}

std::filesystem::path defaultConfigPath() {
    std::unique_ptr<char, decltype(&SDL_free)> basePath(SDL_GetBasePath(), SDL_free);
    if (basePath) return std::filesystem::path(basePath.get()) / "anyps5-input.ini";
    return "anyps5-input.ini";
}

}

Pad::InputConfiguration Pad::ParseInputConfiguration(std::istream& stream, const std::filesystem::path& path) {
    InputConfiguration configuration{{InputMapping.begin(), InputMapping.end()}, {}, path};
    auto& bindings = configuration.bindings;
    std::unordered_set<std::string> overriddenActions;
    std::unordered_set<std::string> givenSettings;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        if (lineNumber == 1 && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        const auto comment = line.find_first_of("#;");
        const std::string_view content = trim(std::string_view(line).substr(0, comment));
        if (content.empty()) continue;
        const auto separator = content.find('=');
        if (separator == std::string_view::npos) invalidLine(path, lineNumber, "expected Action = TYPE:VALUE or Setting = number");

        const auto actionName = trim(content.substr(0, separator));
        const auto source = trim(content.substr(separator + 1));
        if (const auto* setting = findSetting(actionName)) {
            if (!givenSettings.insert(upper(setting->name)).second) invalidLine(path, lineNumber, std::string(setting->name) + " is given twice");
            try {
                configuration.settings.*(setting->field) = parseSetting(*setting, source);
            } catch (const std::runtime_error& error) {
                invalidLine(path, lineNumber, error.what());
            }
            continue;
        }
        const auto* action = findAction(actionName);
        if (action == nullptr) invalidLine(path, lineNumber, "unknown action or setting '" + std::string(actionName) + "'");
        if (source.empty()) invalidLine(path, lineNumber, "source is empty");

        try {
            auto binding = parseBinding(*action, source);
            const auto normalizedAction = upper(action->name);
            if (overriddenActions.insert(normalizedAction).second) {
                std::erase_if(bindings, [action](const InputBinding& existing) { return matchesAction(existing, *action); });
            }
            const bool duplicate = std::any_of(bindings.begin(), bindings.end(), [&binding](const InputBinding& existing) {
                return existing.key == binding.key && existing.mouseButton == binding.mouseButton &&
                    existing.control == binding.control && existing.button == binding.button &&
                    existing.wheelDirection == binding.wheelDirection;
            });
            if (!duplicate) bindings.push_back(binding);
        } catch (const std::runtime_error& error) {
            invalidLine(path, lineNumber, error.what());
        }
    }
    if (stream.bad()) throw std::runtime_error("Pad: cannot read input mapping '" + path.string() + "'");

    return configuration;
}

Pad::InputConfiguration Pad::LoadInputConfiguration() {
    const char* configuredPath = std::getenv("ANYPS5_INPUT_CONFIG");
    const bool explicitPath = configuredPath != nullptr && configuredPath[0] != '\0';
    const std::filesystem::path path = explicitPath ? configuredPath : defaultConfigPath();
    std::ifstream file(path);
    if (!file) {
        if (explicitPath || std::filesystem::exists(path)) {
            throw std::runtime_error("Pad: cannot read input mapping '" + path.string() + "'");
        }
        return InputConfiguration{{InputMapping.begin(), InputMapping.end()}, {}, {}};
    }
    return ParseInputConfiguration(file, path);
}
