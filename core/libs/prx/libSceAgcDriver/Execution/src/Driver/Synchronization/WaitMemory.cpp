#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/SubmissionRing.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#ifdef _WIN32
#include <windows.h>
#endif
#include <immintrin.h>
#include <algorithm>
#include <tuple>
#include <vector>

namespace AgcDriver::DriverDetail {

void PollSleep() {
#ifdef _WIN32
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -2000;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::microseconds(200));
}

namespace {

// APS5_LAB_WAIT_DEFER: bit 0 the deferral of step 1, bit 1 landings that wait on the producer's timeline serial.
unsigned WaitDeferBits() {
    static const unsigned bits = [] {
        const char* text = std::getenv("APS5_LAB_WAIT_DEFER");
        if (text == nullptr || *text == 0 || std::strcmp(text, "0") == 0) return 0u;
        if (std::strcmp(text, "2") == 0) return 3u;
        if (std::strcmp(text, "2s") == 0) return 2u;
        return 1u;
    }();
    return bits;
}

}

bool Driver::waitDeferEnabled() {
    return (WaitDeferBits() & 1u) != 0;
}

bool Driver::waitTimelineLanding() {
    return (WaitDeferBits() & 2u) != 0;
}

namespace {

// APS5_LAB_WAIT_DEFER counters (all threads), printed as one [waitdefer] line every 10 s.
struct DeferStats {
    // Waits that reached the deferral check, and why the ones that stayed in the poll did.
    std::atomic<std::uint64_t> past{0}, noEntry{0}, overwritten{0}, mismatch{0};
    // Deferred waits: where, on which queue, and the state of the label entry that satisfied them.
    std::atomic<std::uint64_t> deferredEntry{0}, deferredPoll{0}, deferredQueue0{0}, unclosed{0}, closedChanged{0}, closedKept{0}, behindCompletion{0}, submits{0};
    // Polls that ended in memory, in the table (a later trusted hit), in a deferral; the time they took.
    std::atomic<std::uint64_t> endMemory{0}, endTable{0}, endOther{0}, memoryUs{0}, tableUs{0}, deferUs{0};
    // Landings of deferred waits: already satisfied, had to wait, the wait, the gap since deferral.
    std::atomic<std::uint64_t> lands{0}, landedFree{0}, landedWaited{0}, waitedUs{0}, maxUs{0}, gapUs{0}, gaveUp{0};
    std::atomic<std::int64_t> reported{0};
} deferStats;

void ReportDeferStats() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto last = deferStats.reported.load(std::memory_order_relaxed);
    if (last == 0) {
        deferStats.reported.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 10'000'000'000ll || !deferStats.reported.compare_exchange_strong(last, now)) return;
    const auto take = [](std::atomic<std::uint64_t>& value) { return static_cast<unsigned long long>(value.exchange(0, std::memory_order_relaxed)); };
    const auto past = take(deferStats.past), none = take(deferStats.noEntry), over = take(deferStats.overwritten), mismatch = take(deferStats.mismatch);
    const auto entry = take(deferStats.deferredEntry), poll = take(deferStats.deferredPoll), q0 = take(deferStats.deferredQueue0), unclosed = take(deferStats.unclosed), changed = take(deferStats.closedChanged), kept = take(deferStats.closedKept), behind = take(deferStats.behindCompletion), submits = take(deferStats.submits);
    const auto endMemory = take(deferStats.endMemory), endTable = take(deferStats.endTable), endOther = take(deferStats.endOther), memoryUs = take(deferStats.memoryUs), tableUs = take(deferStats.tableUs), deferUs = take(deferStats.deferUs);
    const auto lands = take(deferStats.lands), satisfied = take(deferStats.landedFree), waited = take(deferStats.landedWaited), waitedUs = take(deferStats.waitedUs), maxUs = take(deferStats.maxUs), gapUs = take(deferStats.gapUs), gaveUp = take(deferStats.gaveUp);
    AgcDriver::ProfilePrint_nid_no_patch("[waitdefer] (10 s): %llu waits reached the check; deferred %llu (entry %llu, polling %llu, queue 0 %llu; entry unclosed %llu, closed+CPU store %llu, closed+kept %llu, behind completion %llu, open batch submitted %llu) in %.1f ms parked; stayed in the poll: no recorded entry %llu, overwritten %llu, value does not satisfy %llu; polls ended in memory %llu (%.1f ms), table %llu (%.1f ms), other %llu; landings %llu: already satisfied %llu, had to wait %llu (%.1f ms, max %.2f ms), %.1f ms between deferral and landing in total, gave up %llu\n",
                                         past, entry + poll, entry, poll, q0, unclosed, changed, kept, behind, submits, deferUs / 1000.0, none, over, mismatch, endMemory, memoryUs / 1000.0, endTable, tableUs / 1000.0, endOther, lands, satisfied, waited, waitedUs / 1000.0, maxUs / 1000.0, gapUs / 1000.0, gaveUp);
}

void DeferCount(std::atomic<std::uint64_t>& value, std::uint64_t amount = 1) { value.fetch_add(amount, std::memory_order_relaxed); }

// APS5_LAB_WAIT_DEFER=2 / 2s counters, printed as one [waitdefer2] line every 10 s.
constexpr std::size_t LandCauseCount = 8;
struct TimelineStats {
    // Landings by what forced them (deferred and plain ones), those that found the compare unsatisfied, and the time they waited.
    std::array<std::atomic<std::uint64_t>, LandCauseCount> lands{}, waited{}, waitedUs{};
    // Landings that began a timeline wait attempt, and how each attempt ended: the producer's batch was waited for on the GPU
    // (and the compare then held / did not hold), or nothing submitted notes a write over the label (still polling).
    std::atomic<std::uint64_t> attempts{0}, waits{0}, waitUs{0}, held{0}, notHeld{0}, noTarget{0}, openSubmits{0}, skippedQueue0Dormant{0};
    // Deferred waits (step 1) by what the landing found: the worker translated for the whole gap while the label was pending
    // (exact when the landing then had to wait), or the label may have landed earlier in the gap (an upper bound).
    std::atomic<std::uint64_t> savedExactUs{0}, savedUpperUs{0}, deferredLands{0};
    std::atomic<std::int64_t> reported{0};
} timelineStats;

constexpr const char* LandCauseNames[LandCauseCount] = {"effect", "predicated", "dispatch", "draw", "cond_exec", "read", "end", "other"};

void ReportTimelineStats() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto last = timelineStats.reported.load(std::memory_order_relaxed);
    if (last == 0) {
        timelineStats.reported.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 10'000'000'000ll || !timelineStats.reported.compare_exchange_strong(last, now)) return;
    const auto take = [](std::atomic<std::uint64_t>& value) { return static_cast<unsigned long long>(value.exchange(0, std::memory_order_relaxed)); };
    char causes[512];
    int length = 0;
    for (std::size_t i = 0; i < LandCauseCount && length < 440; ++i) {
        const auto lands = take(timelineStats.lands[i]), waited = take(timelineStats.waited[i]), waitedUs = take(timelineStats.waitedUs[i]);
        if (lands != 0) length += std::snprintf(causes + length, sizeof(causes) - static_cast<std::size_t>(length), " %s %llu/%llu/%.1f ms", LandCauseNames[i], lands, waited, waitedUs / 1000.0);
    }
    causes[length] = 0;
    const auto attempts = take(timelineStats.attempts), waits = take(timelineStats.waits), waitUs = take(timelineStats.waitUs), held = take(timelineStats.held), notHeld = take(timelineStats.notHeld), noTarget = take(timelineStats.noTarget), openSubmits = take(timelineStats.openSubmits), dormant = take(timelineStats.skippedQueue0Dormant);
    const auto exactUs = take(timelineStats.savedExactUs), upperUs = take(timelineStats.savedUpperUs), deferredLands = take(timelineStats.deferredLands);
    AgcDriver::ProfilePrint_nid_no_patch("[waitdefer2] (10 s): landings by cause (landings/found unsatisfied/ms waited):%s; timeline: %llu landings tried, %llu waited on the GPU for %.1f ms (compare held after %llu, not after %llu), no recorded label entry in an unfinished batch %llu, open batch submitted %llu, skipped with queue 0 dormant %llu; deferred waits landed %llu: translation overlapped %.1f ms exactly (landing had to wait), up to %.1f ms more (landing found it satisfied)\n",
                                         causes, attempts, waits, waitUs / 1000.0, held, notHeld, noTarget, openSubmits, dormant, deferredLands, exactUs / 1000.0, upperUs / 1000.0);
}

}

