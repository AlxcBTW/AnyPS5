#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <cstdlib>
#include <mutex>
#include <set>
#ifdef _WIN32
#include <windows.h>
#endif
#include <immintrin.h>
#include <algorithm>
#include <tuple>
#include <vector>
#include <array>
#include <map>
#include <optional>

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

void Driver::notePendingWait(std::span<const std::uint32_t> packet, std::uint32_t queue, std::uint64_t received) {
    if (packet.size() > std::tuple_size_v<decltype(PendingWait::words)>) return;
    static const bool allQueues = std::getenv("APS5_LAND_WAITS_ALL_QUEUES") != nullptr;
    if (!allQueues && queue == 0) return;
    if (Pm4::WaitSatisfiedUnchecked(packet)) return;
    auto& waits = pendingWaits();
    for (const auto& wait : waits) {
        if (wait.count == packet.size() && std::equal(packet.begin(), packet.end(), wait.words.begin())) return;
    }
    PendingWait wait{};
    std::copy(packet.begin(), packet.end(), wait.words.begin());
    wait.count = packet.size();
    wait.received = received;
    // exp (APS5_EXP_EARLY_LABEL_SUBMIT): this queue will land the wait before its next guest read, so the
    // label's writer should not leave it in an open batch until then.
    if (EarlyLabelSubmitOn()) wait.awaited = AwaitedLabels::Register(queue, packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u), Pm4::WaitAwaitedBytes(packet));
    waits.push_back(wait);
}

void Driver::landPendingWaits(std::uint32_t queue) {
    auto& waits = pendingWaits();
    if (waits.empty()) return;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    struct LandStats {
        std::uint64_t lands = 0, waited = 0;
        double waitedMs = 0, maxMs = 0;
        std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
    };
    static thread_local LandStats stats;
    const PacketHistory none{};
    for (const auto& wait : waits) {
        const auto packet = std::span<const std::uint32_t>(wait.words).first(wait.count);
        ++stats.lands;
        if (Pm4::WaitSatisfiedUnchecked(packet)) continue;
        const auto started = std::chrono::steady_clock::now();
        waitMemory(packet, queue, none, wait.received, false, true);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        ++stats.waited;
        stats.waitedMs += ms;
        stats.maxMs = std::max(stats.maxMs, ms);
    }
    for (const auto& wait : waits) AwaitedLabels::Unregister(wait.awaited);
    waits.clear();
    if (profile && std::chrono::steady_clock::now() - stats.reported > std::chrono::seconds(10)) {
        AgcDriver::ProfilePrint_nid_no_patch("[landwait] queue 0x%x (10 s): %llu waits landed before a guest read, %llu had to wait for the GPU: %.1f ms in total, max %.2f ms\n", queue, static_cast<unsigned long long>(stats.lands), static_cast<unsigned long long>(stats.waited), stats.waitedMs, stats.maxMs);
        stats = LandStats{};
    }
}

// exp (APS5_PROFILE_DRAW, never push; diagnostic, no behaviour change): the [waitmem] census. Every
// WAIT_REG_MEM not satisfied at entry is timed and classified by how it ended: the value was stored by a
// packet of another queue or of the waiting one after the wait was received (storedSince), the recorder's
// label table holds it (recorded GPU work of that queue writes it), the recorded writes settled (a
// landing wait), or memory showed it (polled: a GPU write landed, or a store nobody noted). Per 10 s and
// waiting queue: count and ms by end, the waits that made a submit, and the addresses that cost the most
// with the last writer the packet history knows for them (queue and opcode: 0x49 RELEASE_MEM, 0x37
// WRITE_DATA, 0x40 COPY_DATA, 0x50 DMA_DATA).
namespace {

enum class WaitEnd : std::size_t { StoredOther, StoredSame, LabelOther, LabelSame, Settled, Memory, Count };
constexpr std::array<const char*, static_cast<std::size_t>(WaitEnd::Count)> WaitEndNames{"stored by another queue", "stored by its own queue", "label table, another queue", "label table, own queue", "recorded writes settled", "memory (polled)"};

struct WaitCensus {
    struct PerQueue {
        std::array<std::uint64_t, static_cast<std::size_t>(WaitEnd::Count)> count{};
        std::array<double, static_cast<std::size_t>(WaitEnd::Count)> ms{};
        std::uint64_t atEntry = 0, submitted = 0, sleeps = 0;
        struct Address {
            std::uint64_t count = 0;
            double ms = 0;
            std::uint32_t writerQueue = 0xffffffffu;
            std::uint32_t writerOpcode = 0;
            std::array<std::uint64_t, static_cast<std::size_t>(WaitEnd::Count)> ends{};
        };
        std::map<std::uint64_t, Address> addresses;
    };
    std::mutex mutex;
    std::map<std::uint32_t, PerQueue> queues;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};

WaitCensus& Census() {
    static WaitCensus census;
    return census;
}

bool CensusOn() {
    static const bool on = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return on;
}

}

