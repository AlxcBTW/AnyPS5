#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitWake.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::AwaitedLabels;
using AgcDriver::DriverDetail::WaitWake;
using Clock = std::chrono::steady_clock;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void LeversDefaultOff() {
    Check(!AgcDriver::DriverDetail::WaitWakeOn(), "APS5_EXP_WAIT_WAKE is off unless set");
    Check(!AgcDriver::DriverDetail::EarlyLabelSubmitOn(), "APS5_EXP_EARLY_LABEL_SUBMIT is off unless set");
}

// A signal between reading the generation and sleeping must end the sleep at once.
void SignalBeforeSleep() {
    WaitWake::TakeCounts();
    const auto seen = WaitWake::Generation();
    WaitWake::Signal();
    const auto start = Clock::now();
    Check(WaitWake::Sleep(seen, std::chrono::seconds(5)), "a signal after the generation was read ends the sleep");
    Check(Clock::now() - start < std::chrono::seconds(1), "the sleep did not wait for its bound");
    Check(WaitWake::TakeCounts().woken == 1, "the sleep is counted as woken");
}

// Without a signal the sleep runs to its bound and reports so.
void BoundWithoutSignal() {
    WaitWake::TakeCounts();
    const auto seen = WaitWake::Generation();
    const auto start = Clock::now();
    Check(!WaitWake::Sleep(seen, std::chrono::milliseconds(3)), "no signal: the bound ends the sleep");
    Check(Clock::now() - start >= std::chrono::milliseconds(2), "the sleep lasted about its bound");
    Check(WaitWake::TakeCounts().timedOut == 1, "the sleep is counted as timed out");
}

// Another thread's signal wakes a sleeper long before its bound, in every interleaving.
void CrossThreadWake() {
    for (int round = 0; round < 20; ++round) {
        std::atomic<bool> about{false};
        std::atomic<bool> result{false};
        std::atomic<long long> elapsedMs{0};
        std::thread sleeper([&] {
            const auto seen = WaitWake::Generation();
            about.store(true);
            const auto start = Clock::now();
            result.store(WaitWake::Sleep(seen, std::chrono::seconds(5)));
            elapsedMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        });
        while (!about.load()) std::this_thread::yield();
        if (round % 2 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        WaitWake::Signal();
        sleeper.join();
#ifdef _WIN32
        Check(result.load(), "the signal woke the sleeper");
        Check(elapsedMs.load() < 1000, "the sleeper woke well before its 5 s bound");
#else
        (void)result;
        Check(elapsedMs.load() <= 6000, "the sleeper returned");
#endif
    }
}

void AwaitedRegistry() {
    Check(AwaitedLabels::ActiveExcept(0) == 0, "no waits announced at start");
    {
        AwaitedLabels::Scope scope(0x20, 0x1000, 4);
        Check(AwaitedLabels::ActiveExcept(0) == 1, "queue 0 sees queue 0x20's wait");
        Check(AwaitedLabels::ActiveExcept(0x20) == 0, "a queue does not see its own wait");
        const auto overlaps = [](std::uint64_t begin, std::size_t bytes) {
            return [=](std::uint64_t address, std::size_t length) { return begin < address + length && address < begin + bytes; };
        };
        Check(AwaitedLabels::AnyExcept(0, overlaps(0x1000, 4)), "a label on the awaited dword matches");
        Check(AwaitedLabels::AnyExcept(0, overlaps(0xffc, 8)), "a wider label over the dword matches");
        Check(!AwaitedLabels::AnyExcept(0, overlaps(0x1004, 4)), "the next dword does not match");
        Check(!AwaitedLabels::AnyExcept(0x20, overlaps(0x1000, 4)), "the waiting queue's own labels never match");
    }
    Check(AwaitedLabels::ActiveExcept(0) == 0, "the scope removed its wait");

    std::vector<AwaitedLabels::Entry*> entries;
    for (std::size_t index = 0; index < AwaitedLabels::Slots; ++index) {
        entries.push_back(AwaitedLabels::Register(0x20, 0x2000 + index * 8, 4));
        Check(entries.back() != nullptr, "a free slot registers");
    }
    Check(AwaitedLabels::Register(0x20, 0x9000, 4) == nullptr, "a full table refuses (the wait is then just not announced)");
    Check(AwaitedLabels::ActiveExcept(0) == AwaitedLabels::Slots, "every slot is in use");
    for (auto* entry : entries) AwaitedLabels::Unregister(entry);
    AwaitedLabels::Unregister(nullptr);
    Check(AwaitedLabels::ActiveExcept(0) == 0, "every slot was freed");

    AwaitedLabels::TakeEarlySubmits();
    AwaitedLabels::NoteEarlySubmit();
    AwaitedLabels::NoteEarlySubmit();
    Check(AwaitedLabels::TakeEarlySubmits() == 2, "early submits are counted");
    Check(AwaitedLabels::TakeEarlySubmits() == 0, "and taken");
}

}

int main() {
    try {
        LeversDefaultOff();
        SignalBeforeSleep();
        BoundWithoutSignal();
        CrossThreadWake();
        AwaitedRegistry();
        std::cout << "wait wake tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
