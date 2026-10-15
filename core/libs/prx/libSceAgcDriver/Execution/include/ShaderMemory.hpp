#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERMEMORY_HPP

#include "Recompiler.hpp"
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
struct SourceHandle;
class PreparedShaderInvocation;
}

namespace AgcDriver {

// Records the guest words a shader's resource analysis reads, as the memory regions its recompile
// request carries. Guest memory is fetched a 4 KiB page at a time; the regions report exactly the
// dwords read, so cache keys built from them do not change with unrelated bytes nearby. A page that
// `pendingWrite` reports as written by recorded GPU work is read word by word instead, so only a
// dword the capture needs waits for that work through the flush hook, not the whole page; a dword
// the query knows to be left unchanged by that work (Driver.cpp's written-element evidence) is read
// raw, without the hook. Debug aid: APS5_NO_WORDWISE_CAPTURE=1 fetches every page whole.
class ShaderMemory {
public:
    // The driver's answer for a range: nothing recorded writes it; recorded work writes it and the
    // read must go through the flush hook; recorded work writes it but leaves it unchanged, so the
    // bytes are read raw; VerifyRaw reads raw and through the hook, counting a difference;
    // KnownValue: recorded work writes it with bytes the driver already knows (a copy HLE's
    // destination, Driver.cpp's known-value ring entries), copied into `known` when the caller
    // passes a span of exactly `bytes` (empty: the answer alone); VerifyKnownValue serves them and
    // reads through the hook too, counting a difference. Volatile: recorded work writes it and the
    // driver has no evidence the bytes stay (or the write range is too large to hold any); it is a
    // Sync for a capture that does not speculate, and a word a speculating capture reads without
    // waiting (Speculate below). The driver answers it only with APS5_SPECULATIVE_CAPTURE set.
    enum class PendingWrite : std::uint8_t { None, Sync, Raw, VerifyRaw, KnownValue, VerifyKnownValue, Volatile };
    using PendingWriteQuery = PendingWrite (*)(std::uint64_t address, std::size_t bytes, std::span<std::byte> known);
    // Words served from known values by every capture, and the verified ones and their mismatches
    // (APS5_VERIFY_KNOWN_VALUES=1), for Driver.cpp's [copy] line.
    struct KnownValueCounts {
        std::uint64_t served, verified, mismatches;
    };
    static KnownValueCounts KnownValues();
    // Told, for a dword read that went through the hook, whether the bytes read before the hook's
    // wait were still there after it (the driver's evidence for later raw reads).
    using PendingWriteObserver = void (*)(std::uint64_t address, bool unchanged);
    // The calling thread's count of hook syncs that waited for unfinished GPU work: a read is only
    // observed when the count moved across it (against finished work both reads see the GPU's
    // bytes, and "unchanged" would be no evidence). Null observes every read that went through
    // the hook.
    using HookWaitCounter = std::uint64_t (*)();
    // The calling thread's GPU waits so far in milliseconds (Recorder::ThreadWaitedMs), set once
    // by the driver: the [capture] line charges the waits made inside a capture to it.
    using WaitedMsProvider = double (*)();
    static void SetWaitedMsProvider(WaitedMsProvider provider);
    explicit ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> initial, PendingWriteQuery pendingWrite = nullptr, PendingWriteObserver observe = nullptr, HookWaitCounter hookWaits = nullptr);
    // Returns what the capture resolved (plan, snapshot, specialization) for
    // ShaderRecompiler::Recompile(request, capture), which then skips its own materialization. One
    // result per call: the draw path captures several stages on one ShaderMemory. With `handle`
    // (the driver's memoized ShaderRecompiler::ResolveSource result) the capture skips the source
    // resolution.
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> Capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle = nullptr);
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> Capture(const ShaderRecompiler::PreparedShaderInvocation& invocation);
    [[nodiscard]] std::vector<ShaderRecompiler::MemoryRegion> Regions() const;
    // The page regions read since the previous call (or construction), a word read again
    // included, the initial regions excluded: one stage's own reads on the draw path's shared
    // ShaderMemory (Regions() stays the union). The spans point into the pages, as Regions()'s do.
    [[nodiscard]] std::vector<ShaderRecompiler::MemoryRegion> TakeRecentRegions();
    // The driver's source handle memo outcomes, for the [capture] line (APS5_PROFILE_DRAW).
    static void CountHandleMemo(bool hit);
    // Speculation (the draw path's GPU fill of flattened-SRT words, see Driver::materializeDrawStage):
    // while on, a word the query calls Volatile (recorded GPU work still writes it) is not read
    // through the flush hook; the read returns the bytes guest memory holds now, without waiting,
    // and its address is listed for TakeSpeculated. Such a word is never cached as read: it joins
    // no region, and a later read (speculating or not) asks again. Off by default; only the walk's
    // single-dword reads speculate, never the bulk reads of image tables.
    void Speculate(bool enabled) { speculating = enabled; }
    bool Speculating() const { return speculating; }
    // The addresses speculated since the previous call (or construction), in read order, a word
    // read twice listed twice; with `values`, the bytes each read returned.
    std::vector<std::uint64_t> TakeSpeculated(std::vector<std::uint32_t>* values = nullptr);
    // Words speculated by every capture since the previous call.
    static std::uint64_t TakeSpeculatedWords();
    // A speculating capture reads these addresses exactly (a first attempt found the walk consumed
    // them); Allow clears the list.
    void ForbidSpeculation(std::span<const std::uint64_t> addresses);
    void AllowSpeculation() { forbidden.clear(); }
    // Counts this memory's hook reads in TakeReadCounts (the draw path sets it): the reads that went
    // through the flush hook, by kind, and how many of them waited for unfinished GPU work.
    void CountReads(bool enabled) { counting = enabled; }
    enum class ReadKind : std::uint8_t { PageWhole, WordNone, WordSync, VolatileNotSpeculating, VolatileForbidden, Count };
    struct ReadCounts {
        std::array<std::uint64_t, static_cast<std::size_t>(ReadKind::Count)> reads{};
        std::array<std::uint64_t, static_cast<std::size_t>(ReadKind::Count)> waited{};
    };
    static ReadCounts TakeReadCounts();

