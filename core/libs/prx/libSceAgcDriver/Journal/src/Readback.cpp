#include "prx/libSceAgcDriver/Journal/include/Readback.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"

namespace AgcDriver::Journal {

void Settle(std::span<const Range> ranges) {
    DriverDetail::Driver::Get().WaitIdle();
    for (const auto& range : ranges) GuestMemory::FlushGpuWrites(range.address, range.bytes);
}

Frame ReadFrame(const DisplayBuffer& buffer) {
    return {buffer.width, buffer.height, ReadDisplayBuffer(buffer)};
}

}
