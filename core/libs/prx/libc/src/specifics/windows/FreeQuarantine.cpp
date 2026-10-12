#ifdef _WIN32
#include <windows.h>
#include "prx/libc/include/FreeQuarantine.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Crash hunt (exp, never push, title-specific): APS5_EXP_FREE_QUARANTINE=<seconds>[,<MiB cap>] delays Wwise's
// frees in Sword of the Sea (PPSA25344). Wwise's software Vorbis decoder keeps writing synthesis output into its
// buffers after they were freed (grow: 0.2 ms after, m3-crash-sr4b; teardown: 17 s after in sr3, 153 s after in
// m3-crash-fix1, and all through a 300 s hold and on in m3-crash-q1), and once rpmalloc hands a block to another
// object the floats corrupt it or a free-list link (crash at app+0x52442f3). Every AK::MemoryMgr::Free goes
// through one stub, app+0x5244e60 (mov rax,[app+0x6d0f0a0]; jmp rax; 1995 calls and 53 tail jumps; nothing else
// reads the slot). The stub is rewritten at libc init, before guest code runs, to jump here.
// A freed block waits in a FIFO and is hashed (its first 64 KiB). It comes up for release once it is <seconds>
// old, or earlier, oldest first, while the held bytes exceed the cap (default 512 MiB; 0 = none) or Wwise's own
// mapping limit is near. Then it is hashed again: a block that changed while held got writes after its free and
// is kept for good (never freed, "written after its free"); so is every block freed by the decoder's buffer frees
// (APS5_EXP_FREE_QUARANTINE_KEEP: return addresses of the Free calls, default the SotS decoder sites below,
// "none" for no site). Everything else goes to Wwise's own free (the function in the slot, read at each call).
// The real free runs on the guest thread that calls Free next, outside the FIFO's lock, never on a host thread.
// Blocks of 1 MiB or more are freed at once, so one big free cannot flush the small ones early.
// [free-quarantine] lines every 10 s give the held count, bytes and ages, what was released and why, and what was
// kept. APS5_EXP_FREE_QUARANTINE_TRACE=<addr>[:<addr>...] (up to 4; default: the absolute addresses of
// APS5_EXP_WATCH_WRITE) logs when a block covering one of them is held, released or kept.
// Off (variable unset): nothing is patched and no thread starts.

namespace {

using FreeQuarantine::Entry;
using FreeQuarantine::Release;
using FreeQuarantine::Taken;

constexpr std::uint64_t FreeStubRva = 0x5244e60;  // 48 8b 05 <rel32 -> FreeSlotRva> ff e0, then int3 x7
constexpr std::uint64_t FreeSlotRva = 0x6d0f0a0;  // AK memory settings copy +0x38 (pfFree): app+0x5240cf0 after
                                                  // MemoryMgr::Init (app+0x5240310), 0 after Term (app+0x5244dd0)
constexpr std::uint64_t SizeSlotRva = 0x6d0f0b0;  // +0x48 (pfSizeOfMemory): app+0x5240dc0, rpmalloc usable size
constexpr std::uint64_t MappedRva = 0x6d0f060;    // bytes Wwise's rpmalloc has mapped
constexpr std::uint64_t LimitRva = 0x6d0f058;     // the most it may map (0 = no limit); a span beyond it fails
constexpr std::uint64_t BigBlockBytes = 1ull << 20;
constexpr std::uint64_t CheckBytes = 64 * 1024;
constexpr std::size_t ReleaseBatch = 8;           // releases per Free call (small: guest stacks)
constexpr unsigned TraceSlots = 4;
constexpr unsigned KeptLines = 32;
constexpr unsigned KeepSiteSlots = 16;
// Return addresses of the software Vorbis decoder's buffer frees (pool 5): 0x52de480 (the grow path, 0x52de6d5
// in m3-crash-sr4b) at 0x52de553 / 0x52de679 / 0x52de6da, its caller at 0x52de899, the channel buffers of
// 0x52de9f0 at 0x52dea20 (m3-crash-fix1) / 0x52dea39, and the teardown 0x52e9f30 at 0x52e9f5f (m3-crash-q1) /
// 0x52e9f90 / 0x52e9fb6.
constexpr std::uint64_t DefaultKeepSites[] = {0x52de553, 0x52de679, 0x52de6da, 0x52de899, 0x52dea20, 0x52dea39, 0x52e9f5f, 0x52e9f90, 0x52e9fb6};

using GuestFree = void(APS5_VABI*)(std::uint64_t poolId, void* pointer);
using GuestSize = std::uint64_t(APS5_VABI*)(std::uint64_t poolId, void* pointer);

void Report(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length <= 0) return;
    if (length > static_cast<int>(sizeof(buffer)) - 1) length = sizeof(buffer) - 1;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buffer, static_cast<DWORD>(length), &written, nullptr);
}

