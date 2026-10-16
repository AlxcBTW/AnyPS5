#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BARRIERDEBT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BARRIERDEBT_HPP

#include "prx/libSceAgcDriver/Graphics/include/PassHazards.hpp"
#include <cstdint>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

// What a recorded dispatch touches, for the hazard-driven barriers (Recorder::CommandsForDispatch): the guest ranges it reads and
// writes through host imports and image mirrors, the storage images it binds with whether each is written, and its indirect
// arguments. `unknown` names the reason when the accesses cannot be listed (an address-based build, a BDA table, a fault buffer, a
// buffer copied for this use, workgroup memory in device memory): such a dispatch is recorded with its full barriers.
struct HazardFootprint {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
    std::vector<std::pair<VkImage, bool>> images;
    const char* unknown = nullptr;

    bool Known() const { return unknown == nullptr; }
    PassAccess Access() const { return PassAccess{reads, writes, images, {}, false, false}; }
};

// A barrier a command left unrecorded (see BarrierDebt).
struct OwedBarrier {
    VkPipelineStageFlags sourceStage = 0;
    VkPipelineStageFlags destinationStage = 0;
    VkAccessFlags sourceAccess = 0;
    VkAccessFlags destinationAccess = 0;
};

// Why a follower could not take over a debt.
enum class DebtVerdict : std::uint8_t {
    NotOwed,    // nothing is owed
    Keep,       // the follower touches nothing the owed run touches: the barrier stays owed
    Hazard,     // the follower overlaps the run in a way PassHazard names
    Uncovered,  // what ordered the run behind the commands before it does not reach the follower's accesses
};

// The trailing barrier of a run of mutually independent commands (dispatches in this version), not yet recorded. The recorder
// withholds the trailing barrier of a dispatch whose accesses it can list; the next command either joins the run (a dispatch
// whose accesses overlap nothing the run touched: no barrier between them, as none is owed between independent commands) or
// settles it (any other command, a dispatch that overlaps the run, the end of the batch): the owed barrier is recorded exactly as
// the dispatch would have recorded it, then the command. A barrier is a memory barrier whose second scope covers everything
// recorded later, so one barrier after the run orders every member before everything after it.
//
// `headCovered` is what the barrier before the run's first command (recorded, or the previous command's trailing one) made
// visible to every later shader access: the followers rely on it for the commands before the run, which their own leading
// barrier would otherwise cover. A run whose head does not cover `needed` takes no follower.
//
// Exactness rests on the footprints: a member whose accesses are not listed completely lets a conflicting follower join. The
// footprints are those the draw path already trusts for merging render passes (PassHazards), and a command whose accesses are
// not known never joins and never owes.
class BarrierDebt {
public:
    bool Active() const { return members != 0; }
    std::uint32_t Members() const { return members; }
    VkAccessFlags HeadCovered() const { return headCovered; }
    const OwedBarrier& Barrier() const { return owed; }
    // Whether a command with the access `follower` may be recorded behind the run without the owed barrier. `needed`: the access
    // bits the follower's leading barrier would have to make visible. `kind` receives the hazard on a Hazard verdict.
    DebtVerdict Decide(const PassAccess& follower, VkAccessFlags needed, PassHazard* kind = nullptr) const;
    // A command whose trailing barrier is owed starts a run (nothing may be owed before)...
    void Start(const PassAccess& access, const OwedBarrier& barrier, VkAccessFlags head);
    // ...or joins the open one after Decide said Keep.
    void Join(const PassAccess& access, const OwedBarrier& barrier);
    // The owed barrier (the union of the members'); the run is over.
    OwedBarrier Take();
    void Clear();

private:
    OwedBarrier owed;
    PassHazards footprint;
    VkAccessFlags headCovered = 0;
    std::uint32_t members = 0;
};

// The brute-force oracle behind the verify mode and the tests: whether the two accesses conflict (a write against a read or a
// write of overlapping guest ranges, a written image against any use of it), by comparing every pair, independent of the
// interval sets BarrierDebt keeps.
bool AccessesConflict(const PassAccess& earlier, const PassAccess& later);

}

#endif
