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
    InputSetting{"KeyboardRampMs", &Pad::InputSettings::keyboardRampMs, 0.0, 2000.0},
    InputSetting{"TriggerRampMs", &Pad::InputSettings::triggerRampMs, 0.0, 2000.0},
    InputSetting{"TriggerWheelStep", &Pad::InputSettings::triggerWheelStep, 0.0, 1.0}
};

struct InputChoice {
    std::string_view name;
    std::array<std::string_view, 3> values;
    void (*apply)(Pad::InputSettings& settings, std::size_t value);
};

constexpr auto choices = std::array{
    InputChoice{"KeyboardMouseTarget", {"both", "pad", "native"}, [](Pad::InputSettings& settings, std::size_t value) {
        settings.keyboardMouseTarget = static_cast<Pad::KeyboardMouseTarget>(value);
    }},
    InputChoice{"MouseStick", {"right", "left", ""}, [](Pad::InputSettings& settings, std::size_t value) {
        settings.mouseStick = static_cast<Pad::MouseStickSide>(value);
    }}
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

const InputChoice* findChoice(std::string_view name) {
    const auto normalized = upper(name);
    for (const auto& choice : choices) {
        if (upper(choice.name) == normalized) return &choice;
    }
    return nullptr;
}

void applyChoice(const InputChoice& choice, std::string_view value, Pad::InputSettings& settings) {
    std::string allowed;
    std::size_t count = 0;
    for (std::size_t index = 0; index < choice.values.size(); ++index) {
        if (choice.values[index].empty()) continue;
        if (upper(choice.values[index]) == upper(value)) {
            choice.apply(settings, index);
            return;
        }
        ++count;
    }
    for (std::size_t index = 0; index < count; ++index) {
        allowed += std::string(index == 0 ? "" : index + 1 == count ? " or " : ", ") + std::string(choice.values[index]);
    }
    throw std::runtime_error(std::string(choice.name) + " must be " + allowed);
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

void loadLayer(Pad::InputConfiguration& configuration, const std::filesystem::path& path, bool required) {
    std::ifstream file(path);
    if (!file) {
        if (required || std::filesystem::exists(path)) {
            throw std::runtime_error("Pad: cannot read input mapping '" + path.string() + "'");
        }
        return;
    }
    configuration = Pad::ParseInputConfiguration(file, path, std::move(configuration));
}

}

Pad::InputConfiguration Pad::DefaultInputConfiguration() {
    return InputConfiguration{{InputMapping.begin(), InputMapping.end()}, {}, {}};
}

std::filesystem::path Pad::DefaultInputDirectory() {
    std::unique_ptr<char, decltype(&SDL_free)> basePath(SDL_GetBasePath(), SDL_free);
    if (basePath) return std::filesystem::path(basePath.get());
    return {};
}

std::string Pad::InputProfileName(std::string_view titleId) {
    const bool valid = !titleId.empty() && titleId.size() <= 32 && std::all_of(titleId.begin(), titleId.end(), [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '-';
    });
    return valid ? "anyps5-input." + std::string(titleId) + ".ini" : std::string();
}

Pad::InputConfiguration Pad::ParseInputConfiguration(std::istream& stream, const std::filesystem::path& path, InputConfiguration base) {
    InputConfiguration configuration = std::move(base);
    configuration.sources.push_back(path);
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
        if (const auto* choice = findChoice(actionName)) {
            if (!givenSettings.insert(upper(choice->name)).second) invalidLine(path, lineNumber, std::string(choice->name) + " is given twice");
            try {
                applyChoice(*choice, source, configuration.settings);
            } catch (const std::runtime_error& error) {
                invalidLine(path, lineNumber, error.what());
            }
            continue;
        }
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

std::string Pad::SourceName(const InputBinding& binding) {
    if (binding.key != SDL_SCANCODE_UNKNOWN) {
        const char* name = SDL_GetScancodeName(binding.key);
        return name != nullptr && name[0] != '\0' ? std::string(name) : "key " + std::to_string(static_cast<int>(binding.key));
    }
    switch (binding.mouseButton) {
        case MouseButton::Left: return "Left click";
        case MouseButton::Middle: return "Middle click";
        case MouseButton::Right: return "Right click";
        case MouseButton::X1: return "Mouse 4";
        case MouseButton::X2: return "Mouse 5";
        case MouseButton::None: break;
    }
    if (binding.wheelDirection > 0) return "Wheel up";
    if (binding.wheelDirection < 0) return "Wheel down";
    return "nothing";
}

std::vector<std::string> Pad::DescribeBindings(const InputConfiguration& configuration) {
    const auto sources = [&configuration](InputControl control, PadButton button) {
        std::string text;
        for (const auto& binding : configuration.bindings) {
            if (binding.control != control || (control == InputControl::Button && binding.button != button)) continue;
            if (!text.empty()) text += "/";
            text += SourceName(binding);
        }
        return text;
    };
    const auto stick = [&sources](InputControl up, InputControl left, InputControl down, InputControl right) {
        std::string text;
        for (const auto control : {up, left, down, right}) {
            auto source = sources(control, PadButton::None);
            const auto separator = source.find('/');
            if (separator != std::string::npos) source.resize(separator);
            text += (text.empty() ? "" : " ") + (source.empty() ? std::string("-") : source);
        }
        return text;
    };
    std::string sticks = "Sticks: left " + stick(InputControl::LeftStickUp, InputControl::LeftStickLeft, InputControl::LeftStickDown, InputControl::LeftStickRight) +
        ", right " + stick(InputControl::RightStickUp, InputControl::RightStickLeft, InputControl::RightStickDown, InputControl::RightStickRight);
    const auto toggle = sources(InputControl::ToggleMouse, PadButton::None);
    const char* mouseSide = configuration.settings.mouseStick == MouseStickSide::Left ? "left" : "right";
    if (!toggle.empty()) sticks += "; " + toggle + " puts the mouse on the " + mouseSide + " stick";
    const auto modifier = sources(InputControl::StickModifier, PadButton::None);
    if (!modifier.empty()) sticks += "; hold " + modifier + " for " + std::to_string(std::lround(configuration.settings.stickModifierScale * 100.0)) + "% tilt";

    struct Named {
        const char* name;
        PadButton button;
    };
    std::string buttons = "Buttons:";
    for (const auto& [name, button] : {Named{"Cross", PadButton::Cross}, Named{"Circle", PadButton::Circle}, Named{"Square", PadButton::Square},
             Named{"Triangle", PadButton::Triangle}, Named{"L1", PadButton::L1}, Named{"R1", PadButton::R1}, Named{"L2", PadButton::L2},
             Named{"R2", PadButton::R2}, Named{"L3", PadButton::L3}, Named{"R3", PadButton::R3}, Named{"Options", PadButton::Options}}) {
        const auto source = sources(InputControl::Button, button);
        if (!source.empty()) buttons += std::string(buttons.back() == ':' ? " " : ", ") + name + " " + source;
    }

    std::string other = "D-pad:";
    for (const auto& [name, button] : {Named{"up", PadButton::Up}, Named{"right", PadButton::Right}, Named{"down", PadButton::Down}, Named{"left", PadButton::Left}}) {
        const auto source = sources(InputControl::Button, button);
        other += std::string(other.back() == ':' ? " " : ", ") + name + " " + (source.empty() ? std::string("-") : source);
    }
    const auto touchLeft = sources(InputControl::TouchLeft, PadButton::None);
    const auto touchRight = sources(InputControl::TouchRight, PadButton::None);
    if (!touchLeft.empty() || !touchRight.empty()) {
        other += "; touch pad: left " + (touchLeft.empty() ? std::string("-") : touchLeft) + ", right " + (touchRight.empty() ? std::string("-") : touchRight);
    }
    const auto fullscreen = sources(InputControl::ToggleFullscreen, PadButton::None);
    if (!fullscreen.empty()) other += "; fullscreen " + fullscreen;
    std::vector<std::string> lines{sticks, buttons, other};
    if (configuration.settings.keyboardMouseTarget == KeyboardMouseTarget::Pad) {
        lines.push_back("Target: pad only; the game's own keyboard and mouse support gets nothing");
    } else if (configuration.settings.keyboardMouseTarget == KeyboardMouseTarget::Native) {
        lines.push_back("Target: native only; keys and the mouse reach the game's own keyboard and mouse support, not the pad");
    }
    return lines;
}

bool Pad::IsKeyboardMouseEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEWHEEL:
            return true;
        default:
            return false;
    }
}

