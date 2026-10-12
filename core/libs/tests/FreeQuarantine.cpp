#include "prx/libc/include/FreeQuarantine.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using FreeQuarantine::Entry;
using FreeQuarantine::Fifo;
using FreeQuarantine::Release;
using FreeQuarantine::Taken;

namespace {

constexpr std::uint64_t Second = 1000000;

void require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

Entry block(std::uint64_t pointer, std::uint64_t bytes, std::uint64_t heldAt) {
    Entry entry;
    entry.pointer = pointer;
    entry.poolId = pointer ^ 0x40000005u;
    entry.bytes = bytes;
    entry.heldAt = heldAt;
    entry.caller = 0x52de6da;
    return entry;
}

std::vector<Taken> take(Fifo& fifo, std::uint64_t now, bool pressure = false, std::size_t capacity = 64) {
    std::vector<Taken> out(capacity);
    out.resize(fifo.TakeDue(now, pressure, out.data(), capacity));
    return out;
}

void holdsUntilTheHoldTime() {
    Fifo fifo(300 * Second, 0);
    fifo.Push(block(0x1000, 2048, 0));
    fifo.Push(block(0x2000, 4096, 1 * Second));
    fifo.Push(block(0x3000, 2048, 2 * Second));
    require(take(fifo, 299 * Second).empty(), "nothing is due before the hold time");
    require(fifo.Count() == 3 && fifo.Bytes() == 8192, "three blocks, 8 KiB held");
    require(fifo.OldestAge(299 * Second) == 299 * Second, "oldest age");
    auto due = take(fifo, 300 * Second);
    require(due.size() == 1 && due[0].entry.pointer == 0x1000 && due[0].why == Release::Age && due[0].age == 300 * Second, "the first block leaves at exactly the hold time");
    require(due[0].entry.poolId == (0x1000 ^ 0x40000005u) && due[0].entry.caller == 0x52de6da, "an entry comes back unchanged");
    due = take(fifo, 302 * Second);
    require(due.size() == 2 && due[0].entry.pointer == 0x2000 && due[1].entry.pointer == 0x3000, "the rest leave in order");
    require(fifo.Count() == 0 && fifo.Bytes() == 0 && fifo.OldestAge(400 * Second) == 0, "empty again");
    require(take(fifo, 1000 * Second).empty(), "an empty FIFO has nothing due");
}

void capReleasesTheOldestEarly() {
    Fifo fifo(300 * Second, 10 * 1024);
    fifo.Push(block(0x1000, 4096, 0));
    fifo.Push(block(0x2000, 4096, 1));
    require(take(fifo, 2).empty(), "8 KiB is under the 10 KiB cap");
    fifo.Push(block(0x3000, 4096, 2));
    auto due = take(fifo, 3);
    require(due.size() == 1 && due[0].entry.pointer == 0x1000 && due[0].why == Release::Cap && due[0].age == 3, "12 KiB > cap: only the oldest leaves");
    require(fifo.Bytes() == 8192 && fifo.Count() == 2, "back under the cap");
    fifo.Push(block(0x4000, 64 * 1024, 4));
    due = take(fifo, 5);
    require(due.size() == 3 && due[0].entry.pointer == 0x2000 && due[1].entry.pointer == 0x3000 && due[2].entry.pointer == 0x4000, "a block bigger than the cap pushes everything out, itself last");
    require(due[2].why == Release::Cap && fifo.Count() == 0 && fifo.Bytes() == 0, "nothing left");
}

void ageComesBeforeCap() {
    Fifo fifo(10 * Second, 4096);
    fifo.Push(block(0x1000, 4096, 0));
    fifo.Push(block(0x2000, 4096, 20 * Second));
    const auto due = take(fifo, 20 * Second);
    require(due.size() == 1 && due[0].why == Release::Age, "an old block counts as released by age even over the cap");
    require(fifo.Count() == 1 && fifo.Bytes() == 4096, "the young one stays at the cap");
}

void pressureAndBatch() {
    Fifo fifo(300 * Second, 0);
    for (std::uint64_t i = 0; i < 20; ++i) fifo.Push(block(0x1000 + i * 0x1000, 2048, i));
    require(take(fifo, 100).empty(), "no cap, no pressure: nothing due");
    auto due = take(fifo, 100, true, 8);
    require(due.size() == 8 && due[0].why == Release::Pressure && due[0].entry.pointer == 0x1000 && due[7].entry.pointer == 0x8000, "pressure takes one batch, oldest first");
    due = take(fifo, 400 * Second, false, 8);
    require(due.size() == 8 && due[0].entry.pointer == 0x9000 && due[0].why == Release::Age, "the batch limit holds for age releases too");
    require(fifo.Count() == 4, "four left");
}

void orderSurvivesGrowthAndWrap() {
    Fifo fifo(1000, 0);
    std::uint64_t pushed = 0;
    std::uint64_t expected = 0;
    std::uint64_t now = 0;
    // Interleave pushes and takes so the ring wraps, then push enough to grow it twice while wrapped.
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 3000; ++i) fifo.Push(block(++pushed, 16, now++));
        for (const auto& taken : take(fifo, now + 1000, false, 2500)) {
            require(taken.entry.pointer == ++expected, "FIFO order across wrap-around");
        }
    }
    for (int i = 0; i < 20000; ++i) fifo.Push(block(++pushed, 16, now++));
    require(fifo.Count() == pushed - expected && fifo.Bytes() == 16 * (pushed - expected), "count and bytes after growth");
    for (;;) {
        const auto due = take(fifo, ~0ull >> 1, false, 4096);
        if (due.empty()) break;
        for (const auto& taken : due) require(taken.entry.pointer == ++expected, "FIFO order across growth");
    }
    require(expected == pushed && fifo.Count() == 0 && fifo.Bytes() == 0, "every block came out once");
}

