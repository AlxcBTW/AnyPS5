#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTBUFFERMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTBUFFERMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "BdaAbi.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

void ShutdownGuestBufferWorkers();

struct GuestMemorySnapshot {
    std::uint64_t address;
    std::span<const std::byte> bytes;
};

// A guest allocation imported with VK_EXT_external_memory_host, usable by the GPU in place.
struct HostImport {
    std::uint64_t base;
    std::uint64_t bytes;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress address;
    void* alias = nullptr;
    std::shared_ptr<const GuestAllocations::Range> range {};
    std::vector<std::shared_ptr<const GuestAllocations::Range>> ranges {};
    // Identity for the life of this import (see HostImportSerial); 0 until first asked for.
    std::uint64_t serial = 0;
    bool unwatched = false;
    bool dmaBuf = false;
    bool joined = false;
    std::shared_ptr<void> chunk;
    mutable std::uint64_t lastUse = 0;
};

enum class ImportWatch : std::uint8_t { Watch, Unwatch };

struct ImportProbe {
    const char* failure = nullptr;
    VkResult result = VK_SUCCESS;
    std::uint32_t pages = 0;
    std::uint32_t writtenAtImport = 0;
    std::uint32_t writtenAfterSubmit = 0;
    std::uint32_t writtenAfterSecondSubmit = 0;
    std::uint32_t writtenDuringSettle = 0;
    std::uint32_t writtenAfterSettle = 0;
    std::uint32_t writtenByCpu = 0;
    std::uint32_t writtenAfterFill = 0;
};

ImportProbe ProbeImportWriteProtection(const Context& context);
ImportProbe ProbeDmaBufImportWriteProtection(const Context& context);
ImportWatch PrepareImportWatch(const Context& context);
void SetImportWatch(const Context& context, ImportWatch watch);

// The host import of the registered allocation containing [address, address + bytes), made on demand
// (alignment and budget permitting), or null. Bytes at `address` are at `address - import->base` in
// the import's buffer.
const HostImport* HostImportFor(const Context& context, std::uint64_t address, std::size_t bytes);
// The existing import serving [address, address + bytes) while the registry has not changed since
// the imports were reconciled, or null: makes none and reconciles none (the shadow copy-back's
// check that its region's import still serves it).
const HostImport* HostImportServing(const Context& context, std::uint64_t address, std::size_t bytes);
void SetHostImportWindow(std::uint64_t bytes);
// Tests: the window of one large registered range (64 MiB) and the import budget (0: the configured one).
void SetHostImportRangeWindow(std::uint64_t bytes);
void SetHostImportBudget(std::uint64_t bytes);
std::uint64_t HostImportLiveBytes();
bool ImportMappedRanges(const Context& context, const GuestAllocations::Mapped& ranges, std::uint64_t generation, bool adoptDevice);
// Whether an existing import covers [address, address + bytes), without reconciling the imports
// with the registry or making one (HostImportFor may take a registry lease): a hint for choices
// made outside the device lock (a sampled texture's path, a dispatch's pre-sync); the path taken
// re-checks with HostImportFor when it binds the import.
bool HostImportCovers(const Context& context, std::uint64_t address, std::size_t bytes);
struct RegisteredPiece {
    std::uint64_t address;
    std::uint64_t bytes;
    bool writable;
    bool gpu;
};
// Whether a readable registered allocation contains [address, address + bytes) right now (one
// registry lease): a storage image whose memory was freed or re-registered has nothing to store to.
bool RegisteredReadableCovers(std::uint64_t address, std::size_t bytes, std::vector<RegisteredPiece>* pieces = nullptr);
std::uint64_t RegisteredReadableEnd(std::uint64_t address);

// A persistent device copy of one registered range of the main guest image (which cannot be host
// imported); see GuestBufferMemory.cpp.
struct ImageMirror;
class Recorder;

template<typename TRange>
void OverlappedRanges(std::span<const TRange> sorted, std::size_t first, std::span<const std::pair<std::uint64_t, std::uint64_t>> writes, std::vector<std::size_t>& indices) {
    indices.clear();
    for (const auto& [begin, end] : writes) {
        auto it = std::partition_point(sorted.begin() + static_cast<std::ptrdiff_t>(first), sorted.end(), [&](const TRange& range) { return range.end <= begin; });
        for (; it != sorted.end() && it->begin < end; ++it) indices.push_back(static_cast<std::size_t>(it - sorted.begin()));
    }
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
}