int Driver::waitTimeoutMs() {
    static const int value = [] { const char* text = std::getenv("APS5_GPU_WAIT_TIMEOUT_MS"); return text ? std::atoi(text) : 1000; }();
    return value;
}

bool Driver::pollReapAll() {
    static const bool all = std::getenv("APS5_POLL_REAP") != nullptr;
    return all;
}

bool Driver::pollTryEach() {
    static const bool each = std::getenv("APS5_POLL_TRY_EACH") != nullptr;
    return each;
}

bool Driver::traceLateLabels() {
    static const bool trace = std::getenv("APS5_TRACE_LATE_LABELS") != nullptr;
    return trace;
}

std::vector<Driver::PendingWait>& Driver::pendingWaits() {
    struct WaitsTag {};
    return HostThreadLocal<std::vector<PendingWait>, WaitsTag>();
}

void Driver::notePendingWait(std::span<const std::uint32_t> packet, std::uint32_t queue, std::uint64_t received, bool deferred) {
    if (packet.size() > std::tuple_size_v<decltype(PendingWait::words)>) return;
    static const bool allQueues = std::getenv("APS5_LAND_WAITS_ALL_QUEUES") != nullptr;
    if (!allQueues && queue == 0 && !deferred) return;
    if (Pm4::WaitSatisfiedUnchecked(packet)) return;
    auto& waits = pendingWaits();
    for (auto& wait : waits) {
        if (wait.count == packet.size() && std::equal(packet.begin(), packet.end(), wait.words.begin())) {
            if (deferred && !wait.deferred) {
                wait.deferred = true;
                wait.noted = std::chrono::steady_clock::now();
            }
            return;
        }
    }
    PendingWait wait{};
    std::copy(packet.begin(), packet.end(), wait.words.begin());
    wait.count = packet.size();
    wait.received = received;
    if (deferred) {
        wait.deferred = true;
        wait.noted = std::chrono::steady_clock::now();
    }
    waits.push_back(wait);
}

