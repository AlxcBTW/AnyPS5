#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SCRATCHWINDOWUNION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SCRATCHWINDOWUNION_HPP

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace AgcDriver::Graphics {

// Matches TextureDetiler's buffer descriptor: align the offset down, include its residual
// base in the range, then round that range to a whole word. Check before widening a window.
inline bool ScratchWindowDescriptorFits(std::uint64_t offset, std::uint64_t bytes,
    std::uint64_t minimumAlignment, std::uint64_t maximumRange) {
    const auto alignment = minimumAlignment < 4 ? 4 : minimumAlignment;
    const auto base = offset % alignment;
    if (bytes == 0 || base > std::numeric_limits<std::uint32_t>::max()) return false;
    if (bytes > std::numeric_limits<std::uint64_t>::max() - base - 3) return false;
    const auto range = (base + bytes + 3) / 4 * 4;
    return range <= maximumRange;
}

// A scratch-only envelope. The caller must still publish only the original runs, and must
// establish that the slice is one non-tail thin mip of one image layer. Refusal keeps the
// original per-run windows. No sorting, rounding, or ownership changes are made here.
inline std::optional<std::pair<std::uint64_t, std::uint64_t>> PlanScratchWindowUnion(
    std::span<const std::pair<std::uint64_t, std::uint64_t>> runs,
    std::uint64_t sliceBegin, std::uint64_t sliceEnd, std::uint64_t blockBytes) {
    if (runs.size() < 2 || blockBytes == 0 || sliceBegin >= sliceEnd) return std::nullopt;
    std::uint64_t keptBytes = 0, previousEnd = sliceBegin;
    for (const auto& [begin, end] : runs) {
        if (begin < previousEnd || begin < sliceBegin || begin >= end || end > sliceEnd) return std::nullopt;
        if ((begin - sliceBegin) % blockBytes != 0 || (end - sliceBegin) % blockBytes != 0) return std::nullopt;
        if (end - begin > std::numeric_limits<std::uint64_t>::max() - keptBytes) return std::nullopt;
        keptBytes += end - begin;
        previousEnd = end;
    }
    const auto spanBytes = runs.back().second - runs.front().first;
    const auto gapBytes = spanBytes - keptBytes;
    // Limit extra temporary storage/work: at most 1 MiB, and at most the kept bytes again.
    // Subtraction avoids overflow in a doubled kept-byte budget near the address-space end.
    if (gapBytes > (1u << 20u) || gapBytes > keptBytes) return std::nullopt;
    return std::pair{runs.front().first, runs.back().second};
}

}

#endif
