#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/SpeculativeCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace AgcDriver::DriverDetail {

SpeculativeMode SpeculativeCaptureMode() {
    static const SpeculativeMode mode = [] {
        const char* text = std::getenv("APS5_SPECULATIVE_CAPTURE");
        if (text == nullptr || *text == '\0' || std::strcmp(text, "0") == 0 || std::strcmp(text, "off") == 0) return SpeculativeMode::Off;
        return std::strcmp(text, "verify") == 0 ? SpeculativeMode::Verify : SpeculativeMode::On;
    }();
    return mode;
}

bool SpeculativeCaptureAbort() {
    static const bool abort = std::getenv("APS5_SPECULATIVE_CAPTURE_ABORT") != nullptr;
    return abort;
}

namespace Speculation {

namespace {

constexpr std::size_t RememberedWords = 4096;

std::mutex refusedMutex;
// Captures left to skip per refused program.
std::unordered_map<std::uint64_t, std::uint32_t>& refusedPrograms() {
    static std::unordered_map<std::uint64_t, std::uint32_t> programs;
    return programs;
}

std::mutex countsMutex;
SpeculationCounts totals;
// Per word read, so lock free.
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(VolatileReason::Count)> volatileReads{};

std::mutex rememberedMutex;
std::deque<Remembered>& remembered() {
    static std::deque<Remembered> words;
    return words;
}

}

bool Refused(std::uint64_t program) {
    std::lock_guard lock(refusedMutex);
    auto& programs = refusedPrograms();
    const auto found = programs.find(program);
    if (found == programs.end()) return false;
    if (--found->second == 0) programs.erase(found);
    return true;
}

void Refuse(std::uint64_t program, RefusalReason reason) {
    {
        std::lock_guard lock(refusedMutex);
        const bool structural = reason == RefusalReason::NoPureSlots || reason == RefusalReason::PlanUnsafe || reason == RefusalReason::FillsDontFit;
        refusedPrograms()[program] = structural ? 65536u : 1024u;
    }
    std::lock_guard lock(countsMutex);
    ++totals.refusals[static_cast<std::size_t>(reason)];
}

void CountVolatile(VolatileReason reason) {
    volatileReads[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void Count(std::uint64_t SpeculationCounts::*field, std::uint64_t amount) {
    std::lock_guard lock(countsMutex);
    totals.*field += amount;
}

SpeculationCounts Take() {
    std::lock_guard lock(countsMutex);
    auto taken = totals;
    totals = SpeculationCounts{};
    taken.speculatedWords = ShaderMemory::TakeSpeculatedWords();
    const auto hooked = ShaderMemory::TakeReadCounts();
    for (std::size_t kind = 0; kind < taken.hookReads.size(); ++kind) {
        taken.hookReads[kind] = hooked.reads[kind];
        taken.hookWaited[kind] = hooked.waited[kind];
    }
    for (std::size_t reason = 0; reason < volatileReads.size(); ++reason) taken.volatileReads[reason] = volatileReads[reason].exchange(0, std::memory_order_relaxed);
    return taken;
}

void Remember(std::span<const std::uint64_t> addresses, std::span<const std::uint32_t> values) {
    std::lock_guard lock(rememberedMutex);
    auto& words = remembered();
    for (std::size_t index = 0; index < addresses.size() && index < values.size(); ++index) {
        if (words.size() >= RememberedWords) words.pop_front();
        words.push_back({addresses[index], values[index]});
    }
}

void Resolve(const std::function<bool(std::uint64_t)>& pending, const std::function<void(std::uint64_t, std::uint32_t)>& finished) {
    std::vector<Remembered> due;
    {
        std::lock_guard lock(rememberedMutex);
        auto& words = remembered();
        if (words.empty()) return;
        std::deque<Remembered> keep;
        for (const auto& word : words) {
            if (pending(word.address)) keep.push_back(word);
            else due.push_back(word);
        }
        words = std::move(keep);
    }
    for (const auto& word : due) finished(word.address, word.value);
}

}

// The evidence a wait would have collected: a remembered word whose writer has finished is read
// again (no wait is left to make) and told to the observer as unchanged or changed, which is what
// lets stable words graduate to raw reads. Words whose writer is still pending stay remembered.
void Driver::resolveSpeculations() {
    static std::atomic<std::uint32_t> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) % Speculation::ResolveEvery != 0) return;
    PendingView pending;
    pending.Load();
    Speculation::Resolve(
        [&](std::uint64_t address) { return pending.Overlaps(address, sizeof(std::uint32_t)); },
        [](std::uint64_t address, std::uint32_t speculated) {
            if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(std::uint32_t))) return;
            std::uint32_t now = 0;
            {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::Capture);
                GuestMemory::Read(address, std::as_writable_bytes(std::span(&now, 1)), alignof(std::uint32_t));
            }
            Speculation::Count(now == speculated ? &SpeculationCounts::observedUnchanged : &SpeculationCounts::observedChanged);
            observePendingWrite(address, now == speculated);
        });
}

}