void Driver::landPendingWaits(std::uint32_t queue, bool onlyDeferred, LandCause cause) {
    static_assert(static_cast<std::size_t>(LandCause::Count) == LandCauseCount);
    auto& waits = pendingWaits();
    if (waits.empty()) return;
    if (onlyDeferred && std::none_of(waits.begin(), waits.end(), [](const PendingWait& wait) { return wait.deferred; })) return;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    struct LandStats {
        std::uint64_t lands = 0, waited = 0;
        double waitedMs = 0, maxMs = 0;
        std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
    };
    static thread_local LandStats stats;
    const bool timelineCounts = waitTimelineLanding();
    const PacketHistory none{};
    std::vector<PendingWait> kept;
    for (const auto& wait : waits) {
        if (onlyDeferred && !wait.deferred) {
            kept.push_back(wait);
            continue;
        }
        const auto packet = std::span<const std::uint32_t>(wait.words).first(wait.count);
        ++stats.lands;
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t gapUs = 0;
        if (wait.deferred) {
            gapUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(started - wait.noted).count());
            DeferCount(deferStats.lands);
            DeferCount(deferStats.gapUs, gapUs);
        }
        if (timelineCounts) DeferCount(timelineStats.lands[static_cast<std::size_t>(cause)]);
        if (Pm4::WaitSatisfiedUnchecked(packet)) {
            if (wait.deferred) {
                DeferCount(deferStats.landedFree);
                if (timelineCounts) {
                    DeferCount(timelineStats.deferredLands);
                    DeferCount(timelineStats.savedUpperUs, gapUs);
                }
            }
            continue;
        }
        waitMemory(packet, queue, none, wait.received, false, true, wait.deferred);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        ++stats.waited;
        stats.waitedMs += ms;
        stats.maxMs = std::max(stats.maxMs, ms);
        if (timelineCounts) {
            DeferCount(timelineStats.waited[static_cast<std::size_t>(cause)]);
            DeferCount(timelineStats.waitedUs[static_cast<std::size_t>(cause)], static_cast<std::uint64_t>(ms * 1000.0));
        }
        if (wait.deferred) {
            const auto us = static_cast<std::uint64_t>(ms * 1000.0);
            DeferCount(deferStats.landedWaited);
            DeferCount(deferStats.waitedUs, us);
            auto seen = deferStats.maxUs.load(std::memory_order_relaxed);
            while (seen < us && !deferStats.maxUs.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
            }
            if (timelineCounts) {
                DeferCount(timelineStats.deferredLands);
                DeferCount(timelineStats.savedExactUs, gapUs);
            }
        }
    }
    if (onlyDeferred) waits = std::move(kept);
    else waits.clear();
    if (waitDeferEnabled()) ReportDeferStats();
    if (timelineCounts) ReportTimelineStats();
    if (profile && std::chrono::steady_clock::now() - stats.reported > std::chrono::seconds(10)) {
        AgcDriver::ProfilePrint_nid_no_patch("[landwait] queue 0x%x (10 s): %llu waits landed before a guest read, %llu had to wait for the GPU: %.1f ms in total, max %.2f ms\n", queue, static_cast<unsigned long long>(stats.lands), static_cast<unsigned long long>(stats.waited), stats.waitedMs, stats.maxMs);
        stats = LandStats{};
    }
}