struct Stats {
    std::uint64_t frees = 0;          // Free calls with a block, held or not
    std::uint64_t held = 0;
    std::uint64_t heldBytes = 0;
    std::uint64_t released[3] = {};   // by Release
    std::uint64_t releasedBytes = 0;
    std::uint64_t maxReleaseAge = 0;
    std::uint64_t youngestEarly = ~0ull;  // youngest age a cap / pressure release had
    std::uint64_t peakCount = 0;
    std::uint64_t peakBytes = 0;
};

struct Traced {
    std::uint64_t address = 0;
    std::atomic<std::uint64_t> block{0};  // the held block covering it, 0 when none
    std::uint64_t blockBytes = 0;
    std::uint64_t caller = 0;
    std::uint64_t heldAt = 0;
};

struct TraceEvent {
    unsigned slot;
    bool released;
    std::uint64_t block;
    std::uint64_t bytes;
    std::uint64_t caller;
    std::uint64_t at;
    std::uint64_t age;
    Release why;
};

std::uint64_t g_base = 0;
const volatile std::uint64_t* g_freeSlot = nullptr;
const volatile std::uint64_t* g_sizeSlot = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
FreeQuarantine::Fifo* g_fifo = nullptr;
Stats g_stats;  // under g_lock
Traced g_traced[TraceSlots];
unsigned g_tracedCount = 0;
std::uint64_t g_keepSites[KeepSiteSlots] = {};  // absolute return addresses
unsigned g_keepSiteCount = 0;
std::atomic<std::uint64_t> g_passedNull{0};
std::atomic<std::uint64_t> g_passedBig{0};
std::atomic<std::uint64_t> g_passedBigBytes{0};
std::atomic<std::uint64_t> g_lostNoFree{0};
std::atomic<std::uint64_t> g_freedAfterHold{0};
std::atomic<std::uint64_t> g_keptWritten{0};       // changed while held (some of them also at a keep site)
std::atomic<std::uint64_t> g_keptWrittenAtSite{0};
std::atomic<std::uint64_t> g_keptSiteOnly{0};      // at a keep site, unchanged while held
std::atomic<std::uint64_t> g_keptBytes{0};
double g_microsPerTick = 0.0;
LARGE_INTEGER g_start{};

std::uint64_t NowMicros() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(static_cast<double>(now.QuadPart - g_start.QuadPart) * g_microsPerTick);
}

const char* ReleaseName(Release why) {
    return why == Release::Age ? "age" : why == Release::Cap ? "cap" : "pressure";
}

std::uint64_t HashBlock(const void* block, std::uint64_t bytes) {
    const auto* cursor = static_cast<const unsigned char*>(block);
    const std::uint64_t words = (bytes < CheckBytes ? bytes : CheckBytes) / 8;
    std::uint64_t hash = 0x9e3779b97f4a7c15ull ^ words;
    for (std::uint64_t i = 0; i < words; ++i) {
        std::uint64_t word;
        std::memcpy(&word, cursor + i * 8, sizeof(word));
        hash = (hash ^ word) * 0x100000001b3ull;
        hash ^= hash >> 29;
    }
    return hash;
}

bool MemoryPressure() {
    const std::uint64_t limit = *reinterpret_cast<const volatile std::uint64_t*>(g_base + LimitRva);
    if (limit == 0) return false;
    const std::uint64_t mapped = *reinterpret_cast<const volatile std::uint64_t*>(g_base + MappedRva);
    const std::uint64_t headroom = limit / 16 > (8ull << 20) ? limit / 16 : (8ull << 20);
    return mapped + headroom > limit;
}

void DescribeFunction(std::uint64_t address, char* text, std::size_t size) {
    if (address == 0) std::snprintf(text, size, "none yet");
    else if (address >= g_base && address < g_base + 0x10000000) std::snprintf(text, size, "app.exe+0x%llx", static_cast<unsigned long long>(address - g_base));
    else std::snprintf(text, size, "0x%llx", static_cast<unsigned long long>(address));
}

