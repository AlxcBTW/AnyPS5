#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "HotLever.hpp"
#include <chrono>
#include <cstdlib>

// exp (APS5_EXP_LAND_CHECK, never push): a compute queue lands a cross-queue wait (#1864) before the guest
// reads behind it, because titles fill the tables a gated dispatch reads after they submit; the gated group is
// resolved after the landing. LAND_CHECK resolves that group BEFORE a landing that has to wait for the GPU,
// lands, and then compares every captured region with memory (through the flush hook, as a capture reads):
//   1  census only: counts runs whose captures were unchanged or changed, then drops them all; the group is
//      resolved again after the landing exactly as before.
//   2  as 1, but the unchanged results stay parked and the real dispatches adopt them (the bytes equal what a
//      capture after the landing reads, so the result is the same); changed ones, and dispatches whose
//      indirect arguments were read on the CPU (not in the captured regions), are dropped and resolved live.
// The number that matters is "changed": how often a capture before the landing would have been wrong.
namespace AgcDriver::DriverDetail {

namespace {

std::int64_t envInteger(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? static_cast<std::int64_t>(std::strtoll(value, nullptr, 10)) : 0;
}

struct LandCheckStats {
    std::uint64_t landings = 0, resolved = 0, unchanged = 0, changed = 0, runsChanged = 0, cpuArguments = 0, kept = 0, regions = 0, bytes = 0, examples = 0;
    double speculateMs = 0, compareMs = 0;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};

LandCheckStats& landCheckStats() {
    static thread_local LandCheckStats stats;
    return stats;
}

bool profiling() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

}

std::int64_t Driver::landCheckMode() {
    static const Aps5Hot::Lever lever("APS5_EXP_LAND_CHECK", envInteger("APS5_EXP_LAND_CHECK"));
    return lever.Value();
}

bool Driver::landingWouldWait() {
    for (const auto& wait : pendingWaits()) {
        if (!Pm4::WaitSatisfiedUnchecked(std::span<const std::uint32_t>(wait.words).first(wait.count))) return true;
    }
    return false;
}

void Driver::speculateGroupBeforeLanding(const Submission& submission, const QueueState& live, std::size_t from) {
    auto& stats = landCheckStats();
    const auto begin = std::chrono::steady_clock::now();
    ++stats.landings;
    resolveGroupAhead(submission, live, from);
    stats.resolved += resolvedAhead().size();
    stats.speculateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void Driver::checkGroupAfterLanding(const Submission& submission, std::int64_t mode) {
    auto& stats = landCheckStats();
    const auto begin = std::chrono::steady_clock::now();
    auto& resolved = resolvedAhead();
    bool runChanged = false;
    for (auto entry = resolved.begin(); entry != resolved.end();) {
        const auto offset = entry->first;
        const auto& result = entry->second;
        const bool indirect = offset < submission.commands.size() && ((submission.commands[offset] >> 8u) & 0xffu) == 0x16u;
        const bool cpuArguments = indirect && result.indirectArguments == 0;
        const ShaderRecompiler::MemoryRegion* differing = nullptr;
        for (const auto& region : result.captured) {
            ++stats.regions;
            stats.bytes += region.bytes.size();
            if (!GuestMemory::EqualsCommitted(region.guestAddress, region.bytes)) {
                differing = &region;
                break;
            }
        }
        if (cpuArguments) ++stats.cpuArguments;
        if (differing != nullptr) {
            ++stats.changed;
            runChanged = true;
            if (profiling() && stats.examples < 8) {
                ++stats.examples;
                AgcDriver::ProfilePrint_nid_no_patch("[landcheck] queue 0x%x: dispatch at +0x%zx (shader 0x%llx) captured 0x%llx+0x%zx before the landing and memory differs after it\n", submission.queue, offset * sizeof(std::uint32_t), static_cast<unsigned long long>(result.address), static_cast<unsigned long long>(differing->guestAddress), differing->bytes.size());
            }
        } else {
            ++stats.unchanged;
        }
        const bool keep = mode >= 2 && differing == nullptr && !cpuArguments;
        if (keep) {
            ++stats.kept;
            ++entry;
        } else {
            entry = resolved.erase(entry);
        }
    }
    if (runChanged) ++stats.runsChanged;
    const auto now = std::chrono::steady_clock::now();
    stats.compareMs += std::chrono::duration<double, std::milli>(now - begin).count();
    if (!profiling() || now - stats.reported < std::chrono::seconds(10)) return;
    AgcDriver::ProfilePrint_nid_no_patch("[landcheck] queue 0x%x (10 s): LAND_CHECK %lld: %llu landings that waited were resolved first (%llu dispatches, %.1f ms); after the landing %llu unchanged, %llu changed (in %llu runs), %llu with indirect arguments read on the CPU; %llu kept for adoption; %llu regions / %llu KiB compared in %.1f ms\n", submission.queue, static_cast<long long>(mode), static_cast<unsigned long long>(stats.landings), static_cast<unsigned long long>(stats.resolved), stats.speculateMs, static_cast<unsigned long long>(stats.unchanged), static_cast<unsigned long long>(stats.changed), static_cast<unsigned long long>(stats.runsChanged), static_cast<unsigned long long>(stats.cpuArguments), static_cast<unsigned long long>(stats.kept), static_cast<unsigned long long>(stats.regions), static_cast<unsigned long long>(stats.bytes >> 10u), stats.compareMs);
    stats = LandCheckStats{};
    stats.reported = now;
}

}