private:
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle, const ShaderRecompiler::PreparedShaderInvocation* invocation);
    static constexpr std::size_t PageBytes = 4096;
    static constexpr std::size_t PageWords = PageBytes / sizeof(std::uint32_t);

    struct Page {
        std::array<std::uint32_t, PageWords> words{};
        std::bitset<PageWords> valid;
        std::bitset<PageWords> read;
        std::bitset<PageWords> recent;
        bool wordwise = false;
    };

    static bool read(void* context, std::uint64_t address, std::uint32_t* value);
    static bool readWords(void* context, std::uint64_t address, std::uint32_t* values, std::uint64_t count);
    static bool accessible(void* context, std::uint64_t address, std::uint64_t bytes);
    static bool pending(void* context, std::uint64_t address, std::uint64_t bytes);
    Page& page(std::uint64_t base);
    [[nodiscard]] bool overlapsInitial(std::uint64_t begin, std::uint64_t end) const;

    // Regions given at construction (the registered shader's code and header), referenced as given:
    // the caller keeps them alive for as long as the capture is used.
    std::map<std::uint64_t, std::span<const std::byte>> initial;
    std::map<std::uint64_t, Page> pages;
    PendingWriteQuery pendingWrite = nullptr;
    PendingWriteObserver observe = nullptr;
    HookWaitCounter hookWaits = nullptr;
    bool speculating = false;
    bool counting = false;
    std::vector<std::uint64_t> forbidden;
    std::vector<std::uint64_t> speculated;
    std::vector<std::uint32_t> speculatedValues;
};

// The GPU fills a speculative capture needs (Driver::materializeDrawStage): every speculated
// address must be read only as the leaf of pure flat-SRT slots (ShaderRecompiler::SrtReadTrace
// `leaves`, as (flat offset, address)): it is in none of the walk's other reads (`otherReads`,
// sorted) and was read no more often than leaves name it, so no read the trace did not see (a table
// snapshot, a specialization read) consumed it. Returns the (flat offset, address) of every
// such leaf, sorted by flat offset; nullopt when some speculated address fails (the capture must be
// repeated without speculation).
// The speculated addresses SpeculatedLeafSlots would refuse (sorted, distinct); `consumed` receives
// whether one of them is among the walk's other reads.
std::vector<std::uint64_t> SpeculationFailures(std::span<const std::uint64_t> speculated, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads, bool& consumed);
std::optional<std::vector<std::pair<std::uint32_t, std::uint64_t>>> SpeculatedLeafSlots(std::span<const std::uint64_t> speculated, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads);

// The positions, among a dispatch-cache variant's stored words, of the pure flat-SRT leaves a
// capture read (ShaderRecompiler::SrtReadTrace), for a hit that differs only there. `runs` are the
// variant's [begin, end) byte ranges in address order and `words` their dwords in that order;
// `leaves` are (flat offset, address) and `otherReads` the sorted addresses of every other read.
// A leaf is skipped when its address is among the other reads (a walk read aliases the dword,
// counted `aliased`), is not dword-aligned or lies in no run (counted `unmapped`), or its stored
// word differs from the flattened SRT's at that offset (counted `mismatched`). `positions`
// (sorted, may repeat) and `slots` (the flat offset of each) are parallel.
struct DataWordPositionCounts {
    std::uint64_t unmapped = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t aliased = 0;
};
DataWordPositionCounts DataWordPositions(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads, std::span<const std::uint32_t> words, std::span<const std::uint32_t> flattenedSrt, std::vector<std::uint32_t>& positions, std::vector<std::uint32_t>& slots);

}

#endif
