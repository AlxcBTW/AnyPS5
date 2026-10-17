#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_HOOKCALLERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_HOOKCALLERS_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace AgcDriver::Graphics::HookCallers {

void NotePublish(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uint64_t bytes, std::size_t units);
void ReportIfDue();

}

#endif
