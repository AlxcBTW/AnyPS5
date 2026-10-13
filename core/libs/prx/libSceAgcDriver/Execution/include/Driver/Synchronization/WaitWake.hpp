#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SYNCHRONIZATION_WAITWAKE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SYNCHRONIZATION_WAITWAKE_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace AgcDriver::DriverDetail {

// Hot levers (Aps5Hot, flippable mid-run; each wait and each packet boundary reads the live value):
// APS5_EXP_WAIT_WAKE=1: a blocked WAIT_REG_MEM sleeps on WaitWake instead of PollSleep's plain timer.
bool WaitWakeOn();
// APS5_EXP_EARLY_LABEL_SUBMIT=1: landing and noted cross-queue waits announce their dword in AwaitedLabels,
// and the worker whose open batch (or queue of deferred labels) holds it submits at its next packet.
bool EarlyLabelSubmitOn();

// exp (APS5_EXP_WAIT_WAKE, never push): a queue worker polling a WAIT_REG_MEM sleeps in 200 us timer quanta
// (PollSleep) between its checks. WaitWake lets it sleep until either that bound passes or something that can
// satisfy the wait happens: a worker noted a label store (Driver::noteLabelStore), or completions were reaped
// (GPU progress). Signal() is cheap while nobody sleeps. A wake only ends the sleep early: the waiter repeats
// exactly the checks it would make after the timer, so every decision stays as it was.
class WaitWake {
public:
    // The current signal generation: read it before the checks a sleep follows, and pass it to Sleep.
    static std::uint64_t Generation();
    // Sleeps until a Signal() after `seen`, or `bound`. True when a signal ended (or prevented) the sleep.
    static bool Sleep(std::uint64_t seen, std::chrono::microseconds bound);
    static void Signal();
    struct Counts {
        std::uint64_t signals = 0;
        std::uint64_t woken = 0;
        std::uint64_t timedOut = 0;
    };
    // Since the last call (the [waitmem] line).
    static Counts TakeCounts();
};

// exp (APS5_EXP_EARLY_LABEL_SUBMIT, never push): the dwords queue workers are blocked on right now (a
// WAIT_REG_MEM that polls, or a landing wait), so a worker that has a label for one of them in its open batch
// submits that batch at once instead of at the label flush deadline (Driver::flushBetweenPackets). Only the
// submission time moves: the batch holds the same commands in the same order.
class AwaitedLabels {
public:
    static constexpr std::size_t Slots = 16;
    struct Entry;
    // Registers a blocked wait; null when every slot is taken (that wait is then simply not announced).
    static Entry* Register(std::uint32_t queue, std::uint64_t address, std::size_t bytes);
    static void Unregister(Entry* entry);
    // Blocked waits of queues other than `queue`.
    static std::size_t ActiveExcept(std::uint32_t queue);
    // Whether `writes(address, bytes)` holds for a blocked wait of a queue other than `queue`.
    static bool AnyExcept(std::uint32_t queue, const std::function<bool(std::uint64_t, std::size_t)>& writes);
    static void NoteEarlySubmit();
    static std::uint64_t TakeEarlySubmits();

    class Scope {
    public:
        Scope(std::uint32_t queue, std::uint64_t address, std::size_t bytes) : entry(Register(queue, address, bytes)) {}
        ~Scope() { Unregister(entry); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Entry* entry;
    };
};

}

#endif