void Driver::waitMemory(std::span<const std::uint32_t> packet, std::uint32_t queue, const PacketHistory& context, std::uint64_t received, bool heldAtSubmit, bool requireMemory, bool strictLanding) {

    auto start = std::chrono::steady_clock::now();
    auto lastDone = packetsDone.load();

    GuestMemory::TagGpuLockThread(queue);
    auto& outcomes = waitOutcomes();
    const std::uint64_t awaited = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
    const std::size_t awaitedBytes = Pm4::WaitAwaitedBytes(packet);
    static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
    if (traceGpu) std::fprintf(stderr, "[gpu] %.1f queue 0x%x waits 0x%llx == 0x%x (now 0x%x)\n", TraceMs(), queue, static_cast<unsigned long long>(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u)), packet[4],
                               *reinterpret_cast<const volatile std::uint32_t*>(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u)));
    struct WaitTrace {
        bool enabled; std::uint32_t queue; std::uint64_t address; std::chrono::steady_clock::time_point begin;
        ~WaitTrace() {
            if (!enabled) return;
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (ms >= 0.5) std::fprintf(stderr, "[gpu] %.1f queue 0x%x wait on 0x%llx done after %.1f ms\n", TraceMs(), queue, static_cast<unsigned long long>(address), ms);
        }
    } waitTrace{traceGpu, queue, packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u), std::chrono::steady_clock::now()};
    bool warned = false;

    struct EpochPoint {
        bool bump = true;
        ~EpochPoint() {
            if (bump) bumpEpoch(&EpochBumps::waits);
        }
    } epochPoint;
    GuestMemory::CheckRange(reinterpret_cast<const void*>(awaited), awaitedBytes, awaitedBytes);
    if (Pm4::WaitSatisfiedUnchecked(packet)) {
        ++outcomes.atEntry;
        return;
    }
    if (heldAtSubmit) return;
    if (std::uint32_t writer = queue; !requireMemory && storedSince(packet, awaited, awaitedBytes, received, &writer)) {
        if (writer != queue) notePendingWait(packet, queue, received);
        else epochPoint.bump = false;
        return;
    }

    static const bool labelShortcut = std::getenv("APS5_NO_LABEL_SHORTCUT") == nullptr;
    const bool labelTable = labelShortcut && !requireMemory;
    static const bool overlapSubmit = std::getenv("APS5_NO_WAIT_OVERLAP_SUBMIT") == nullptr;

    static const bool waitLock = std::getenv("APS5_WAIT_LOCK") != nullptr;

    std::uint64_t refusedGeneration = 0;

    bool behindCompletion = false;
    const auto traceLate = [&](const char* outcome, const std::optional<Graphics::Recorder::LabelHit>& hit, Graphics::Recorder::LabelRefusal refusal) {
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1) >= 200) return;
        std::fprintf(stderr, "[late] queue 0x%x wait on 0x%llx (function %u ref 0x%x mask 0x%x) received %llu: %s; entry value 0x%llx stamp %llu generation %llu, refusal %d\n", queue, static_cast<unsigned long long>(awaited), packet[1] & 7u, packet[4], packet[5], static_cast<unsigned long long>(received), outcome,
                     static_cast<unsigned long long>(hit ? hit->value : 0), static_cast<unsigned long long>(hit ? hit->stamp : 0), static_cast<unsigned long long>(hit ? hit->generation : 0), static_cast<int>(refusal));
    };
    const auto takeLabel = [&](const std::optional<Graphics::Recorder::LabelHit>& hit, Graphics::Recorder::LabelRefusal refusal, bool polling, bool unlockedHit) {
        behindCompletion = refusal == Graphics::Recorder::LabelRefusal::BehindCompletion;
        const bool trace = traceLateLabels() && (refusal != Graphics::Recorder::LabelRefusal::None || (hit.has_value() && hit->late));
        if (!hit.has_value() || !Pm4::WaitComparesValue(packet, hit->value)) {
            if (trace) traceLate(hit.has_value() ? "value mismatch" : "refused by the table", hit, refusal);
            return false;
        }
        if (hit->late) {
            if (hit->generation == refusedGeneration) return false;
            if (!GuestMemory::UnchangedSinceCollected(awaited, awaitedBytes, hit->generation)) {
                ++outcomes.lateRefusedCpuStore;
                refusedGeneration = hit->generation;
                if (trace) traceLate("refused, CPU store since the group closed", hit, refusal);
                return false;
            }
            ++outcomes.fromRecorderLate;
            if (trace) traceLate("trusted", hit, refusal);
        }
        ++(polling ? outcomes.fromRecorderPolling : outcomes.fromRecorder);
        if (unlockedHit) ++outcomes.fromRecorderUnlocked;
        if (hit->queue == queue) {
            ++outcomes.fromRecorderSameQueue;
            if (!hit->late) epochPoint.bump = false;
        } else {
            notePendingWait(packet, queue, received);
        }
        return true;
    };

    // APS5_LAB_WAIT_DEFER: a recorded, satisfying, not overwritten label entry that the late rules (or the
    // pending group close) keep from serving the wait lets translation go on. The compare is then
    // verified in guest memory by landPendingWaits before the next packet with an effect.
    const bool deferOn = waitDeferEnabled() && labelTable && packet.size() <= std::tuple_size_v<decltype(PendingWait::words)>;
    bool classified = false, deferredNow = false;
    const auto tryDefer = [&](VulkanDevice* lockedDevice, bool polling) {
        Graphics::Recorder::RawLabel raw;
        const bool found = Graphics::Recorder::LookupLabelRaw(awaited, awaitedBytes, received, raw);
        const bool satisfies = found && !raw.overwritten && Pm4::WaitComparesValue(packet, raw.value);
        if (!classified) {
            classified = true;
            DeferCount(deferStats.past);
            if (!satisfies) DeferCount(!found ? deferStats.noEntry : raw.overwritten ? deferStats.overwritten : deferStats.mismatch);
        }
        if (!satisfies) return false;
        DeferCount(polling ? deferStats.deferredPoll : deferStats.deferredEntry);
        if (queue == 0) DeferCount(deferStats.deferredQueue0);
        DeferCount(!raw.closed ? deferStats.unclosed : GuestMemory::UnchangedSinceCollected(awaited, awaitedBytes, raw.generation) ? deferStats.closedKept : deferStats.closedChanged);
        if (raw.behindCompletion) DeferCount(deferStats.behindCompletion);
        notePendingWait(packet, queue, received, true);
        const auto flush = [&](VulkanDevice& target) {
            if (!target.OpenWriteOverlaps(awaited, awaitedBytes)) return;
            target.SubmitRecorded(false);
            DeferCount(deferStats.submits);
        };
        if (lockedDevice != nullptr) {
            flush(*lockedDevice);
        } else {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Wait);
            std::unique_lock flushLock(GuestMemory::GpuMutex(), std::try_to_lock);
            if (flushLock.owns_lock()) {
                if (const auto flushDevice = device.Load()) flush(*flushDevice);
            }
        }
        deferredNow = true;
        return true;
    };

    std::uint64_t seenGeneration = Graphics::Recorder::WriteGeneration();

    const bool unlocked = !waitLock && overlapSubmit;
    if (unlocked) {
        if (labelTable) {
            Graphics::Recorder::LabelRefusal refusal{};
            if (takeLabel(Graphics::Recorder::LookupLabel(awaited, awaitedBytes, received, &refusal), refusal, false, true)) return;
        }
    }
    const bool lockedEntry = !unlocked || Graphics::Recorder::SnapshotWriteOverlaps(awaited, awaitedBytes);
    if (!lockedEntry) ++outcomes.entriesUnlocked;

    static const bool blockingEntry = std::getenv("APS5_WAIT_LOCK_ENTRY") != nullptr;
    bool recheck = !lockedEntry;
    bool entryTryFailed = false;
    if (lockedEntry) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Wait);
        std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
        if (blockingEntry || !unlocked) {
            gpuLock.lock();
        } else if (!gpuLock.try_lock()) {
            ++outcomes.entryTriesFailed;
            recheck = true;
            entryTryFailed = true;
        }
        if (const auto localDevice = gpuLock.owns_lock() ? device.Load() : std::shared_ptr<VulkanDevice>{}) {

            if (labelTable) {
                Graphics::Recorder::LabelRefusal refusal{};
                if (takeLabel(localDevice->PendingLabel(awaited, awaitedBytes, received, &refusal), refusal, false, false)) return;
            }

            if (!overlapSubmit || localDevice->OpenWriteOverlaps(awaited, awaitedBytes)) localDevice->SubmitRecorded(false);
        }
    }

    struct PollAccount {
        bool on;
        const bool& deferred;
        int outcome = 2;
        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        ~PollAccount() {
            if (!on) return;
            const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
            if (deferred) {
                DeferCount(deferStats.deferUs, us);
            } else if (outcome == 0) {
                DeferCount(deferStats.endMemory);
                DeferCount(deferStats.memoryUs, us);
            } else if (outcome == 1) {
                DeferCount(deferStats.endTable);
                DeferCount(deferStats.tableUs, us);
            } else {
                DeferCount(deferStats.endOther);
            }
            ReportDeferStats();
        }
    } account{deferOn, deferredNow};
    if (deferOn && tryDefer(nullptr, false)) return;

    static const bool pollProfile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& poll = pollStats();
    const auto pollRefused = [&](Graphics::Recorder::LabelRefusal refusal) {
        if (pollProfile && refusal != Graphics::Recorder::LabelRefusal::None && static_cast<std::size_t>(refusal) < poll.refusals.size()) ++poll.refusals[static_cast<std::size_t>(refusal)];
    };

    const bool reapAll = pollReapAll() || !labelShortcut || queue == 0 || strictLanding;
    std::chrono::steady_clock::time_point lastTry{};
    const auto pollService = [&](bool sleeping) {
        if (pollProfile) ++poll.ticks;
        const auto generation = Graphics::Recorder::WriteGeneration();
        const bool changed = generation != seenGeneration || recheck;
        const bool labelDue = labelFlushDue();
        const bool reap = sleeping && (behindCompletion || ((reapAll || queue0Dormant.load(std::memory_order_relaxed)) && completionsPending()));
        if (!changed && !labelDue && !reap) return false;

        if (!changed && !pollTryEach()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastTry < PollTryInterval) return false;
            lastTry = now;
        }
        if (changed && unlocked && labelTable) {

            Graphics::Recorder::LabelRefusal refusal{};
            if (takeLabel(Graphics::Recorder::LookupLabel(awaited, awaitedBytes, received, &refusal), refusal, true, true)) {
                if (pollProfile) ++poll.tableHits;
                return true;
            }
            pollRefused(refusal);
            if (deferOn && tryDefer(nullptr, true)) return true;
        }
        if (pollProfile) ++poll.tries;
        std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
        if (!gpuLock.owns_lock()) {
            ++triesFailed[TryPoll];
            if (pollProfile) ++poll.triesFailed;
            return false;
        }

        struct TryHold {
            bool enabled;
            PollStats& poll;
            std::chrono::steady_clock::time_point start;
            ~TryHold() {
                if (!enabled) return;
                const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                poll.tryHoldUs += us;
                poll.longestTryHoldUs = std::max(poll.longestTryHoldUs, us);
            }
        } tryHold{pollProfile, poll, pollProfile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
        const auto localDevice = device.Load();
        if (localDevice == nullptr) {
            seenGeneration = generation;
            recheck = false;
            return false;
        }
        bool submitted = false;
        if (changed) {
            seenGeneration = generation;
            recheck = false;
            if (labelTable) {
                Graphics::Recorder::LabelRefusal refusal{};
                if (takeLabel(localDevice->PendingLabel(awaited, awaitedBytes, received, &refusal), refusal, true, false)) {
                    if (pollProfile) ++poll.tableHits;
                    return true;
                }
                pollRefused(refusal);
                if (deferOn && tryDefer(localDevice.get(), true)) return true;
            }
            if (localDevice->OpenWriteOverlaps(awaited, awaitedBytes)) {
                localDevice->SubmitRecorded(false);
                submitted = true;
            }
        }
        if (labelDue && !submitted && Graphics::Recorder::PendingLabelSince().has_value()) {
            localDevice->SubmitRecorded(false);
            submitted = true;
        }
        if (submitted) {
            ++outcomes.pollSubmits;
            if (pollProfile) ++poll.submits;
        }
        if (reap) {
            const auto withWorkBefore = pollProfile ? Graphics::Recorder::ReapsWithWork() : 0;
            localDevice->ReapRecorded();
            ++outcomes.pollReaps;
            if (pollProfile) {
                ++poll.reaps;
                ++(behindCompletion ? poll.reapsBehindCompletion : poll.reapsOther);
                poll.reapsWithWork += Graphics::Recorder::ReapsWithWork() - withWorkBefore;
            }
        }
        return false;
    };

    if (entryTryFailed && pollService(false)) {
        if (!deferredNow) account.outcome = 1;
        return;
    }

    noteWaitBlocked(queue, awaited, true);
    struct BlockedWait {
        Driver& driver;
        std::uint32_t queue;
        ~BlockedWait() { driver.noteWaitBlocked(queue, 0, false); }
    } blocked{*this, queue};
    static const bool pauseSpin = std::getenv("APS5_NO_PAUSE_SPIN") == nullptr;
    const auto spinLimit = queue == 0 ? std::chrono::microseconds(1500) : std::chrono::microseconds(100);
    const auto spinStart = std::chrono::steady_clock::now();
    bool spinning = pauseSpin;
    std::uint32_t polls = 0;
    StallWatch stall(queue);
    std::chrono::steady_clock::time_point settledAt{};

    // APS5_LAB_WAIT_DEFER=2: a landing (the compare must hold in guest memory) of a compute worker waits for the producer's batch on
    // the GPU timeline, without the mutex, instead of polling memory. Only the way the thread sleeps changes: the loop below
    // re-reads memory after every wait and keeps polling when the compare still does not hold (a recycled slot, a label stored by a
    // completion, a producer that is not recorded yet). The wait covers the batch that holds the label's pending-table entry, so it
    // ends when that batch's store ran; a dword without an entry (no label store recorded for it) is not waited for at all. Queue 0 and a dormant queue 0 keep polling: their polls reap
    // completions (flips, interrupts) that a blocked worker would leave to others.
    const bool timelineLanding = requireMemory && queue != 0 && waitTimelineLanding();
    std::uint64_t timelineWaited = 0;
    bool timelineTried = false, timelineNoTarget = false, timelineDormant = false;
    const auto landOnTimeline = [&] {
        if (queue0Dormant.load(std::memory_order_relaxed)) {
            if (!timelineDormant) {
                timelineDormant = true;
                DeferCount(timelineStats.skippedQueue0Dormant);
            }
            return false;
        }
        if (GuestMemory::GpuMutex().HeldByThisThread()) return false;
        std::shared_ptr<VulkanDevice> target;
        std::uint64_t serial = 0;
        bool openSubmitted = false;
        {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Wait);
            std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
            if (!gpuLock.owns_lock()) return false;
            target = device.Load();
            if (target == nullptr || !target->CanWaitUnlocked()) return false;
            openSubmitted = target->OpenWriteOverlaps(awaited, awaitedBytes);
            serial = target->SubmitForLabel(awaited, awaitedBytes);
        }
        if (!timelineTried) {
            timelineTried = true;
            DeferCount(timelineStats.attempts);
        }
        if (openSubmitted) DeferCount(timelineStats.openSubmits);
        if (serial == 0 || serial <= timelineWaited) {
            if (timelineWaited == 0 && !timelineNoTarget) {
                timelineNoTarget = true;
                DeferCount(timelineStats.noTarget);
            }
            return false;
        }
        const auto begin = std::chrono::steady_clock::now();
        target->WaitRecorded(serial);
        timelineWaited = serial;
        DeferCount(timelineStats.waits);
        DeferCount(timelineStats.waitUs, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count()));
        DeferCount(Pm4::WaitSatisfiedUnchecked(packet) ? timelineStats.held : timelineStats.notHeld);
        return true;
    };
    while (!Pm4::WaitSatisfiedUnchecked(packet)) {
        if (std::uint32_t writer = queue; !requireMemory && storedSince(packet, awaited, awaitedBytes, received, &writer)) {
            if (writer != queue) notePendingWait(packet, queue, received);
            else epochPoint.bump = false;
            account.outcome = 3;
            return;
        }
        ++polls;
        if (spinning) {
            _mm_pause();
            if ((polls & 63u) != 0) continue;
            if (std::chrono::steady_clock::now() - spinStart > spinLimit) spinning = false;
        } else if (pauseSpin || polls >= 4000) {
            PollSleep();
        } else {
            std::this_thread::yield();
        }
        CheckFailure();
        if (timelineLanding && !spinning && landOnTimeline()) continue;
        if (pollService(!spinning)) {
            if (!deferredNow) account.outcome = 1;
            return;
        }
        noteAwaitingTitle(queue, awaited);
        if (requireMemory && (polls & 15u) == 0) {
            std::unique_lock settleLock(GuestMemory::GpuMutex(), std::try_to_lock);
            if (settleLock.owns_lock()) {
                if (const auto localDevice = device.Load(); localDevice != nullptr && localDevice->RecordedWritesSettled(awaited, awaitedBytes)) {
                    if (!strictLanding) return;
                    // A deferred wait was never proven by the table: settled writes without the value in memory
                    // keep it waiting (as the hardware would) until the usual timeout, then it gives up like a plain landing.
                    const auto now = std::chrono::steady_clock::now();
                    if (settledAt == std::chrono::steady_clock::time_point{}) settledAt = now;
                    if (now - settledAt > std::chrono::milliseconds(waitTimeoutMs())) {
                        DeferCount(deferStats.gaveUp);
                        std::fprintf(stderr, "[waitdefer] queue 0x%x landing of the wait on 0x%llx gave up: recorded writes settled without the compare holding\n", queue, static_cast<unsigned long long>(awaited));
                        return;
                    }
                }
            }
        }
        if (const auto done = packetsDone.load(); done != lastDone || (!requireMemory && packetsInFlight.load() != 0)) {
            lastDone = done;
            start = std::chrono::steady_clock::now();
            stall.Progressed();
        }
        stall.Check(((packet[0] >> 8u) & 0xffu) == 0x93u ? "WAIT_REG_MEM_64 at" : "WAIT_REG_MEM at", awaited);

        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(waitTimeoutMs())) {
            warned = true;
            ++outcomes.timedOut;
            {
                static std::mutex reportedMutex;
                static std::set<std::uint64_t> reported;
                static std::uint64_t timeouts = 0;
                const std::lock_guard lock(reportedMutex);
                if (++timeouts % 20 == 0) std::fprintf(stderr, "[gpu] %llu GPU waits have timed out\n", static_cast<unsigned long long>(timeouts));
                if (!reported.insert(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u)).second) continue;
            }
            const bool wide = ((packet[0] >> 8u) & 0xffu) == 0x93u;
            const std::uint64_t reference = wide ? packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u) : packet[4];
            const std::uint64_t mask = wide ? packet[6] | (static_cast<std::uint64_t>(packet[7]) << 32u) : packet[5];
            const std::uint64_t current = wide ? *reinterpret_cast<const volatile std::uint64_t*>(awaited) : *reinterpret_cast<const volatile std::uint32_t*>(awaited);
            std::fprintf(stderr, "[gpu] queue 0x%x WAIT_REG_MEM%s%s at 0x%llx timed out after %dms (function %u ref 0x%llx mask 0x%llx value 0x%llx)\n", queue, wide ? "_64" : "", requireMemory ? " (landing)" : "",
                         static_cast<unsigned long long>(awaited), waitTimeoutMs(), packet[1] & 7u, static_cast<unsigned long long>(reference), static_cast<unsigned long long>(mask), static_cast<unsigned long long>(current));
            const auto address = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
            for (const auto& record : writeHistory()) {
                if (record.length != 0 && record.target <= address && address < record.target + std::max<std::uint64_t>(record.length, 4))
                    std::fprintf(stderr, "[gpu]   earlier write by queue 0x%x opcode 0x%x at 0x%llx+0x%llx\n", record.queue, record.opcode, static_cast<unsigned long long>(record.target), static_cast<unsigned long long>(record.length));
            }

            if (const auto reportDevice = device.Load(); reportDevice != nullptr) {
                Graphics::Recorder::LabelRefusal refusal{};
                const auto hit = reportDevice->PendingLabel(awaited, awaitedBytes, received, &refusal);
                const auto since = Graphics::Recorder::PendingLabelSince();
                const auto age = since.has_value() ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *since).count() : -1.0;
                std::fprintf(stderr, "[gpu]   recorder: table %s (refusal %u), open batch writes it %d, snapshot write overlaps %d, pending completion labels %llu, write-back completions %llu, pending label age %.1f ms, write generation %llu seen %llu, queue 0 dormant %d, completions pending %d, label due %d\n",
                             hit.has_value() ? "hit" : "miss", static_cast<unsigned>(refusal), reportDevice->OpenWriteOverlaps(awaited, awaitedBytes) ? 1 : 0, Graphics::Recorder::SnapshotWriteOverlaps(awaited, awaitedBytes) ? 1 : 0,
                             static_cast<unsigned long long>(Graphics::Recorder::PendingCompletionLabels()), static_cast<unsigned long long>(Graphics::Recorder::PendingWriteBackCompletions()), age,
                             static_cast<unsigned long long>(Graphics::Recorder::WriteGeneration()), static_cast<unsigned long long>(seenGeneration), queue0Dormant.load(std::memory_order_relaxed) ? 1 : 0, completionsPending() ? 1 : 0, labelFlushDue() ? 1 : 0);
            }
            context.Print(stderr, "[gpu]   preceding packet %s\n");
        }
    }
    ++outcomes.polled;
    account.outcome = 0;
}

}
