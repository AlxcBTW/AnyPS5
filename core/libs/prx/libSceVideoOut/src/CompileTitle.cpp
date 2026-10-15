#include "prx/libSceVideoOut/include/CompileTitle.hpp"
#include <cstdio>

bool CompileTitleVisible(const CompileTitleInput& input) {
    if (input.completed == 0 && input.active == 0) return false;
    if (input.nowMs < input.lastPresentMs || input.nowMs - input.lastPresentMs <= CompileTitleIdleMs) return false;
    if (input.active != 0) return true;
    return input.nowMs >= input.lastEndMs && input.nowMs - input.lastEndMs < CompileTitleLingerMs;
}

std::string FormatCompileTitle(std::string_view baseTitle, const CompileTitleInput& input) {
    const auto lastMs = input.active != 0 ? (input.nowMs > input.lastBeginMs ? input.nowMs - input.lastBeginMs : 0) : input.lastDurationMs;
    char done[48];
    if (input.expected != 0) std::snprintf(done, sizeof(done), "%llu / ~%llu", static_cast<unsigned long long>(input.completed), static_cast<unsigned long long>(input.expected));
    else std::snprintf(done, sizeof(done), "%llu", static_cast<unsigned long long>(input.completed));
    char text[128];
    std::snprintf(text, sizeof(text), " | compiling shaders: %s done, %.1f s on the last one", done, static_cast<double>(lastMs) / 1000.0);
    return std::string(baseTitle) + text;
}
