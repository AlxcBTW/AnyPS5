#include "prx/libSceAgcDriver/Journal/include/Readback.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include <cstring>

namespace AgcDriver::Journal {

void Settle(std::span<const Range> ranges) {
    DriverDetail::Driver::Get().WaitQuiescent();
    for (const auto& range : ranges) GuestMemory::FlushGpuWrites(range.address, range.bytes);
}

Frame ReadFrame(const DisplayBuffer& buffer) {
    if (buffer.dccAddress != 0) {
        GuestMemory::FlushGpuWrites(buffer.address, DisplayBufferSize(buffer));
        const auto keys = DisplayBufferKeys(buffer);
        if (keys != Graphics::DccKeys::Uncompressed) {
            const auto pixel = DisplayBufferClearPixel(buffer, keys);
            Frame frame{buffer.width, buffer.height, std::vector<std::byte>(static_cast<std::size_t>(buffer.width) * buffer.height * pixel.size())};
            for (std::size_t offset = 0; offset < frame.bgra.size(); offset += pixel.size()) std::memcpy(frame.bgra.data() + offset, pixel.data(), pixel.size());
            return frame;
        }
    }
    return {buffer.width, buffer.height, ReadDisplayBuffer(buffer)};
}

}