// Identity of the import serving [address, address + bytes): a serial unique for the life of one
// import, 0 when no import serves the range. An equal serial later means the same VkBuffer still
// backs the range (a re-import gets a new serial), which is what a descriptor set written against it
// needs to stay valid (see the ShaderResources cache). With `reconcile`, imports whose registered
// range changed are dropped first, as an upload does; without it (right after an upload, before the
// work using the import is recorded) the imports are left as they are.
std::uint64_t HostImportSerial(const Context& context, std::uint64_t address, std::size_t bytes, bool reconcile);

// The import table's identity (the fast Revalidate): serials proved under one identity stand
// while the table still has it, i.e. the same device, the same epoch (bumped by every retire)
// and the registry generation it was reconciled with, which must still be the live one.
struct HostImportsProof {
    VkDevice device = VK_NULL_HANDLE;
    std::uint64_t epoch = 0;
    std::uint64_t refreshedGeneration = 0;
};
// Whether the table still matches `proof` (one lock); false for an empty or stale proof.
bool HostImportsUnchanged(const Context& context, const HostImportsProof& proof);
// The table's identity when it is reconciled with the live registry (one lock), else empty.
HostImportsProof HostImportsIdentity(const Context& context);

// Identity of the read-only image mirror serving [address, address + bytes) (the counterpart of
// HostImportSerial for main-image ranges, which no import serves): a serial unique for the life of
// one mirror with its top bit set, 0 when no read-only mirror whose registered range still exists
// covers the range. A read-only mirror's bytes and VkBuffer never change, so a descriptor set written
// against it stays valid while the serial is unchanged.
std::uint64_t ImageMirrorSerial(const Context& context, std::uint64_t address, std::size_t bytes);

// Deferred lease release (see GuestBufferMemory.cpp). An address-based build leases every readable
// registered allocation until its write-back, which runs when its batch completed; the guest's
// unmap/mprotect/free of a leased allocation waits for that through the registry's pin waiter, which
// submits the recorder and finishes its batches up to the newest one recorded with a lease. SyncLeaseWork
// says whether the build's work must instead be synced as soon as it is recorded
// (APS5_SYNC_LEASE_DISPATCH=1, the behaviour before deferral). The driver and the draw path count
// their choice with CountLeaseOutcome right after the lease-holding resources were kept by the open
// batch: `batchSerial` is that batch's serial (Recorder::Submissions() + 1 under the device lock), 0
// when synced, and the waiter targets the newest of them. Under APS5_PROFILE_DRAW CountLeaseOutcome
// prints the [address-sync] leases line every 10 s from LeaseCounters (cumulative).
bool SyncLeaseWork();
void CountLeaseOutcome(bool synced, std::uint64_t batchSerial);
struct LeaseStats {
    std::uint64_t deferred = 0;
    std::uint64_t synced = 0;
    // Pin-contention waits made by guest threads (the waiter calls), how many of them finished the
    // newest lease batch, how many had to drain the whole recorder instead (no lease batch noted),
    // how many only dropped the cached address space (no GPU wait, see AddressSpaceStats), and
    // their total time.
    std::uint64_t contentionWaits = 0;
    std::uint64_t contentionSyncs = 0;
    std::uint64_t contentionDrains = 0;
    std::uint64_t cacheDrops = 0;
    double contentionMs = 0;
};
LeaseStats LeaseCounters();

