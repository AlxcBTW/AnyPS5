#include "prx/libSceAgcDriver/Graphics/include/IndirectPublish.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4Opcodes.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace AgcDriver::Graphics::IndirectPublish {

namespace {

struct Context {
    const char* reason;
    std::uint64_t address;
    std::size_t bytes;
};

thread_local Context current{nullptr, 0, 0};

struct Key {
    std::string reason;
    std::uint32_t opcode;
    std::uint32_t queue;
    bool operator<(const Key& other) const { return std::tie(reason, opcode, queue) < std::tie(other.reason, other.opcode, other.queue); }
};

struct Totals {
    std::uint64_t flushes = 0, requestedBytes = 0, largestRequest = 0, imagesStored = 0, publishes = 0, units = 0, publishedBytes = 0, largestPublish = 0;
};

struct Stats {
    std::mutex mutex;
    std::map<Key, Totals> byKey;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

Stats& State() {
    static Stats* stats = new Stats();
    return *stats;
}

Key KeyOf(const char* reason) {
    const auto packet = GuestMemory::CurrentPacket();
    return Key{reason != nullptr ? reason : "none", packet.opcode, packet.queue};
}

std::string PacketText(std::uint32_t opcode) {
    if (opcode == GuestMemory::NoPacket) return "no-packet";
    if (opcode == 0xffffu) return "flip";
    for (const auto& entry : Pm4::Opcodes) {
        if (entry.value == opcode) return std::string(entry.name);
    }
    char text[24];
    std::snprintf(text, sizeof(text), "op 0x%x", opcode);
    return text;
}

}

bool Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_LAB_INDIRECT_PUBLISH");
        return text != nullptr && text[0] != 0 && text[0] != 0x30;
    }();
    return enabled;
}

FlushScope::FlushScope(const char* reason, std::uint64_t address, std::size_t bytes) : previousReason(current.reason), previousAddress(current.address), previousBytes(current.bytes), active(Enabled() && current.reason == nullptr) {
    if (!active) return;
    current = {reason, address, bytes};
    auto& stats = State();
    std::lock_guard lock(stats.mutex);
    auto& totals = stats.byKey[KeyOf(reason)];
    ++totals.flushes;
    totals.requestedBytes += bytes;
    totals.largestRequest = std::max<std::uint64_t>(totals.largestRequest, bytes);
}

FlushScope::~FlushScope() {
    if (!active) return;
    current = {previousReason, previousAddress, previousBytes};
}

void NoteImagesStored(std::size_t images) {
    if (!Enabled() || images == 0) return;
    auto& stats = State();
    std::lock_guard lock(stats.mutex);
    stats.byKey[KeyOf(current.reason)].imagesStored += images;
}

void NotePublish(std::uint64_t bytes, std::size_t units) {
    if (!Enabled()) return;
    auto& stats = State();
    std::lock_guard lock(stats.mutex);
    auto& totals = stats.byKey[KeyOf(current.reason)];
    ++totals.publishes;
    totals.units += units;
    totals.publishedBytes += bytes;
    totals.largestPublish = std::max(totals.largestPublish, bytes);
}

void ReportIfDue() {
    if (!Enabled()) return;
    auto& stats = State();
    std::string report;
    {
        std::lock_guard lock(stats.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now - stats.lastReport < std::chrono::seconds(10)) return;
        stats.lastReport = now;
        const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
        const auto mib = [](std::uint64_t value) { return static_cast<double>(value) / 1048576.0; };
        Totals all, indirect;
        for (const auto& [key, totals] : stats.byKey) {
            const bool arguments = key.reason == "indirect dispatch arguments" || key.reason == "indirect draw arguments";
            for (auto* into : {&all, arguments ? &indirect : nullptr}) {
                if (into == nullptr) continue;
                into->flushes += totals.flushes;
                into->requestedBytes += totals.requestedBytes;
                into->imagesStored += totals.imagesStored;
                into->publishes += totals.publishes;
                into->units += totals.units;
                into->publishedBytes += totals.publishedBytes;
            }
        }
        char text[768];
        std::snprintf(text, sizeof(text), "[indirectpublish] 10 s: argument flushes %llu (requested %.3f MiB, images stored %llu, publishes %llu, %llu units, %.3f MiB); all flushes %llu (requested %.1f MiB, images stored %llu, publishes %llu, %llu units, %.1f MiB); by flush reason and packet (flushes/requested MiB/largest request MiB/images stored/publishes/units/published MiB/largest publish MiB):",
            count(indirect.flushes), mib(indirect.requestedBytes), count(indirect.imagesStored), count(indirect.publishes), count(indirect.units), mib(indirect.publishedBytes),
            count(all.flushes), mib(all.requestedBytes), count(all.imagesStored), count(all.publishes), count(all.units), mib(all.publishedBytes));
        report = text;
        std::vector<std::pair<const Key*, const Totals*>> hot;
        for (const auto& [key, totals] : stats.byKey) hot.emplace_back(&key, &totals);
        std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second->publishedBytes > b.second->publishedBytes; });
        if (hot.empty()) report += " none";
        for (std::size_t i = 0; i < hot.size() && i < 24; ++i) {
            const auto& key = *hot[i].first;
            const auto& t = *hot[i].second;
            std::snprintf(text, sizeof(text), " [%s q0x%x %s: %llu/%.2f/%.2f/%llu/%llu/%llu/%.2f/%.2f]", key.reason.c_str(), key.queue, PacketText(key.opcode).c_str(), count(t.flushes), mib(t.requestedBytes), mib(t.largestRequest), count(t.imagesStored), count(t.publishes), count(t.units), mib(t.publishedBytes), mib(t.largestPublish));
            report += text;
        }
        stats.byKey.clear();
    }
    report += "\n";
    std::fwrite(report.data(), 1, report.size(), stderr);
}

}
