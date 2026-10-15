#ifndef CORE_LIBS_LABDISPLAY_HPP
#define CORE_LIBS_LABDISPLAY_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace LabDisplay {

inline bool EnvOn(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

inline bool LogCalls() {
    static const bool value = EnvOn("APS5_LAB_DISPLAY_LOG");
    return value;
}

inline bool Tv1080() {
    static const bool value = EnvOn("APS5_LAB_DISPLAY_TV");
    return value;
}

inline bool Force1080() {
    static const bool value = EnvOn("APS5_LAB_DISPLAY_1080") || Tv1080();
    return value;
}

inline bool ForceSdr() {
    static const bool value = EnvOn("APS5_LAB_DISPLAY_SDR") || Tv1080();
    return value;
}

inline bool Force60Hz() {
    static const bool value = EnvOn("APS5_LAB_DISPLAY_60HZ") || Tv1080();
    return value;
}

inline int ResolutionCode() {
    static const int value = [] {
        const char* text = std::getenv("APS5_LAB_DISPLAY_RES_CODE");
        return text != nullptr && text[0] != '\0' ? std::atoi(text) : -1;
    }();
    return value;
}

inline bool ViewportCensus() {
    static const bool value = EnvOn("APS5_LAB_VIEWPORT_CENSUS");
    return value;
}

inline std::uint64_t GpuClockDivisor() {
    static const std::uint64_t value = [] {
        const char* text = std::getenv("APS5_LAB_GPU_CLOCK_DIV");
        const long long parsed = text != nullptr && text[0] != '\0' ? std::atoll(text) : 1;
        return parsed > 0 ? static_cast<std::uint64_t>(parsed) : std::uint64_t{1};
    }();
    return value;
}

inline std::atomic<std::uint64_t>& GpuClockReads(int kind) {
    static std::atomic<std::uint64_t> counts[3];
    return counts[kind];
}

inline std::uint64_t GpuClock(int kind, std::uint64_t ticks) {
    GpuClockReads(kind).fetch_add(1, std::memory_order_relaxed);
    return ticks / GpuClockDivisor();
}

inline long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline unsigned long long ThreadTag() {
    return static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffffull);
}

inline void Log(const char* function, std::atomic<std::uint64_t>& calls, std::uintptr_t caller, const char* format, ...) {
    const std::uint64_t number = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (number > 64 && (number & (number - 1)) != 0) return;
    char body[768];
    std::va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(body, sizeof(body), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "[display] t=%lld tid=%06llx ret=0x%llx n=%llu %s %s\n", NowMs(), ThreadTag(), static_cast<unsigned long long>(caller), static_cast<unsigned long long>(number), function, body);
}

inline void NoteDraw(std::uint32_t renderWidth, std::uint32_t renderHeight, float viewportWidth, float viewportHeight, std::uint32_t scissorWidth, std::uint32_t scissorHeight) {
    if (renderWidth < 256 || renderHeight < 256) return;
    static std::mutex mutex;
    static std::map<std::array<std::uint32_t, 6>, std::uint64_t> counts;
    static long long windowStart = 0;
    static std::uint64_t windowDraws = 0;
    std::lock_guard lock(mutex);
    const long long now = NowMs();
    if (windowStart == 0) windowStart = now;
    const std::array<std::uint32_t, 6> key{{renderWidth, renderHeight, static_cast<std::uint32_t>(std::fabs(viewportWidth) + 0.5f), static_cast<std::uint32_t>(std::fabs(viewportHeight) + 0.5f), scissorWidth, scissorHeight}};
    counts[key] += 1;
    ++windowDraws;
    if (now - windowStart < 10000) return;
    std::vector<std::pair<std::uint64_t, std::array<std::uint32_t, 6>>> sorted;
    sorted.reserve(counts.size());
    for (const auto& entry : counts) sorted.emplace_back(entry.second, entry.first);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "[lab-census] t=%lld window=%lld ms draws=%llu (render targets >= 256 px; render extent / viewport / scissor)\n", now, now - windowStart, static_cast<unsigned long long>(windowDraws));
    std::fprintf(stderr, "[lab-census]   GPU clock reads by the title in the window: copy-data %llu, release-mem (queued) %llu, release-mem (executed) %llu\n", static_cast<unsigned long long>(GpuClockReads(0).exchange(0)), static_cast<unsigned long long>(GpuClockReads(1).exchange(0)), static_cast<unsigned long long>(GpuClockReads(2).exchange(0)));
    for (std::size_t i = 0; i < sorted.size() && i < 14; ++i) {
        const auto& key = sorted[i].second;
        std::fprintf(stderr, "[lab-census]   render %ux%u viewport %ux%u scissor %ux%u draws %llu\n", key[0], key[1], key[2], key[3], key[4], key[5], static_cast<unsigned long long>(sorted[i].first));
    }
    counts.clear();
    windowDraws = 0;
    windowStart = now;
}

}

#define APS5_LAB_DISPLAY_LOG(function, ...) \
    do { \
        if (LabDisplay::LogCalls()) { \
            static std::atomic<std::uint64_t> labCalls{0}; \
            LabDisplay::Log(function, labCalls, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)), __VA_ARGS__); \
        } \
    } while (0)

#endif
