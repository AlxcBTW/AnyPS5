#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_INDIRECTPUBLISH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_INDIRECTPUBLISH_HPP

#include <cstddef>
#include <cstdint>

namespace AgcDriver::Graphics::IndirectPublish {

bool Enabled();

// The flush a publish belongs to, kept for the thread while FlushPending or PublishShadowsOnly runs
// (plain data, restored on exit).
class FlushScope {
public:
    FlushScope(const char* reason, std::uint64_t address, std::size_t bytes);
    ~FlushScope();
    FlushScope(const FlushScope&) = delete;
    FlushScope& operator=(const FlushScope&) = delete;

private:
    const char* previousReason;
    std::uint64_t previousAddress;
    std::size_t previousBytes;
    bool active;
};

void NoteImagesStored(std::size_t images);
void NotePublish(std::uint64_t bytes, std::size_t units);
void ReportIfDue();

}

#endif