void clockBehindTheEntry() {
    Fifo fifo(5, 0);
    fifo.Push(block(0x1000, 16, 100));
    require(take(fifo, 50).empty() && fifo.OldestAge(50) == 0, "a time before the entry counts as age 0");
}

void parse() {
    std::uint64_t hold = 0;
    std::uint64_t cap = 0;
    require(FreeQuarantine::ParseSpec("300", hold, cap) && hold == 300 * Second && cap == FreeQuarantine::DefaultCapMiB << 20, "seconds alone take the default cap");
    require(FreeQuarantine::ParseSpec("300,1024", hold, cap) && hold == 300 * Second && cap == 1024ull << 20, "seconds and cap");
    require(FreeQuarantine::ParseSpec("0.5,0", hold, cap) && hold == 500000 && cap == 0, "fractions and no cap");
    require(!FreeQuarantine::ParseSpec("", hold, cap), "empty");
    require(!FreeQuarantine::ParseSpec("0", hold, cap), "zero seconds");
    require(!FreeQuarantine::ParseSpec("-5", hold, cap), "negative seconds");
    require(!FreeQuarantine::ParseSpec("300,", hold, cap), "missing cap");
    require(!FreeQuarantine::ParseSpec("300,-1", hold, cap), "negative cap");
    require(!FreeQuarantine::ParseSpec("300x", hold, cap), "trailing text");
    require(!FreeQuarantine::ParseSpec("300,512,1", hold, cap), "extra field");
    require(!FreeQuarantine::ParseSpec(nullptr, hold, cap), "null");
}

}  // namespace

int main() {
    try {
        holdsUntilTheHoldTime();
        capReleasesTheOldestEarly();
        ageComesBeforeCap();
        pressureAndBatch();
        orderSurvivesGrowthAndWrap();
        clockBehindTheEntry();
        parse();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "free quarantine test failed: %s\n", error.what());
        return 1;
    }
    std::puts("free quarantine tests passed");
    return 0;
}