bool IsKeepSite(std::uint64_t caller) {
    for (unsigned i = 0; i < g_keepSiteCount; ++i) {
        if (g_keepSites[i] == caller) return true;
    }
    return false;
}

// A due block either goes to Wwise's free or, when it changed while held or came from a keep site, stays allocated
// for good. Returns whether it was kept.
bool KeepInsteadOfFree(const Taken& taken) {
    const auto& entry = taken.entry;
    const bool written = HashBlock(reinterpret_cast<const void*>(entry.pointer), entry.bytes) != entry.contentHash;
    const bool site = IsKeepSite(entry.caller);
    if (!written && !site) {
        g_freedAfterHold.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_keptBytes.fetch_add(entry.bytes, std::memory_order_relaxed);
    if (!written) {
        g_keptSiteOnly.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (site) g_keptWrittenAtSite.fetch_add(1, std::memory_order_relaxed);
    const auto kept = g_keptWritten.fetch_add(1, std::memory_order_relaxed) + 1;
    if (kept <= KeptLines) {
        float head[4] = {};
        if (entry.bytes >= sizeof(head)) std::memcpy(head, reinterpret_cast<const void*>(entry.pointer), sizeof(head));
        char caller[64];
        DescribeFunction(entry.caller, caller, sizeof(caller));
        Report("[free-quarantine] t+%.3f s kept block 0x%llx for good: written after its free (%llu bytes, pool %llu, freed by %s%s %.3f s ago, due by %s); head %g %g %g %g\n",
            NowMicros() / 1e6, static_cast<unsigned long long>(entry.pointer), static_cast<unsigned long long>(entry.bytes), static_cast<unsigned long long>(entry.poolId),
            caller, site ? " = keep site" : "", taken.age / 1e6, ReleaseName(taken.why), head[0], head[1], head[2], head[3]);
    }
    return true;
}

void PrintTraceEvent(const TraceEvent& event) {
    if (!event.released) {
        char caller[64];
        DescribeFunction(event.caller, caller, sizeof(caller));
        Report("[free-quarantine] t+%.3f s %s traced 0x%llx: block 0x%llx (%llu bytes) freed by %s\n", event.at / 1e6, event.age != 0 ? "keeps for good" : "holds",
            static_cast<unsigned long long>(g_traced[event.slot].address), static_cast<unsigned long long>(event.block), static_cast<unsigned long long>(event.bytes), caller);
    } else {
        Report("[free-quarantine] t+%.3f s released traced 0x%llx: block 0x%llx after %.3f s (%s)\n", event.at / 1e6,
            static_cast<unsigned long long>(g_traced[event.slot].address), static_cast<unsigned long long>(event.block), event.age / 1e6, ReleaseName(event.why));
    }
}

// Wwise's Free, redirected: AK::MemoryMgr::Free(AkMemPoolId, void*), System V, on the guest thread that frees.
void APS5_VABI QuarantineFree(std::uint64_t poolId, void* pointer) {
    const auto realFree = reinterpret_cast<GuestFree>(*g_freeSlot);
    if (realFree == nullptr) {  // Wwise not initialised or already terminated: the stub would have jumped to 0
        g_lostNoFree.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (pointer == nullptr) {
        g_passedNull.fetch_add(1, std::memory_order_relaxed);
        realFree(poolId, pointer);
        return;
    }
    const auto sizeOf = reinterpret_cast<GuestSize>(*g_sizeSlot);
    Entry entry;
    entry.pointer = reinterpret_cast<std::uint64_t>(pointer);
    entry.poolId = poolId;
    entry.bytes = sizeOf != nullptr ? sizeOf(poolId, pointer) : 0;
    entry.caller = reinterpret_cast<std::uint64_t>(__builtin_return_address(0));
    if (entry.bytes >= BigBlockBytes) {
        g_passedBig.fetch_add(1, std::memory_order_relaxed);
        g_passedBigBytes.fetch_add(entry.bytes, std::memory_order_relaxed);
        realFree(poolId, pointer);
        return;
    }
    entry.contentHash = HashBlock(pointer, entry.bytes);
    const bool pressure = MemoryPressure();
    Taken due[ReleaseBatch];
    TraceEvent events[TraceSlots * 2];
    unsigned eventCount = 0;

    AcquireSRWLockExclusive(&g_lock);
    const std::uint64_t now = NowMicros();
    entry.heldAt = now;
    g_fifo->Push(entry);
    auto& stats = g_stats;
    ++stats.frees;
    ++stats.held;
    stats.heldBytes += entry.bytes;
    for (unsigned slot = 0; slot < g_tracedCount; ++slot) {
        auto& traced = g_traced[slot];
        const bool covers = entry.bytes != 0 ? traced.address >= entry.pointer && traced.address < entry.pointer + entry.bytes : traced.address == entry.pointer;
        if (!covers || traced.block.load(std::memory_order_relaxed) != 0) continue;
        traced.block.store(entry.pointer, std::memory_order_release);
        traced.blockBytes = entry.bytes;
        traced.caller = entry.caller;
        traced.heldAt = now;
        events[eventCount++] = TraceEvent{slot, false, entry.pointer, entry.bytes, entry.caller, now, 0, Release::Age};
    }
    if (g_fifo->Count() > stats.peakCount) stats.peakCount = g_fifo->Count();
    if (g_fifo->Bytes() > stats.peakBytes) stats.peakBytes = g_fifo->Bytes();
    const std::size_t count = g_fifo->TakeDue(now, pressure, due, ReleaseBatch);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& taken = due[i];
        ++stats.released[static_cast<unsigned>(taken.why)];
        stats.releasedBytes += taken.entry.bytes;
        if (taken.age > stats.maxReleaseAge) stats.maxReleaseAge = taken.age;
        if (taken.why != Release::Age && taken.age < stats.youngestEarly) stats.youngestEarly = taken.age;
        for (unsigned slot = 0; slot < g_tracedCount; ++slot) {
            auto& traced = g_traced[slot];
            if (traced.block.load(std::memory_order_relaxed) != taken.entry.pointer) continue;
            traced.block.store(0, std::memory_order_release);
            if (eventCount < TraceSlots * 2) events[eventCount++] = TraceEvent{slot, true, taken.entry.pointer, taken.entry.bytes, taken.entry.caller, now, taken.age, taken.why};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);

    bool keptAny = false;
    bool kept[ReleaseBatch] = {};
    for (std::size_t i = 0; i < count; ++i) {
        kept[i] = KeepInsteadOfFree(due[i]);
        keptAny = keptAny || kept[i];
        if (!kept[i]) realFree(due[i].entry.poolId, reinterpret_cast<void*>(due[i].entry.pointer));
    }
    for (unsigned i = 0; i < eventCount; ++i) {
        bool keptBlock = false;
        for (std::size_t j = 0; j < count; ++j) keptBlock = keptBlock || (kept[j] && events[i].released && due[j].entry.pointer == events[i].block);
        if (!keptBlock) PrintTraceEvent(events[i]);  // a kept one is reported below as "keeps for good"
    }
    if (!keptAny || g_tracedCount == 0) return;
    // A kept block that covers a traced address counts as held again (the write watch marks writes into it).
    eventCount = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (std::size_t i = 0; i < count; ++i) {
        if (!kept[i]) continue;
        const auto& held = due[i].entry;
        for (unsigned slot = 0; slot < g_tracedCount && eventCount < TraceSlots * 2; ++slot) {
            auto& traced = g_traced[slot];
            const bool covers = held.bytes != 0 ? traced.address >= held.pointer && traced.address < held.pointer + held.bytes : traced.address == held.pointer;
            if (!covers || traced.block.load(std::memory_order_relaxed) != 0) continue;
            traced.block.store(held.pointer, std::memory_order_release);
            events[eventCount++] = TraceEvent{slot, false, held.pointer, held.bytes, held.caller, now, 1, Release::Age};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (unsigned i = 0; i < eventCount; ++i) PrintTraceEvent(events[i]);
}

DWORD WINAPI StatsThread(LPVOID) {
    SetThreadDescription(GetCurrentThread(), L"aps5 free quarantine stats");
    Stats previous;
    std::uint64_t previousWritten = 0;
    std::uint64_t previousBig = 0;
    bool slotsReported = false;
    for (;;) {
        Sleep(10000);
        const std::uint64_t freeFn = *g_freeSlot;
        const std::uint64_t sizeFn = *g_sizeSlot;
        if (!slotsReported && freeFn != 0) {
            char freeText[64], sizeText[64];
            DescribeFunction(freeFn, freeText, sizeof(freeText));
            DescribeFunction(sizeFn, sizeText, sizeof(sizeText));
            Report("[free-quarantine] Wwise initialised: Free = %s (expected app.exe+0x5240cf0), SizeOfMemory = %s (expected app.exe+0x5240dc0)\n", freeText, sizeText);
            slotsReported = true;
        }
        AcquireSRWLockShared(&g_lock);
        const Stats stats = g_stats;
        const std::uint64_t now = NowMicros();
        const std::size_t heldCount = g_fifo->Count();
        const std::uint64_t heldBytes = g_fifo->Bytes();
        const std::uint64_t oldest = g_fifo->OldestAge(now);
        ReleaseSRWLockShared(&g_lock);
        const std::uint64_t maxAge = oldest > stats.maxReleaseAge ? oldest : stats.maxReleaseAge;
        const std::uint64_t mapped = *reinterpret_cast<const volatile std::uint64_t*>(g_base + MappedRva);
        const std::uint64_t limit = *reinterpret_cast<const volatile std::uint64_t*>(g_base + LimitRva);
        const std::uint64_t big = g_passedBig.load(std::memory_order_relaxed);
        char early[48] = "none";
        if (stats.youngestEarly != ~0ull) std::snprintf(early, sizeof(early), "%.1f s", stats.youngestEarly / 1e6);
        Report("[free-quarantine] t+%.0f s: held %llu blocks / %.1f MiB (peak %llu / %.1f MiB), oldest %.1f s, max age reached %.1f s; "
               "last 10 s: %llu frees held (%.2f MiB), %llu released (age %llu, cap %llu, pressure %llu), %llu big freed at once; "
               "in total %llu held, %llu released, youngest early release %s; Wwise mapped %.1f MiB, limit %.1f MiB\n",
            now / 1e6, static_cast<unsigned long long>(heldCount), heldBytes / 1048576.0, static_cast<unsigned long long>(stats.peakCount), stats.peakBytes / 1048576.0,
            oldest / 1e6, maxAge / 1e6,
            static_cast<unsigned long long>(stats.held - previous.held), (stats.heldBytes - previous.heldBytes) / 1048576.0,
            static_cast<unsigned long long>(stats.released[0] + stats.released[1] + stats.released[2] - previous.released[0] - previous.released[1] - previous.released[2]),
            static_cast<unsigned long long>(stats.released[0] - previous.released[0]), static_cast<unsigned long long>(stats.released[1] - previous.released[1]),
            static_cast<unsigned long long>(stats.released[2] - previous.released[2]), static_cast<unsigned long long>(big - previousBig),
            static_cast<unsigned long long>(stats.held), static_cast<unsigned long long>(stats.released[0] + stats.released[1] + stats.released[2]), early,
            mapped / 1048576.0, limit / 1048576.0);
        const std::uint64_t written = g_keptWritten.load(std::memory_order_relaxed);
        const std::uint64_t siteOnly = g_keptSiteOnly.load(std::memory_order_relaxed);
        Report("[free-quarantine] after the hold: %llu freed unchanged; kept for good %llu blocks / %.2f MiB: %llu written after their free (%llu in the last 10 s, %llu of them at keep sites), %llu more from keep sites\n",
            static_cast<unsigned long long>(g_freedAfterHold.load(std::memory_order_relaxed)), static_cast<unsigned long long>(written + siteOnly),
            g_keptBytes.load(std::memory_order_relaxed) / 1048576.0, static_cast<unsigned long long>(written), static_cast<unsigned long long>(written - previousWritten),
            static_cast<unsigned long long>(g_keptWrittenAtSite.load(std::memory_order_relaxed)), static_cast<unsigned long long>(siteOnly));
        previousWritten = written;
        const std::uint64_t lost = g_lostNoFree.load(std::memory_order_relaxed);
        if (lost != 0) Report("[free-quarantine] %llu frees came while Wwise's Free slot was empty (dropped)\n", static_cast<unsigned long long>(lost));
        previous = stats;
        previousBig = big;
    }
}

void AddTrace(const char* list) {
    std::uint64_t addresses[TraceSlots];
    g_tracedCount = FreeQuarantine::ParseAddressList(list, addresses, TraceSlots);
    for (unsigned slot = 0; slot < g_tracedCount; ++slot) g_traced[slot].address = addresses[slot];
}

bool IsExecutableAddress(const void* address) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT) return false;
    const DWORD protection = memory.Protect & 0xff;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

void Install(const char* spec) {
    std::uint64_t holdMicros = 0;
    std::uint64_t capBytes = 0;
    if (!FreeQuarantine::ParseSpec(spec, holdMicros, capBytes)) {
        Report("[free-quarantine] APS5_EXP_FREE_QUARANTINE=%s not understood (<seconds>[,<MiB cap>]); left off\n", spec);
        return;
    }
    g_base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    auto* const stub = reinterpret_cast<std::uint8_t*>(g_base + FreeStubRva);
    static const std::uint8_t expected[] = {0x48, 0x8b, 0x05};
    std::int32_t displacement = 0;
    const bool code = IsExecutableAddress(stub);
    if (code) std::memcpy(&displacement, stub + 3, sizeof(displacement));
    const bool shape = code && std::memcmp(stub, expected, sizeof(expected)) == 0 && stub[7] == 0xff && stub[8] == 0xe0 &&
        stub[9] == 0xcc && stub[10] == 0xcc && stub[11] == 0xcc;
    if (!shape || g_base + FreeStubRva + 7 + static_cast<std::int64_t>(displacement) != g_base + FreeSlotRva) {
        Report("[free-quarantine] app.exe+0x%llx is not Wwise's Free stub (mov rax,[app.exe+0x%llx]; jmp rax): %s; left off\n",
            static_cast<unsigned long long>(FreeStubRva), static_cast<unsigned long long>(FreeSlotRva), !code ? "not mapped code" : !shape ? "other bytes" : "other slot");
        return;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_microsPerTick = 1e6 / static_cast<double>(frequency.QuadPart);
    QueryPerformanceCounter(&g_start);
    g_freeSlot = reinterpret_cast<const volatile std::uint64_t*>(g_base + FreeSlotRva);
    g_sizeSlot = reinterpret_cast<const volatile std::uint64_t*>(g_base + SizeSlotRva);
    g_fifo = new FreeQuarantine::Fifo(holdMicros, capBytes);
    if (const char* keep = std::getenv("APS5_EXP_FREE_QUARANTINE_KEEP")) {
        g_keepSiteCount = FreeQuarantine::ParseAddressList(keep, g_keepSites, KeepSiteSlots);
    } else {
        for (const std::uint64_t site : DefaultKeepSites) g_keepSites[g_keepSiteCount++] = site;
    }
    for (unsigned i = 0; i < g_keepSiteCount; ++i) g_keepSites[i] += g_base;
    if (const char* trace = std::getenv("APS5_EXP_FREE_QUARANTINE_TRACE")) AddTrace(trace);
    else if (const char* watch = std::getenv("APS5_EXP_WATCH_WRITE")) AddTrace(watch);

    std::uint8_t jump[12] = {0x48, 0xb8};  // mov rax, imm64; jmp rax
    const auto target = reinterpret_cast<std::uint64_t>(&QuarantineFree);
    std::memcpy(jump + 2, &target, sizeof(target));
    jump[10] = 0xff;
    jump[11] = 0xe0;
    DWORD old = 0;
    if (!VirtualProtect(stub, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) {
        Report("[free-quarantine] VirtualProtect failed (%lu); left off\n", GetLastError());
        return;
    }
    std::memcpy(stub, jump, sizeof(jump));
    VirtualProtect(stub, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(jump));
    Report("[free-quarantine] Wwise Free stub app.exe+0x%llx redirected: hold %.1f s, cap %llu MiB, blocks >= 1 MiB freed at once, blocks written while held kept for good, %u keep site(s), %u traced address(es)\n",
        static_cast<unsigned long long>(FreeStubRva), holdMicros / 1e6, static_cast<unsigned long long>(capBytes >> 20), g_keepSiteCount, g_tracedCount);
    for (unsigned i = 0; i < g_keepSiteCount; ++i) Report("[free-quarantine] keep site app.exe+0x%llx\n", static_cast<unsigned long long>(g_keepSites[i] - g_base));
    for (unsigned slot = 0; slot < g_tracedCount; ++slot) Report("[free-quarantine] tracing 0x%llx\n", static_cast<unsigned long long>(g_traced[slot].address));
    CreateThread(nullptr, 0, StatsThread, nullptr, 0, nullptr);
}

const bool g_freeQuarantineInstalled = [] {
    if (const char* spec = std::getenv("APS5_EXP_FREE_QUARANTINE")) Install(spec);
    return true;
}();

}  // namespace

namespace FreeQuarantine {

bool HoldsTraced(std::uint64_t address) {
    for (unsigned slot = 0; slot < g_tracedCount; ++slot) {
        if (g_traced[slot].address == address) return g_traced[slot].block.load(std::memory_order_acquire) != 0;
    }
    return false;
}

}  // namespace FreeQuarantine
#endif
