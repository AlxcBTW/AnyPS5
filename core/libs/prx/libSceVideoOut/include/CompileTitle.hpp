#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_COMPILETITLE_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_COMPILETITLE_HPP

#include <cstdint>
#include <string>
#include <string_view>

inline constexpr std::uint64_t CompileTitleIdleMs = 1000;
inline constexpr std::uint64_t CompileTitleLingerMs = 2000;
inline constexpr std::uint64_t CompileTitleIntervalMs = 500;

struct CompileTitleInput {
    std::uint64_t nowMs;
    std::uint64_t lastPresentMs;
    std::uint64_t completed;
    std::uint32_t active;
    std::uint64_t lastBeginMs;
    std::uint64_t lastEndMs;
    std::uint64_t lastDurationMs;
};

bool CompileTitleVisible(const CompileTitleInput& input);
std::string FormatCompileTitle(std::string_view baseTitle, const CompileTitleInput& input);

#endif
