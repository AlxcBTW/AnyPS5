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

std::vector<std::uint32_t> Driver::VideoOutputHandles() {
    std::lock_guard lock(mutex);
    std::vector<std::uint32_t> handles;
    for (const auto& [handle, output] : outputs) handles.push_back(handle);
    return handles;
}

}