// The cached address space of address-based builds (see GuestBufferMemory.cpp): builds served by
// the cached space, rebuilds by reason, builds whose space could not be published (an import
// retired while it was built) and the pin waiter's drops (cumulative). `enabled` is false under
// APS5_NO_ADDRESS_SPACE_CACHE=1, when every build takes the per-build path.
struct AddressSpaceStats {
    bool enabled = true;
    std::uint64_t hits = 0;
    std::uint64_t rebuiltFirst = 0;
    std::uint64_t rebuiltGeneration = 0;
    std::uint64_t rebuiltEpoch = 0;
    std::uint64_t rebuiltDevice = 0;
    std::uint64_t rebuiltWaiterDrop = 0;
    std::uint64_t unpublished = 0;
    // Builds that left the space for the per-build path: a region partially overlapping one of
    // its regions, or an import retired under it before the upload.
    std::uint64_t dissolvedOverlap = 0;
    std::uint64_t dissolvedImports = 0;
    std::uint64_t waiterDrops = 0;
};
AddressSpaceStats AddressSpaceCounters();

struct MirrorStats {
    std::uint64_t heapMirrors = 0;
    std::uint64_t heapBytes = 0;
    std::uint64_t rebuilds = 0;
    std::uint64_t blocksCopied = 0;
    std::uint64_t heapRefills = 0;
    std::uint64_t sweeps = 0;
    std::uint64_t heapChecks = 0;
    std::uint64_t sparseBinds = 0;
};
MirrorStats MirrorCounters();
void ClearSnapshotCopies(VkDevice device);
std::vector<std::uint64_t> AddressTableKey(std::span<const std::pair<std::uint64_t, std::uint64_t>> writes, std::span<const ShaderRecompiler::BdaAbi::Range> own);
void ClearImageMirrors(VkDevice device);
void ClearHostImports(VkDevice device);

struct AddressCopy {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t committed;
    const char* reason;
};
std::string AddressCopyOverflow(std::vector<AddressCopy> copies, std::uint64_t limit);

// Lazy write-back (shadow-sourced staging, see GuestBufferMemory.cpp): on unless
// APS5_YOLO_GPU=1 and APS5_NO_LAZY_WRITEBACK unset, and only while unit shadows are on.
bool LazyWritebackEnabled();
// Whether a region of `regionBytes` whose copy-in would read `shadowBytes` from unit-shadow slabs
// is staged from the shadow instead of bound in place: at most `maxBytes`, at least one unit from
// the shadow, and at least half of it, so the import part the copy-in reads over the bus never
// exceeds the publish it replaces.
bool ShadowStageAccepted(std::uint64_t regionBytes, std::uint64_t shadowBytes, std::uint64_t maxBytes);
// Shadow copy-back (see GuestBufferMemory.cpp, "shadow copy-back"): on unless
// APS5_NO_SHADOW_COPYBACK=1, and only with lazy write-back on.
bool ShadowCopyBackEnabled();
// The dispatch program whose resources the calling thread builds or revalidates (0 for draws),
// for the APS5_TRACE_REGION_PUBLISH lines only.
void SetTraceProgram(std::uint64_t programAddress);

