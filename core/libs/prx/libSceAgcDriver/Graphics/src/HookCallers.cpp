#include "prx/libSceAgcDriver/Graphics/include/HookCallers.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4Opcodes.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace AgcDriver::Graphics::HookCallers {

namespace {

constexpr std::size_t CompareChunk = 4096;
constexpr std::size_t TopCallers = 20;

struct Key {
    std::uint8_t kind;
    std::uint8_t site;
    std::uint32_t queue;
    std::uint32_t opcode;
    std::uint64_t caller;
    bool operator<(const Key& other) const { return std::tie(kind, site, queue, opcode, caller) < std::tie(other.kind, other.site, other.queue, other.opcode, other.caller); }
};

struct Totals {
    std::uint64_t publishes = 0, units = 0, bytes = 0, accessBytes = 0, outsideBytes = 0, noAccess = 0;
    std::uint64_t measured = 0, measuredBytes = 0, changedBytes = 0, changedPublishes = 0, identicalPublishes = 0, unmeasured = 0;
};

struct Stats {
    std::mutex mutex;
    std::map<Key, Totals> byKey;
    Totals total;
    std::uint64_t outstanding = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

Stats& State() {
    static Stats* stats = new Stats();
    return *stats;
}

std::uint64_t SnapshotLimit() {
    static const std::uint64_t limit = [] {
        const char* text = std::getenv("APS5_LAB_HOOK_CALLERS_SNAP_MIB");
        return (text != nullptr ? std::strtoull(text, nullptr, 10) : 16ull) << 20u;
    }();
    return limit;
}

struct Snapshot {
    Key key;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::vector<std::byte> before;
};

void Add(Totals& into, const Totals& from) {
    into.publishes += from.publishes;
    into.units += from.units;
    into.bytes += from.bytes;
    into.accessBytes += from.accessBytes;
    into.outsideBytes += from.outsideBytes;
    into.noAccess += from.noAccess;
    into.measured += from.measured;
    into.measuredBytes += from.measuredBytes;
    into.changedBytes += from.changedBytes;
    into.changedPublishes += from.changedPublishes;
    into.identicalPublishes += from.identicalPublishes;
    into.unmeasured += from.unmeasured;
}

void Finish(const std::shared_ptr<Snapshot>& snapshot) {
    std::uint64_t changed = 0;
    std::size_t at = 0;
    for (const auto& [begin, end] : snapshot->ranges) {
        const auto length = static_cast<std::size_t>(end - begin);
        if (GuestMemory::Accessible(reinterpret_cast<const void*>(begin), length)) {
            for (std::size_t offset = 0; offset < length; offset += CompareChunk) {
                const auto part = std::min(CompareChunk, length - offset);
                if (std::memcmp(snapshot->before.data() + at + offset, reinterpret_cast<const void*>(begin + offset), part) != 0) changed += part;
            }
        }
        at += length;
    }
    Totals totals;
    totals.measured = 1;
    totals.measuredBytes = snapshot->before.size();
    totals.changedBytes = changed;
    if (changed != 0) totals.changedPublishes = 1;
    else totals.identicalPublishes = 1;
    auto& stats = State();
    std::lock_guard lock(stats.mutex);
    Add(stats.byKey[snapshot->key], totals);
    Add(stats.total, totals);
    --stats.outstanding;
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

void NotePublish(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t bytes, std::size_t units) {
    if (!GuestMemory::HookCallersEnabled() || ranges.empty()) return;
    const auto access = GuestMemory::CurrentHookAccess();
    const auto packet = GuestMemory::CurrentPacket();
    Key key{static_cast<std::uint8_t>(access.kind), static_cast<std::uint8_t>(GuestMemory::CurrentReadSite()), packet.queue, packet.opcode, 0};
    if (access.caller != nullptr) {
        key.caller = GuestMemory::CodeOffset(access.caller);
    } else {
        unsigned long long frames[6] = {};
        const auto captured = GuestMemory::CaptureCallerOffsets(frames, 0);
        for (std::size_t i = 0; i < captured && i < 6; ++i) {
            if (frames[i] != 0) {
                key.caller = frames[i];
                break;
            }
        }
    }
    Totals totals;
    totals.publishes = 1;
    totals.units = units;
    totals.bytes = bytes;
    if (access.kind == GuestMemory::HookKind::None) {
        totals.noAccess = 1;
    } else {
        const auto accessEnd = access.address + access.bytes;
        totals.accessBytes = access.bytes;
        for (const auto& [begin, end] : ranges) {
            const auto low = std::max(begin, access.address);
            const auto high = std::min(end, accessEnd);
            totals.outsideBytes += (end - begin) - (high > low ? high - low : 0);
        }
    }
    std::shared_ptr<Snapshot> snapshot;
    auto* recorder = Recorder::Active();
    if (recorder != nullptr && bytes <= SnapshotLimit()) {
        snapshot = std::make_shared<Snapshot>();
        snapshot->key = key;
        snapshot->before.reserve(static_cast<std::size_t>(bytes));
        for (const auto& [begin, end] : ranges) {
            const auto length = static_cast<std::size_t>(end - begin);
            if (!GuestMemory::Accessible(reinterpret_cast<const void*>(begin), length)) {
                snapshot.reset();
                break;
            }
            const auto* from = reinterpret_cast<const std::byte*>(begin);
            snapshot->before.insert(snapshot->before.end(), from, from + length);
            snapshot->ranges.emplace_back(begin, end);
        }
    }
    if (snapshot == nullptr) totals.unmeasured = 1;
    {
        auto& stats = State();
        std::lock_guard lock(stats.mutex);
        Add(stats.byKey[key], totals);
        Add(stats.total, totals);
        if (snapshot != nullptr) ++stats.outstanding;
    }
    if (snapshot != nullptr) recorder->OnRetire([snapshot] { Finish(snapshot); });
}

void ReportIfDue() {
    if (!GuestMemory::HookCallersEnabled()) return;
    auto& stats = State();
    std::string report;
    {
        std::lock_guard lock(stats.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now - stats.lastReport < std::chrono::seconds(10)) return;
        stats.lastReport = now;
        const auto calls = GuestMemory::TakeHookCallCounts();
        const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
        const auto mib = [](std::uint64_t value) { return static_cast<double>(value) / 1048576.0; };
        const auto call = [&](GuestMemory::HookKind kind) { return count(calls[static_cast<std::size_t>(kind)]); };
        const auto& t = stats.total;
        char text[1024];
        std::snprintf(text, sizeof(text), "[hookcallers] 10 s: hook calls read %llu, read-committed %llu, equals %llu, write %llu, write-changed %llu, direct %llu; hook-reason publishes %llu (%llu units, %.1f MiB; the accesses touched %.1f MiB, %.1f MiB of the published bytes lie outside them, %llu publishes without a hook access); measured %llu publishes (%.1f MiB): bytes changed by the publish %.1f MiB in %llu publishes, identical in %llu; unmeasured %llu, measurement outstanding %llu; top by MiB published (kind site queue packet +caller: publishes/MiB published/MiB accessed/MiB outside, measured n changed MiB of MiB):",
            call(GuestMemory::HookKind::Read), call(GuestMemory::HookKind::ReadCommitted), call(GuestMemory::HookKind::Equals), call(GuestMemory::HookKind::Write), call(GuestMemory::HookKind::WriteChanged), call(GuestMemory::HookKind::Direct),
            count(t.publishes), count(t.units), mib(t.bytes), mib(t.accessBytes), mib(t.outsideBytes), count(t.noAccess), count(t.measured), mib(t.measuredBytes), mib(t.changedBytes), count(t.changedPublishes), count(t.identicalPublishes), count(t.unmeasured), count(stats.outstanding));
        report = text;
        std::vector<std::pair<const Key*, const Totals*>> hot;
        for (const auto& [key, totals] : stats.byKey) hot.emplace_back(&key, &totals);
        std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second->bytes > b.second->bytes; });
        if (hot.empty()) report += " none";
        for (std::size_t i = 0; i < hot.size() && i < TopCallers; ++i) {
            const auto& key = *hot[i].first;
            const auto& k = *hot[i].second;
            std::snprintf(text, sizeof(text), " [%s %s q0x%x %s +0x%llx: %llu/%.1f/%.1f/%.1f, m%llu c%.1f of %.1f]", GuestMemory::HookKindName(static_cast<GuestMemory::HookKind>(key.kind)), GuestMemory::ReadSiteName(static_cast<GuestMemory::ReadSite>(key.site)), key.queue, PacketText(key.opcode).c_str(), static_cast<unsigned long long>(key.caller), count(k.publishes), mib(k.bytes), mib(k.accessBytes), mib(k.outsideBytes), count(k.measured), mib(k.changedBytes), mib(k.measuredBytes));
            report += text;
        }
        stats.byKey.clear();
        stats.total = Totals{};
    }
    report += "\n";
    std::fwrite(report.data(), 1, report.size(), stderr);
}

}