void Driver::waitMemory(std::span<const std::uint32_t> packet, std::uint32_t queue, const PacketHistory& context, std::uint64_t received, bool heldAtSubmit, bool requireMemory) {

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
        if (CensusOn()) {
            std::lock_guard lock(Census().mutex);
            ++Census().queues[queue].atEntry;
        }
        return;
    }
    if (heldAtSubmit) return;
    struct CensusWait {
        Driver& driver;
        bool on;
        std::uint32_t queue;
        std::uint64_t address;
        std::chrono::steady_clock::time_point begin;
        WaitEnd end = WaitEnd::Memory;
        bool submitted = false;
        bool slept = false;
        ~CensusWait() {
            if (!on) return;
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            // The newest write the packet history knows for the awaited dword (a ring the packet loop fills).
            std::uint32_t writerQueue = 0xffffffffu, writerOpcode = 0;
            const auto& history = Driver::writeHistory();
            const auto cursor = Driver::writeCursor();
            for (std::size_t back = 1; back <= history.size() && back <= cursor; ++back) {
                const auto& record = history[(cursor - back) % history.size()];
                if (record.length != 0 && record.target <= address && address < record.target + std::max<std::uint64_t>(record.length, 4)) {
                    writerQueue = record.queue;
                    writerOpcode = record.opcode;
                    break;
                }
            }
            auto& census = Census();
            std::lock_guard lock(census.mutex);
            auto& perQueue = census.queues[queue];
            const auto index = static_cast<std::size_t>(end);
            ++perQueue.count[index];
            perQueue.ms[index] += ms;
            if (submitted) ++perQueue.submitted;
            if (slept) ++perQueue.sleeps;
            auto& entry = perQueue.addresses[address];
            ++entry.count;
            entry.ms += ms;
            ++entry.ends[index];
            if (writerQueue != 0xffffffffu) {
                entry.writerQueue = writerQueue;
                entry.writerOpcode = writerOpcode;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - census.reported < std::chrono::seconds(10)) return;
            census.reported = now;
            for (auto& [waiting, stats] : census.queues) {
                std::string line;
                char item[256];
                std::uint64_t waits = 0;
                double total = 0;
                for (std::size_t kind = 0; kind < stats.count.size(); ++kind) {
                    waits += stats.count[kind];
                    total += stats.ms[kind];
                    if (stats.count[kind] == 0) continue;
                    std::snprintf(item, sizeof(item), "%s %s %llu/%.0fms", line.empty() ? "" : ",", WaitEndNames[kind], static_cast<unsigned long long>(stats.count[kind]), stats.ms[kind]);
                    line += item;
                }
                std::vector<std::pair<std::uint64_t, const WaitCensus::PerQueue::Address*>> top;
                for (const auto& [where, stat] : stats.addresses) top.emplace_back(where, &stat);
                std::sort(top.begin(), top.end(), [](const auto& left, const auto& right) { return left.second->ms > right.second->ms; });
                std::string tops;
                for (std::size_t rank = 0; rank < top.size() && rank < 8; ++rank) {
                    const auto& stat = *top[rank].second;
                    std::size_t mainEnd = 0;
                    for (std::size_t kind = 1; kind < stat.ends.size(); ++kind) {
                        if (stat.ends[kind] > stat.ends[mainEnd]) mainEnd = kind;
                    }
                    std::snprintf(item, sizeof(item), " 0x%llx x%llu %.0fms (writer queue 0x%x opcode 0x%x; mostly %s)", static_cast<unsigned long long>(top[rank].first), static_cast<unsigned long long>(stat.count), stat.ms, stat.writerQueue, stat.writerOpcode, WaitEndNames[mainEnd]);
                    tops += item;
                }
                AgcDriver::ProfilePrint_nid_no_patch("[waitmem] queue 0x%x (10 s): %llu waits satisfied at entry; %llu blocked %.0f ms by end:%s; %llu made a submit, %llu slept; top addresses:%s\n", waiting, static_cast<unsigned long long>(stats.atEntry), static_cast<unsigned long long>(waits), total, line.c_str(), static_cast<unsigned long long>(stats.submitted), static_cast<unsigned long long>(stats.sleeps), tops.c_str());
                stats = WaitCensus::PerQueue{};
            }
            const auto wakes = WaitWake::TakeCounts();
            AgcDriver::ProfilePrint_nid_no_patch("[waitwake] (10 s): WAIT_WAKE %d: sleeps ended by a label store %llu, ran to the 200 us bound %llu, signals sent to sleepers %llu; EARLY_LABEL_SUBMIT %d: batches submitted early for an awaited label %llu\n", WaitWakeOn() ? 1 : 0, static_cast<unsigned long long>(wakes.woken), static_cast<unsigned long long>(wakes.timedOut), static_cast<unsigned long long>(wakes.signals), EarlyLabelSubmitOn() ? 1 : 0, static_cast<unsigned long long>(AwaitedLabels::TakeEarlySubmits()));
        }
    } censusWait{*this, CensusOn(), queue, awaited, std::chrono::steady_clock::now()};
    if (std::uint32_t writer = queue; !requireMemory && storedSince(packet, awaited, awaitedBytes, received, &writer)) {
        censusWait.end = writer != queue ? WaitEnd::StoredOther : WaitEnd::StoredSame;
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
        censusWait.end = hit->queue == queue ? WaitEnd::LabelSame : WaitEnd::LabelOther;
        if (unlockedHit) ++outcomes.fromRecorderUnlocked;
        if (hit->queue == queue) {
            ++outcomes.fromRecorderSameQueue;
            if (!hit->late) epochPoint.bump = false;
        } else {
            notePendingWait(packet, queue, received);
        }
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

            if (!overlapSubmit || localDevice->OpenWriteOverlaps(awaited, awaitedBytes)) {
                localDevice->SubmitRecorded(false);
                censusWait.submitted = true;
            }
        }
    }

    static const bool pollProfile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& poll = pollStats();
    const auto pollRefused = [&](Graphics::Recorder::LabelRefusal refusal) {
        if (pollProfile && refusal != Graphics::Recorder::LabelRefusal::None && static_cast<std::size_t>(refusal) < poll.refusals.size()) ++poll.refusals[static_cast<std::size_t>(refusal)];
    };

    const bool reapAll = pollReapAll() || !labelShortcut || queue == 0;
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
            censusWait.submitted = true;
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

    if (entryTryFailed && pollService(false)) return;

    noteWaitBlocked(queue, awaited, true);
    struct BlockedWait {
        Driver& driver;
        std::uint32_t queue;
        ~BlockedWait() { driver.noteWaitBlocked(queue, 0, false); }
    } blocked{*this, queue};
    std::optional<AwaitedLabels::Scope> awaitedScope;
    if (requireMemory && EarlyLabelSubmitOn()) awaitedScope.emplace(queue, awaited, awaitedBytes);
    const bool wakeOn = WaitWakeOn();
    std::uint64_t wakeSeen = 0;
    static const bool pauseSpin = std::getenv("APS5_NO_PAUSE_SPIN") == nullptr;
    const auto spinLimit = queue == 0 ? std::chrono::microseconds(1500) : std::chrono::microseconds(100);
    const auto spinStart = std::chrono::steady_clock::now();
    bool spinning = pauseSpin;
    std::uint32_t polls = 0;
    while (!Pm4::WaitSatisfiedUnchecked(packet)) {
        // Read before this round's checks: a store noted after them bumps it, so the sleep below cannot miss it.
        if (wakeOn) wakeSeen = WaitWake::Generation();
        if (std::uint32_t writer = queue; !requireMemory && storedSince(packet, awaited, awaitedBytes, received, &writer)) {
            censusWait.end = writer != queue ? WaitEnd::StoredOther : WaitEnd::StoredSame;
            if (writer != queue) notePendingWait(packet, queue, received);
            else epochPoint.bump = false;
            return;
        }
        ++polls;
        if (spinning) {
            _mm_pause();
            if ((polls & 63u) != 0) continue;
            if (std::chrono::steady_clock::now() - spinStart > spinLimit) spinning = false;
        } else if (pauseSpin || polls >= 4000) {
            censusWait.slept = true;
            if (wakeOn) WaitWake::Sleep(wakeSeen, std::chrono::microseconds(200));
            else PollSleep();
        } else {
            std::this_thread::yield();
        }
        CheckFailure();
        if (pollService(!spinning)) return;
        noteAwaitingTitle(queue, awaited);
        if (requireMemory && (polls & 15u) == 0) {
            std::unique_lock settleLock(GuestMemory::GpuMutex(), std::try_to_lock);
            if (settleLock.owns_lock()) {
                if (const auto localDevice = device.Load(); localDevice != nullptr && localDevice->RecordedWritesSettled(awaited, awaitedBytes)) {
                    censusWait.end = WaitEnd::Settled;
                    return;
                }
            }
        }
        if (const auto done = packetsDone.load(); done != lastDone || (!requireMemory && packetsInFlight.load() != 0)) {
            lastDone = done;
            start = std::chrono::steady_clock::now();
        }

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
}

}