class GuestBufferMemory {
public:
    explicit GuestBufferMemory(const Context& context);
    ~GuestBufferMemory();
    void AcquireRegistered();
    // APS5_PROFILE_DRAW: the parts of one address-based build ([address] line every 10 s): the
    // registry lease, the imports pass, the mirror preparation and the mirror compare (blocks and
    // copied blocks) are timed by AcquireRegistered on the calling thread; the caller adds the
    // snapshot compares it made after it (ShaderResources::prepareAddressBindings).
    static void CountAddressBuild(double snapshotsUs);
    // A guest range bound through a descriptor. Both read live guest memory at upload and bind the
    // same way (a storage buffer); AddWritable also notes the range in Writes(), so it gets the
    // write-back's reference copy, a write-back, the recorder's pending-write note and the
    // direct-write marks. AddReadable is for an element the shader is proved never to store to
    // (DescriptorBinding::bufferWritten): the CPU never has to wait for it. `atomic` marks an
    // element the shader updates atomically (DescriptorBinding::bufferAtomic), staged in device
    // memory whatever its size (see AllowDeviceStaging).
    void AddWritable(std::uint64_t address, std::size_t bytes, bool atomic = false, bool swept = false);
    void AddReadable(std::uint64_t address, std::size_t bytes);
    // Device-local staging of written and atomic elements inside host imports (see
    // GuestBufferMemory.cpp): allowed only for a build whose every use records its work and then
    // calls RecordCopyBacks (a dispatch), since a staged region's results reach guest memory by
    // that copy alone. Call before Upload.
    void AllowDeviceStaging() { stagingAllowed = true; }
    void AllowAdjustedRegions() { adjustedRegions = true; }
    // Records the copy-in of every staged region anew for another use of this upload (a resource
    // cache hit, from ShaderResources::Revalidate, under GuestMemory::GpuMutex, once the imports
    // were confirmed unchanged): the previous use's copy-back left the shadow behind, and the next
    // RecordCopyBacks copies it back again. Nothing to do without staged regions.
    void RecordStagingCopies(Recorder& recorder);
    void AddSnapshot(const GuestMemorySnapshot& snapshot);
    // Upload is the two stages below back to back. UploadPrepare needs no device lock: it merges the
    // regions, binds the image mirrors and host imports that already serve them (an import pointer is
    // re-checked against the registry epoch later) and copies read-only regions. UploadFinish runs
    // under GuestMemory::GpuMutex: it reconciles and makes imports (retiring one hands its buffer to
    // the recorder), refreshes writable mirrors and flushes pending results (both may wait for
    // recorded work) and copies the regions a descriptor may write, so the window between that copy
    // and the recorder's pending-write note stays closed.
    void Upload(bool addressable);
    void UploadPrepare(bool addressable);
    void UploadFinish(bool addressable);
    VkDescriptorBufferInfo Descriptor(std::uint64_t address, std::size_t bytes, std::uint32_t& adjustment) const;
    static std::uint64_t ViewBytes(std::uint64_t bytes, std::uint32_t adjustment);
    std::vector<ShaderRecompiler::BdaAbi::Range> AddressRanges() const;
    // The BDA table of the cached address space when it serves this upload alone (an address-based
    // build with no region outside it): its ranges, immutable while the space lives, and the
    // space's serial, by which BdaResources reuses the table buffer built for it without a
    // compare. Nothing when a region of this build is not in the space (AddressRanges then merges).
    struct CachedTable {
        std::uint64_t serial;
        const std::vector<ShaderRecompiler::BdaAbi::Range>* ranges;
    };
    std::optional<CachedTable> CachedAddressTable() const;
    // The registered memory an address-based build maps, shared by consecutive builds (see
    // GuestBufferMemory.cpp): its lease and the sorted, immutable regions of the ranges served in
    // place by imports and mirrors. Opaque outside GuestBufferMemory.cpp.
    struct AddressSpace;
    void WriteBack();
    bool WritesOverlap(std::uint64_t address, std::size_t bytes) const;
    bool CopiedWritesOverlap(std::uint64_t address, std::size_t bytes) const;
    // Guest ranges the shader may write through descriptors, as [begin, end).
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& Writes() const { return writes; }
    // Whether any written range lives in a copied buffer, so a write-back must run once the GPU is done.
    bool HasCopiedWrites() const;
    // Reports writes into host-imported memory (made by the GPU in place, or copied back into it by
    // RecordCopyBacks) to the write tracking now.
    void MarkDirectWrites() const;
    // Records, into the recorder's open batch, the copy of every written sub-range of a region the
    // GPU copied out of a host import (see Region::gpuCopy) back into the import: what the shader
    // wrote lands in guest memory by the GPU, ordered after the recorded work, so the region needs
    // no CPU write-back (HasCopiedWrites no longer counts it) and the caller notes the ranges as
    // pending writes and marks them as MarkDirectWrites does. Called right after the work using the
    // regions was recorded (ShaderResources::MarkGpuWrites), under GuestMemory::GpuMutex. A use that
    // never calls it (a synchronous draw) stores the staging bytes from the CPU in WriteBack.
    void RecordCopyBacks(Recorder& recorder);
    // Whether registered allocations are pinned until write-back (address-based shaders): by this
    // build's own lease, or by the cached address space it holds.
    bool HoldsLease() const { return !lease.empty() || space != nullptr; }
    std::size_t CopiedBytes() const {
        std::size_t bytes = 0;
        for (const auto& region : regions) if (region.buffer != nullptr) bytes += static_cast<std::size_t>((region.bufferEnd != 0 ? region.bufferEnd : region.end) - region.begin);
        return bytes;
    }
    // Every uploaded region as [begin, end) when all of them are served by host imports, in place or
    // through a device-local staging copy of the import (nothing was copied through the CPU, so the
    // upload can serve a later identical build), else nothing.
    std::optional<std::vector<std::pair<std::uint64_t, std::uint64_t>>> DirectRegions() const;
    // Lazy write-back (see GuestBufferMemory.cpp, "shadow-sourced staging"; APS5_NO_LAZY_WRITEBACK=1
    // turns it off): whether [begin, end) lies inside a region staged from the unit shadow. Such a
    // region's flush stores the pending images into the shadow without publishing them into the
    // import (PublishScope::None), and its copy-in reads the fresh units from their slab.
    bool ShadowSourced(std::uint64_t begin, std::uint64_t end) const;
    // Publishes the units that a written range inside a shadow-sourced region covers partly: its
    // copy-back stamps them, so the rest of each such unit must be in the import first. Called after
    // the region's flush and before its copy-in, under GuestMemory::GpuMutex.
    void PublishShadowSourcedEdges() const;
    // [lazywb] census of a region bound in place whose flush published the unit shadow into its
    // import (PublishScope::Whole): units copied, by whether a descriptor writes the region.
    static void NoteInPlacePublish(bool written, std::uint64_t regionBytes, std::uint64_t units, bool reused = false, bool draw = false, std::uint64_t begin = 0);
    bool StagingAllowed() const { return stagingAllowed; }
    // Shadow copy-back, reuse of a cached build (ShaderResources::Revalidate, under
    // GuestMemory::GpuMutex, before the region flushes): whether the build's decision for
    // [begin, end) went stale so that the build is to be made anew. A region bound in place whose
    // written range now lies over enough fresh unit-shadow units to be staged (the pending images
    // are stored into the shadow first, without a publish), or a region staged from the shadow whose
    // copy-in would now read more than half of it from the import. Bounded per region (flips).
    bool ShadowDecisionStale(std::uint64_t begin, std::uint64_t end);
    // The same check for every region staged from the shadow (they are not among the in-place
    // regions the caller walks).
    bool ShadowStagedStale();
    struct CutRegion {
        std::uint64_t begin;
        std::uint64_t end;
        std::uint64_t committedEnd;
    };
    std::vector<CutRegion> CutRegions() const;
    // Every region the recorded work reads in place through a host import, as [begin, end): the
    // regions bound in place (`direct`), an address-based build's leased heaps included. A region
    // the GPU copies out of an import (gpuCopy) notes its read itself when the copy is recorded.
    // For the recorder's read tracking (ShaderResources::MarkGpuWrites); nothing once committed.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> InPlaceReads() const;
    struct SplitReads {
        std::shared_ptr<const std::vector<std::pair<std::uint64_t, std::uint64_t>>> space;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> own;
    };
    std::optional<SplitReads> SplitInPlaceReads() const;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> DeviceReads() const;

private:
    struct Region {
        std::uint64_t begin;
        std::uint64_t end;
        bool writable;
        std::vector<std::byte> snapshot;
        // Shared so a recorded GPU copy into it (see gpuCopy) can keep it in the batch itself, before
        // the caller keeps the whole resources: a throw between the two would otherwise return it to
        // the pool under an unsubmitted copy command.
        std::shared_ptr<Buffer> buffer;
        // Guest bytes as uploaded; write-back only stores bytes the GPU changed.
        std::vector<std::byte> uploaded {};
        // Registered allocation that is imported: its bytes are read from live guest memory, not a snapshot.
        bool hostBacked = false;
        // Set when the region is served by an imported allocation; nothing is copied or written back.
        const HostImport* direct = nullptr;
        // Set when the region covers uncommitted pages: only the `backed` parts (possibly none) are guest
        // memory; the rest reads as zeros and is never stored.
        bool sparse = false;
        bool sparseHeap = false;
        bool runCut = false;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> backed {};
        std::uint64_t lastView = 0;
        std::uint64_t bufferEnd = 0;
        // Set when the region is served by an image mirror: nothing is copied; writable mirrors are
        // written back by comparing with the mirror's shadow. Kept alive here for recorded work.
        std::shared_ptr<ImageMirror> mirror {};
        // The mirror is a descriptor sub-range one found by UploadPrepare (not a lease mirror), which
        // UploadFinish confirms still has its Range before the region binds it.
        bool subrangeMirror = false;
        // Set by UploadPrepare when UploadFinish still has device-lock work for the region: a copy a
        // descriptor writes, a writable mirror's refresh, an import to reconcile or make, or the
        // pending-results flush of an import it took.
        bool pending = false;
        // The region lies inside a host import but is misaligned for binding in place: `buffer` is
        // filled by a vkCmdCopyBuffer from the import recorded into the batch (no CPU read, so no
        // flush-hook wait), from `copySource` (the import's VkBuffer, alive until the batch
        // completed, with the guest address of its first byte). `copiedBack` once RecordCopyBacks
        // recorded the written sub-ranges' copies back into the import.
        bool gpuCopy = false;
        VkBuffer copySource = VK_NULL_HANDLE;
        std::uint64_t copySourceBase = 0;
        // The import's size, with copySource and copySourceBase its identity for the unit shadow
        // (UnitShadow's registry keys a shadow by all three).
        std::uint64_t copySourceBytes = 0;
        bool copiedBack = false;
        // A device-local staging region whose copy-in reads the import's fresh unit-shadow units
        // from their slab and the rest from the import; nothing over it was published for its use
        // (see "shadow-sourced staging" in GuestBufferMemory.cpp).
        bool shadowSourced = false;
        // A written range inside was flushed without a publish (UploadFinish): every path the
        // region takes afterwards publishes before reading, or sources the copy from the shadow.
        bool deferredPublish = false;
        // Bytes this region holds of the shadow-staging budget (stageFromShadow), returned by the
        // destructor.
        std::uint64_t budgetBytes = 0;
        // An element the shader updates atomically lies inside (AddWritable's `atomic`).
        bool atomic = false;
        bool swept = false;
        // The gpuCopy buffer is a device-local staging shadow (see stagingEligible): no host
        // mapping, so nothing is ever stored from it by the CPU, and the region is taken even when
        // the import could bind it in place.
        bool deviceLocal = false;
        // The device refused the shadow (memory or allocations exhausted): the region takes the
        // path it would take without staging, in both upload stages.
        bool unstaged = false;
        // Staged from the unit shadow instead of bound in place (stageFromShadow accepted it): with
        // shadow copy-back its written ranges go back into the shadow's slabs, not the import.
        bool shadowStaged = false;
        // The last copy-back of this region went into the shadow (RecordCopyBacks stamped and
        // marked its ranges itself): MarkDirectWrites leaves them alone.
        bool shadowCopiedBack = false;
        // stageFromShadow refused the region for want of device memory or budget: a revalidation
        // does not ask for a rebuild to stage it again.
        bool shadowRefused = false;
    };
    // The written ranges of a shadow-staged region back into its import's unit shadow (slab pieces,
    // seeded where a piece covers a unit partly), the rest into the import; stamps and marks them.
    // Returns the bytes that went into slabs.
    std::uint64_t copyBackIntoShadow(Recorder& recorder, Region& region, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& merged, VkCommandBuffer commands);
    // The bytes of [region.begin, region.end) a shadow-sourced copy-in would read from slabs now.
    std::uint64_t freshBytes(const Region& region) const;
    bool restageCandidate(const Region& region) const;
    // UploadFinish's `addressable`, for the revalidation's decision.
    bool builtAddressable = false;

