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

inline double GpuClockScale() {
    static const double value = [] {
        double scale = 1.0;
        const char* multiply = std::getenv("APS5_LAB_GPU_CLOCK_SCALE");
        if (multiply != nullptr && multiply[0] != '\0' && std::atof(multiply) > 0.0) scale = std::atof(multiply);
        const char* divide = std::getenv("APS5_LAB_GPU_CLOCK_DIV");
        if (divide != nullptr && divide[0] != '\0' && std::atof(divide) > 0.0) scale /= std::atof(divide);
        return scale;
    }();
    return value;
}

inline long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline unsigned long long ThreadTag() {
    return static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffffull);
}

inline std::atomic<std::uint64_t>& GpuClockReads(int kind) {
    static std::atomic<std::uint64_t> counts[3];
    return counts[kind];
}

struct StampGaps {
    std::atomic<std::uint64_t> buckets[8];
    std::atomic<std::uint64_t> maxTicks;
    std::atomic<std::uint64_t> sumTicks;
    std::atomic<std::uint64_t> count;
};

struct StampSlot {
    std::atomic<std::uint64_t> tag;
    StampGaps gaps[3];
};

constexpr int StampSlots = 16;

inline StampSlot* AllStampSlots() {
    static StampSlot slots[StampSlots];
    return slots;
}

inline StampSlot& MyStampSlot() {
    static std::atomic<int> used{0};
    thread_local StampSlot* mine = nullptr;
    if (mine == nullptr) {
        const int index = used.fetch_add(1, std::memory_order_relaxed);
        mine = &AllStampSlots()[index < StampSlots ? index : StampSlots - 1];
        mine->tag.store(ThreadTag() + 1, std::memory_order_relaxed);
    }
    return *mine;
}

inline void NoteStamp(int kind, std::uint64_t ticks) {
    thread_local std::uint64_t last[3] = {0, 0, 0};
    const std::uint64_t previous = last[kind];
    last[kind] = ticks;
    if (previous == 0 || ticks < previous) return;
    const std::uint64_t gap = ticks - previous;
    static constexpr std::uint64_t limits[7] = {1000, 10000, 100000, 500000, 1667000, 5000000, 15000000};
    std::size_t bucket = 0;
    while (bucket < 7 && gap >= limits[bucket]) ++bucket;
    StampGaps& gaps = MyStampSlot().gaps[kind];
    gaps.buckets[bucket].fetch_add(1, std::memory_order_relaxed);
    gaps.count.fetch_add(1, std::memory_order_relaxed);
    gaps.sumTicks.fetch_add(gap, std::memory_order_relaxed);
    std::uint64_t seen = gaps.maxTicks.load(std::memory_order_relaxed);
    while (gap > seen && !gaps.maxTicks.compare_exchange_weak(seen, gap, std::memory_order_relaxed)) {
    }
}

inline std::uint64_t ScaleTicks(std::uint64_t ticks) {
    const double scale = GpuClockScale();
    if (scale == 1.0) return ticks;
    static const std::uint64_t anchor = ticks;
    const double delta = static_cast<double>(static_cast<std::int64_t>(ticks) - static_cast<std::int64_t>(anchor));
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(anchor) + static_cast<std::int64_t>(delta * scale));
}

inline std::uint64_t GpuClock(int kind, std::uint64_t ticks) {
    GpuClockReads(kind).fetch_add(1, std::memory_order_relaxed);
    NoteStamp(kind, ticks);
    return ScaleTicks(ticks);
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
    static const char* const kindNames[3] = {"copy-data clock", "release-mem timestamp (queued)", "release-mem timestamp (executed)"};
    for (int slot = 0; slot < StampSlots; ++slot) {
        StampSlot& owner = AllStampSlots()[slot];
        const std::uint64_t tag = owner.tag.load(std::memory_order_relaxed);
        for (int kind = 0; kind < 3; ++kind) {
            StampGaps& gaps = owner.gaps[kind];
            const std::uint64_t count = gaps.count.exchange(0);
            std::uint64_t b[8];
            for (int i = 0; i < 8; ++i) b[i] = gaps.buckets[i].exchange(0);
            const double maxMs = static_cast<double>(gaps.maxTicks.exchange(0)) / 100000.0;
            const double meanMs = count != 0 ? static_cast<double>(gaps.sumTicks.exchange(0)) / 100000.0 / static_cast<double>(count) : 0.0;
            if (count == 0) continue;
            std::fprintf(stderr, "[lab-census]   %s: gaps between consecutive stamps of one thread tid=%06llx (real time, clock scale %.3f) n=%llu mean %.2f ms max %.2f ms; <10us %llu <100us %llu <1ms %llu <5ms %llu <16.7ms %llu <50ms %llu <150ms %llu >=150ms %llu\n", kindNames[kind], static_cast<unsigned long long>(tag - 1), GpuClockScale(), static_cast<unsigned long long>(count), meanMs, maxMs, static_cast<unsigned long long>(b[0]), static_cast<unsigned long long>(b[1]), static_cast<unsigned long long>(b[2]), static_cast<unsigned long long>(b[3]), static_cast<unsigned long long>(b[4]), static_cast<unsigned long long>(b[5]), static_cast<unsigned long long>(b[6]), static_cast<unsigned long long>(b[7]));
        }
    }
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
