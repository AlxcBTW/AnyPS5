#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitMemory.hpp"
#include <array>
#include <atomic>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

PollStats& Driver::pollStats() {
    static thread_local PollStats stats;
    return stats;
}

WaitOutcomes& Driver::waitOutcomes() {
    static thread_local WaitOutcomes outcomes;
    return outcomes;
}

Graphics::Recorder::LateStatistics& Driver::lateCountsSeen() {
    static thread_local Graphics::Recorder::LateStatistics seen{};
    return seen;
}

EpochBumps& Driver::epochBumps() {
    static thread_local EpochBumps bumps;
    return bumps;
}

namespace {
std::atomic<std::uint64_t> receiveClock{0};
std::array<std::atomic<std::uint64_t>, 64> queueLastReceive{};
thread_local std::uint64_t epochReceiveClock = 0;
}

void NoteQueueReceive(std::uint32_t queue) {
    const auto clock = receiveClock.fetch_add(1, std::memory_order_seq_cst) + 1;
    auto& slot = queueLastReceive[queue & 63u];
    auto seen = slot.load(std::memory_order_relaxed);
    while (seen < clock && !slot.compare_exchange_weak(seen, clock, std::memory_order_seq_cst)) {
    }
}

bool QueueQuietSinceEpoch(std::uint32_t queue) {
    return queueLastReceive[queue & 63u].load(std::memory_order_seq_cst) <= epochReceiveClock;
}

void Driver::bumpEpoch(std::uint64_t EpochBumps::*counter) {
    epochReceiveClock = receiveClock.load(std::memory_order_seq_cst);
    GuestMemory::BumpCollectEpoch();
    ++(epochBumps().*counter);
}

bool Driver::packetEpoch() {
    static const bool packet = std::getenv("APS5_PACKET_EPOCH") != nullptr;
    return packet;
}

}
