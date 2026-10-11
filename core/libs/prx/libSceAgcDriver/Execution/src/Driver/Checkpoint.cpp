#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"

namespace AgcDriver::DriverDetail {

Driver::QueueCheckpoint Driver::Checkpoint() {
    std::lock_guard lock(mutex);
    rethrowFailure();
    require(completed == accepted, "a queue checkpoint needs every accepted submission completed");
    return {queues, resetGraphics};
}

void Driver::Restore(QueueCheckpoint checkpoint) {
    std::lock_guard lock(mutex);
    rethrowFailure();
    require(completed == accepted, "restoring queue state needs every accepted submission completed");
    queues = std::move(checkpoint.queues);
    resetGraphics = checkpoint.resetGraphics;
}

bool Driver::WaitQuiescent() {
    require(!onWorkerThread(), "worker cannot wait for the driver to be quiescent");
    constexpr int StableRounds = 4;
    std::unique_lock lock(mutex);
    ++idleWaiters;
    struct Waiter {
        std::uint32_t& count;
        ~Waiter() { --count; }
    } waiter{idleWaiters};
    auto seen = std::pair{completed, packetsDone.load(std::memory_order_acquire)};
    for (int stable = 0; stable < StableRounds;) {
        rethrowFailure();
        checkStopping();
        if (completed >= accepted) return true;
        changed.wait_for(lock, std::chrono::milliseconds(5));
        const auto now = std::pair{completed, packetsDone.load(std::memory_order_acquire)};
        const bool awaitsTitle = runningWorkers.load(std::memory_order_acquire) == 0 && orderHolders.load(std::memory_order_acquire) == 0 && !completionsPending();
        stable = awaitsTitle && now == seen ? stable + 1 : 0;
        seen = now;
    }
    return false;
}

std::vector<std::uint32_t> Driver::VideoOutputHandles() {
    std::lock_guard lock(mutex);
    std::vector<std::uint32_t> handles;
    for (const auto& [handle, output] : outputs) handles.push_back(handle);
    return handles;
}

}