bool Pad::DeliverToNative(const InputSettings& settings, const SDL_Event& event) {
    return settings.keyboardMouseTarget != KeyboardMouseTarget::Pad || !IsKeyboardMouseEvent(event);
}

std::string Pad::MouseCaptureHint(const InputConfiguration& configuration, bool captured) {
    std::string action;
    for (const auto& binding : configuration.bindings) {
        if (binding.control != InputControl::ToggleMouse) continue;
        if (!action.empty()) action += " or ";
        switch (binding.mouseButton) {
            case MouseButton::Left: action += "left-click"; break;
            case MouseButton::Middle: action += "middle-click"; break;
            case MouseButton::Right: action += "right-click"; break;
            case MouseButton::X1: action += "press mouse button 4"; break;
            case MouseButton::X2: action += "press mouse button 5"; break;
            case MouseButton::None: action += "press " + SourceName(binding); break;
        }
    }
    if (action.empty()) return {};
    if (captured) return "Mouse captured, " + action + " to release";
    action[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(action[0])));
    return action + " to capture the mouse";
}

Pad::InputConfiguration Pad::LoadInputConfiguration(const std::filesystem::path& directory, std::string_view titleId) {
    auto configuration = DefaultInputConfiguration();
    const char* configuredPath = std::getenv("ANYPS5_INPUT_CONFIG");
    const bool explicitPath = configuredPath != nullptr && configuredPath[0] != '\0';
    loadLayer(configuration, explicitPath ? std::filesystem::path(configuredPath) : directory / "anyps5-input.ini", explicitPath);
    const auto profile = InputProfileName(titleId);
    if (!profile.empty()) loadLayer(configuration, directory / profile, false);
    return configuration;
}
