#include "prx/libSceAgcDriver/Graphics/include/BarrierDebt.hpp"

namespace AgcDriver::Graphics {

DebtVerdict BarrierDebt::Decide(const PassAccess& follower, VkAccessFlags needed, PassHazard* kind) const {
    if (members == 0) return DebtVerdict::NotOwed;
    if ((headCovered & needed) != needed) return DebtVerdict::Uncovered;
    const auto hazard = footprint.Check(follower, false);
    if (kind != nullptr) *kind = hazard;
    return hazard == PassHazard::None ? DebtVerdict::Keep : DebtVerdict::Hazard;
}

void BarrierDebt::Start(const PassAccess& access, const OwedBarrier& barrier, VkAccessFlags head) {
    Clear();
    footprint.Add(access);
    owed = barrier;
    headCovered = head;
    members = 1;
}

void BarrierDebt::Join(const PassAccess& access, const OwedBarrier& barrier) {
    footprint.Add(access);
    owed.sourceStage |= barrier.sourceStage;
    owed.destinationStage |= barrier.destinationStage;
    owed.sourceAccess |= barrier.sourceAccess;
    owed.destinationAccess |= barrier.destinationAccess;
    ++members;
}

OwedBarrier BarrierDebt::Take() {
    const auto barrier = owed;
    Clear();
    return barrier;
}

void BarrierDebt::Clear() {
    owed = {};
    footprint.Clear();
    headCovered = 0;
    members = 0;
}

namespace {

bool overlap(const std::pair<std::uint64_t, std::uint64_t>& a, const std::pair<std::uint64_t, std::uint64_t>& b) {
    return a.first < a.second && b.first < b.second && a.first < b.second && b.first < a.second;
}

bool anyOverlap(GuestRangeList a, GuestRangeList b) {
    for (const auto& x : a) {
        for (const auto& y : b) {
            if (overlap(x, y)) return true;
        }
    }
    return false;
}

}

bool AccessesConflict(const PassAccess& earlier, const PassAccess& later) {
    // An access whose ranges are unknown conflicts with every other that touches memory.
    const auto touches = [](const PassAccess& use) {
        for (const auto& range : use.reads) if (range.first < range.second) return true;
        for (const auto& range : use.writes) if (range.first < range.second) return true;
        return use.anyReads || use.anyWrites;
    };
    if ((later.anyWrites && touches(earlier)) || (earlier.anyWrites && touches(later))) return true;
    const auto writes = [](const PassAccess& use) {
        for (const auto& range : use.writes) if (range.first < range.second) return true;
        return false;
    };
    if ((later.anyReads && writes(earlier)) || (earlier.anyReads && writes(later))) return true;
    if (anyOverlap(earlier.writes, later.reads) || anyOverlap(earlier.writes, later.writes) || anyOverlap(earlier.reads, later.writes)) return true;
    for (const auto& [image, written] : earlier.images) {
        if (image == VK_NULL_HANDLE) continue;
        for (const auto& [other, otherWritten] : later.images) {
            if (other == image && (written || otherWritten)) return true;
        }
    }
    return false;
}

}
