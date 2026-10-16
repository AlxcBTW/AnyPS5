#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

double TraceMs() {
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count();
}

std::uint64_t SkippedShader() {
    static const std::uint64_t skipped = [] { const char* text = std::getenv("APS5_SKIP_SHADER"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();
    return skipped;
}

namespace {

std::uint64_t LabCodeHash(std::span<const std::uint32_t> code) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : code) {
        hash ^= word;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

struct LabCodeSwitches {
    bool trace = false;
    std::vector<std::uint64_t> skip;

    LabCodeSwitches() {
        trace = std::getenv("APS5_LAB_TRACE_CODE") != nullptr;
        const char* text = std::getenv("APS5_LAB_SKIP_CODE");
        for (const char* p = text; p != nullptr && *p != 0;) {
            char* end = nullptr;
            const auto value = std::strtoull(p, &end, 16);
            if (end == p) {
                ++p;
                continue;
            }
            skip.push_back(value);
            p = end;
        }
    }
};

}

bool LabCode(const char* kind, unsigned stage, std::uint64_t address, std::span<const std::uint32_t> code) {
    static const LabCodeSwitches switches;
    if (!switches.trace && switches.skip.empty()) return false;
    const auto hash = LabCodeHash(code);
    const bool skipped = std::find(switches.skip.begin(), switches.skip.end(), hash) != switches.skip.end();
    if (switches.trace || skipped) {
        static std::mutex mutex;
        static std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
        std::lock_guard lock(mutex);
        if (seen.insert({address, hash}).second) {
            std::fprintf(stderr, "[labcode] %s stage=%u 0x%llx hash=%016llx words=%zu%s\n", kind, stage, static_cast<unsigned long long>(address), static_cast<unsigned long long>(hash), code.size(), skipped ? " skipped" : "");
        }
    }
    return skipped;
}

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(std::string("AGC driver: ") + reason);
    }
}

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset) {
    const auto it = registers.find(offset);
    require(it != registers.end(), "required shader register has not been written");
    return it->second;
}

std::uint32_t readUserData(const Registers& shader, std::uint32_t offset) {
    const auto it = shader.find(offset);
    return it == shader.end() ? 0u : it->second;
}

}
