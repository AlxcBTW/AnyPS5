#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitWake.hpp"
#include "HotLever.hpp"
#include <algorithm>
#include <array>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AgcDriver::DriverDetail {

namespace {

std::atomic<std::uint64_t> generation{0};
std::atomic<std::uint64_t> signals{0}, woken{0}, timedOut{0}, gpuCompleted{0}, gpuBound{0};

#ifdef _WIN32
// One auto-reset event per sleeping thread (the queue workers), so a Signal wakes every sleeper and an
// event set while its thread was awake ends at most its next sleep early.
struct Sleeper {
    std::atomic<bool> sleeping{false};
    HANDLE event = nullptr;
};
constexpr std::size_t SleeperSlots = 32;
std::array<Sleeper, SleeperSlots> sleepers;
std::atomic<std::size_t> sleeperCount{0};
std::atomic<std::uint32_t> sleepingNow{0};

struct ThreadSleep {
    Sleeper* slot = nullptr;
    HANDLE timer = nullptr;
    ThreadSleep() {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const auto index = sleeperCount.fetch_add(1, std::memory_order_acq_rel);
        if (index < SleeperSlots) {
            slot = &sleepers[index];
            slot->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
    }
};
#endif

std::int64_t envInteger(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? static_cast<std::int64_t>(std::strtoll(value, nullptr, 10)) : 0;
}

}

namespace {

const Aps5Hot::Lever& waitWakeLever() {
    static const Aps5Hot::Lever lever("APS5_EXP_WAIT_WAKE", envInteger("APS5_EXP_WAIT_WAKE"));
    return lever;
}

}

bool WaitWakeOn() {
    return waitWakeLever().On();
}

std::int64_t WaitWakeMode() {
    return waitWakeLever().Value();
}

bool EarlyLabelSubmitOn() {
    static const Aps5Hot::Lever lever("APS5_EXP_EARLY_LABEL_SUBMIT", envInteger("APS5_EXP_EARLY_LABEL_SUBMIT"));
    return lever.On();
}

std::uint64_t WaitWake::Generation() {
    return generation.load(std::memory_order_seq_cst);
}

bool WaitWake::Sleep(std::uint64_t seen, std::chrono::microseconds bound) {
#ifdef _WIN32
    thread_local ThreadSleep self;
    if (self.slot != nullptr && self.slot->event != nullptr && self.timer != nullptr) {
        // Announce the sleep, then look at the generation once more: a Signal between the caller's checks and
        // here has either bumped it (no sleep) or will find this thread sleeping and set its event.
        self.slot->sleeping.store(true, std::memory_order_seq_cst);
        sleepingNow.fetch_add(1, std::memory_order_seq_cst);
        bool signalled = generation.load(std::memory_order_seq_cst) != seen;
        if (!signalled) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(bound.count()) * 10;
            if (SetWaitableTimer(self.timer, &due, 0, nullptr, nullptr, FALSE)) {
                const std::array<HANDLE, 2> handles{self.slot->event, self.timer};
                signalled = WaitForMultipleObjects(2, handles.data(), FALSE, INFINITE) == WAIT_OBJECT_0;
                if (signalled) CancelWaitableTimer(self.timer);
            } else {
                signalled = WaitForSingleObject(self.slot->event, static_cast<DWORD>((bound.count() + 999) / 1000)) == WAIT_OBJECT_0;
            }
        }
        sleepingNow.fetch_sub(1, std::memory_order_seq_cst);
        self.slot->sleeping.store(false, std::memory_order_seq_cst);
        (signalled ? woken : timedOut).fetch_add(1, std::memory_order_relaxed);
        return signalled;
    }
#endif
    if (generation.load(std::memory_order_seq_cst) != seen) {
        woken.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::this_thread::sleep_for(bound);
    timedOut.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void WaitWake::Signal() {
    generation.fetch_add(1, std::memory_order_seq_cst);
#ifdef _WIN32
    if (sleepingNow.load(std::memory_order_seq_cst) == 0) return;
    signals.fetch_add(1, std::memory_order_relaxed);
    const auto count = std::min(sleeperCount.load(std::memory_order_acquire), SleeperSlots);
    for (std::size_t index = 0; index < count; ++index) {
        auto& sleeper = sleepers[index];
        if (sleeper.event != nullptr && sleeper.sleeping.load(std::memory_order_seq_cst)) SetEvent(sleeper.event);
    }
#endif
}

void WaitWake::NoteGpuSleep(bool completed) {
    (completed ? gpuCompleted : gpuBound).fetch_add(1, std::memory_order_relaxed);
}

WaitWake::Counts WaitWake::TakeCounts() {
    return {signals.exchange(0, std::memory_order_relaxed), woken.exchange(0, std::memory_order_relaxed), timedOut.exchange(0, std::memory_order_relaxed), gpuCompleted.exchange(0, std::memory_order_relaxed), gpuBound.exchange(0, std::memory_order_relaxed)};
}

struct AwaitedLabels::Entry {
    std::atomic<bool> used{false};
    std::atomic<std::uint32_t> queue{0};
    std::atomic<std::uint64_t> address{0};
    std::atomic<std::size_t> bytes{0};
};

namespace {

std::array<AwaitedLabels::Entry, AwaitedLabels::Slots> awaited;
std::atomic<std::uint64_t> earlySubmits{0};

}

AwaitedLabels::Entry* AwaitedLabels::Register(std::uint32_t queue, std::uint64_t address, std::size_t bytes) {
    for (auto& entry : awaited) {
        bool expected = false;
        if (!entry.used.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;
        entry.queue.store(queue, std::memory_order_relaxed);
        entry.bytes.store(bytes, std::memory_order_relaxed);
        // Published last: a reader that sees the address sees the rest.
        entry.address.store(address, std::memory_order_release);
        return &entry;
    }
    return nullptr;
}

void AwaitedLabels::Unregister(Entry* entry) {
    if (entry == nullptr) return;
    entry->address.store(0, std::memory_order_release);
    entry->used.store(false, std::memory_order_release);
}

std::size_t AwaitedLabels::ActiveExcept(std::uint32_t queue) {
    std::size_t active = 0;
    for (const auto& entry : awaited) {
        if (entry.address.load(std::memory_order_acquire) != 0 && entry.queue.load(std::memory_order_relaxed) != queue) ++active;
    }
    return active;
}

bool AwaitedLabels::AnyExcept(std::uint32_t queue, const std::function<bool(std::uint64_t, std::size_t)>& writes) {
    for (const auto& entry : awaited) {
        const auto address = entry.address.load(std::memory_order_acquire);
        if (address == 0 || entry.queue.load(std::memory_order_relaxed) == queue) continue;
        if (writes(address, entry.bytes.load(std::memory_order_relaxed))) return true;
    }
    return false;
}

void AwaitedLabels::NoteEarlySubmit() {
    earlySubmits.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t AwaitedLabels::TakeEarlySubmits() {
    return earlySubmits.exchange(0, std::memory_order_relaxed);
}

}