    // How [begin, end) lies against the space's base regions.
    enum class BaseOverlap { None, Inside, Partial };
    BaseOverlap baseOverlap(std::uint64_t begin, std::uint64_t end, const Region** owner) const;
    // Copies the space's base regions into this build's own regions (today's per-build form) and
    // drops the space: for a region that partially overlaps a base region, and for a build whose
    // imports changed under the space (`resolve`: the direct regions are re-resolved by UploadFinish).
    void dissolveSpace(bool resolve);
    void completeSpaceWrites() const;
    // The region containing `address`: this build's own regions (sorted after UploadPrepare) or
    // the space's base regions, or null.
    const Region* owner(std::uint64_t address) const;
    // Builds this build's regions from a lease as the per-build path always did: `base` gets the
    // ranges served in place (imports, mirrors), `copied` the ones copied per build.
    struct CopiedRange {
        std::uint64_t begin;
        std::uint64_t end;
        bool writable;
    };
    void addCopiedRange(const CopiedRange& range);
    // The BDA table entry of an uploaded region.
    static ShaderRecompiler::BdaAbi::Range addressRange(const Region& region);
    static std::uint64_t servedEnd(const Region& region);
    static std::uint64_t inPlaceEnd(const Region& region);
    void validate(std::uint64_t address, std::size_t bytes) const;
    // The region of a descriptor-bound range (AddWritable/AddReadable), committed pages only. A
    // range inside a base region of the space adds nothing: the region serves it, as today's merge
    // of the two did.
    void addDescriptorRegion(std::uint64_t address, std::size_t bytes, bool atomic, bool swept, bool readOnly);
    static bool servesView(const Region& region, std::uint64_t view);
    static bool sparseHeapCandidate(const Region& region);
    bool bindSparseMirror(Region& region, const GuestAllocations::Lease& lease);
    // Gives a region a buffer of its own with its bytes (guest memory for host-backed and writable
    // ranges, plus the write-back's reference copy for a range a descriptor writes; else its snapshot).
    void copyRegion(Region& region, bool addressable);
    // Whether a region inside a host import that cannot be bound in place is copied by the GPU
    // instead of the CPU (see Region::gpuCopy): live guest bytes, not sparse, at most the size
    // APS5_GPU_COPY_MAX_KIB allows, and APS5_CPU_COPIES unset.
    bool gpuCopyEligible(const Region& region) const;
    // Whether a region inside a host import is staged in device memory instead of bound in place
    // (see GuestBufferMemory.cpp): staging allowed, not address-based, a written element inside,
    // and an atomic element or a size within the written-shadow window. Independent of the import,
    // so UploadPrepare and UploadFinish decide alike.
    bool stagingEligible(const Region& region, bool addressable) const;
    // Lazy write-back: whether the region may be staged from the unit shadow (a staging-eligible
    // region, or with APS5_NO_SHADOW_STAGE unset any region of a dispatch build up to
    // APS5_SHADOW_STAGE_MAX_KIB), independent of the import and of the shadow's state.
    bool shadowStagingCandidate(const Region& region, bool addressable) const;
    // A region UploadFinish took in place (`entry` its import), after its flush stored the pending
    // images without a publish: staged from the shadow when ShadowStageAccepted holds for the fresh
    // bytes the copy-in would read from slabs, and a device-local buffer is to be had.
    bool stageFromShadow(Region& region, bool addressable);
    void publishEdges(const Region& region) const;
    bool bindableInPlace(std::uint64_t offset, bool addressable) const;
    // Records the import-to-buffer copies of the given gpuCopy regions into the open batch, with
    // the barriers that order them after earlier recorded writes and before the shaders reading them.
    void recordGpuCopies(std::span<Region* const> copies, bool addressable);
    void takeHeapReferences();
    Context context;
    bool stagingAllowed = false;
    bool adjustedRegions = false;
    GuestAllocations::Lease lease;
    // The cached address space this build maps through (its lease pins the ranges); `regions` then
    // holds only the regions outside it (V#s, snapshots, ranges copied per build).
    std::shared_ptr<const AddressSpace> space;
    mutable std::uint64_t tableSerial = 0;
    mutable std::shared_ptr<const std::vector<ShaderRecompiler::BdaAbi::Range>> tableRanges;
    // Import registry epoch when `direct` pointers were taken at acquire time; they are reused while
    // no import was destroyed since.
    std::uint64_t importsEpoch = 0;
    std::uint64_t importFloor = 0;
    std::vector<Region> regions;
    // Whether `regions` is in ascending address order (true right after AcquireRegistered, whose
    // regions follow the registry's order), so AddSnapshot can search instead of scanning.
    bool regionsSorted = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> readsOverBase;
    std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> heapReferences;
    // UploadPrepare ran (regions are frozen); `uploaded` once UploadFinish ran.
    bool prepared = false;
    bool uploaded = false;
    bool committed = false;
};

}

#endif
