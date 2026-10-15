#include "prx/libc/include/HostCommitLog.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace HostCommitLog {
namespace {

constexpr std::size_t ReportBytes = std::size_t{32} << 20;
constexpr double MiB = 1024.0 * 1024.0;
constexpr double GiB = MiB * 1024.0;

unsigned intervalSeconds() {
    static const unsigned seconds = [] {
        const char* text = std::getenv("APS5_HOST_COMMIT_LOG");
        if (text == nullptr) return 0u;
        const auto value = std::strtoul(text, nullptr, 10);
        return value == 0 ? 0u : static_cast<unsigned>(std::max<unsigned long>(5, value));
    }();
    return seconds;
}

struct Tagged {
    std::uintptr_t end;
    const char* owner;
};

std::mutex& registryLock() {
    static auto* lock = new std::mutex();
    return *lock;
}

std::map<std::uintptr_t, Tagged>& registry() {
    static auto* tagged = new std::map<std::uintptr_t, Tagged>();
    return *tagged;
}

void eraseRange(std::map<std::uintptr_t, Tagged>& ranges, std::uintptr_t start, std::uintptr_t end) {
    auto it = ranges.lower_bound(start);
    if (it != ranges.begin() && std::prev(it)->second.end > start) --it;
    while (it != ranges.end() && it->first < end) {
        const auto first = it->first;
        const auto range = it->second;
        it = ranges.erase(it);
        if (first < start) ranges.emplace(first, Tagged{start, range.owner});
        if (range.end > end) it = ranges.emplace(end, Tagged{range.end, range.owner}).first;
    }
}

struct Logical {
    std::int64_t bytes = 0;
    std::int64_t count = 0;
};

std::mutex& logicalLock() {
    static auto* lock = new std::mutex();
    return *lock;
}

std::map<std::string, Logical>& logicalRegistry() {
    static auto* counters = new std::map<std::string, Logical>();
    return *counters;
}

[[maybe_unused]] void printLogical() {
    std::vector<std::pair<std::string, Logical>> sorted;
    {
        std::lock_guard lock(logicalLock());
        sorted.assign(logicalRegistry().begin(), logicalRegistry().end());
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& first, const auto& second) { return first.second.bytes > second.second.bytes; });
    for (const auto& [name, counter] : sorted) {
        if (counter.count == 0 && counter.bytes == 0) continue;
        std::fprintf(stderr, "[host-commit]   logical %s: %.2f GiB in %lld allocations (counted by its owner, not walked; owners backed by Vulkan memory overlap vk-mapped)\n", name.c_str(), static_cast<double>(counter.bytes) / GiB, static_cast<long long>(counter.count));
    }
}

#ifdef _WIN32
using QueryWorkingSet = BOOL (WINAPI*)(HANDLE, PVOID, DWORD);
using QueryThread = LONG (WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

struct ThreadBasic {
    LONG exitStatus;
    void* teb;
    void* process;
    void* thread;
    ULONG_PTR affinity;
    LONG priority;
    LONG basePriority;
};

constexpr std::size_t TebDeallocationStack = 0x1478;

std::map<std::uintptr_t, std::string> threadStacks() {
    std::map<std::uintptr_t, std::string> stacks;
    static const auto query = reinterpret_cast<QueryThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (query == nullptr) return stacks;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return stacks;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto process = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process) continue;
        const HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr) continue;
        ThreadBasic basic{};
        std::uintptr_t deallocation = 0;
        SIZE_T read = 0;
        if (query(thread, 0, &basic, sizeof(basic), nullptr) >= 0 && basic.teb != nullptr)
            ReadProcessMemory(GetCurrentProcess(), static_cast<const char*>(basic.teb) + TebDeallocationStack, &deallocation, sizeof(deallocation), &read);
        if (deallocation != 0 && read == sizeof(deallocation)) {
            std::string name = "thread " + std::to_string(entry.th32ThreadID);
            PWSTR description = nullptr;
            if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
                char text[128] = "";
                WideCharToMultiByte(CP_UTF8, 0, description, -1, text, sizeof(text), nullptr, nullptr);
                LocalFree(description);
                if (text[0] != 0) name = text;
            }
            stacks.emplace(deallocation, std::move(name));
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return stacks;
}

class ResidentCounter {
public:
    ResidentCounter() : query(reinterpret_cast<QueryWorkingSet>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32QueryWorkingSetEx"))), entries(65536) {}

    std::uint64_t Count(std::uintptr_t start, std::uintptr_t end) {
        if (query == nullptr) return 0;
        std::uint64_t resident = 0;
        for (auto cursor = start; cursor < end;) {
            std::size_t used = 0;
            for (; used < entries.size() && cursor < end; ++used, cursor += 4096) entries[used].VirtualAddress = reinterpret_cast<PVOID>(cursor);
            if (!query(GetCurrentProcess(), entries.data(), static_cast<DWORD>(used * sizeof(entries[0])))) return resident;
            for (std::size_t index = 0; index < used; ++index) {
                if ((entries[index].VirtualAttributes.Flags & 1u) != 0) resident += 4096;
            }
        }
        return resident;
    }

private:
    QueryWorkingSet query;
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> entries;
};

struct Usage {
    std::uint64_t allocations = 0;
    std::uint64_t committed = 0;
    std::uint64_t resident = 0;

    void Add(std::uint64_t committedBytes, std::uint64_t residentBytes) {
        ++allocations;
        committed += committedBytes;
        resident += residentBytes;
    }
};

struct Owner {
    std::string category;
    std::string detail;
};

Owner ownerOf(std::uintptr_t base, std::uintptr_t end, DWORD type, const std::map<std::uintptr_t, Tagged>& tagged, const std::map<std::uintptr_t, std::string>& stacks, std::uintptr_t arenaBase, std::size_t arenaBytes) {
    auto it = tagged.upper_bound(base);
    if (it != tagged.begin() && std::prev(it)->second.end > base) return {std::prev(it)->second.owner, {}};
    if (it != tagged.end() && it->first < end) return {it->second.owner, {}};
    if (base >= arenaBase && base - arenaBase < arenaBytes) return {type == MEM_MAPPED ? "guest-view" : "guest-mmap", {}};
    if (const auto stack = stacks.find(base); stack != stacks.end()) return {"thread-stack", stack->second};
    if (type == MEM_IMAGE) return {"image", {}};
    return {type == MEM_MAPPED ? "host-mapped" : "host-private", {}};
}

const char* typeName(DWORD type) {
    return type == MEM_MAPPED ? "mapped" : type == MEM_IMAGE ? "image" : "private";
}

class Sweeper {
public:
    void Run() {
        const auto stacks = threadStacks();
        std::map<std::uintptr_t, Tagged> tagged;
        {
            std::lock_guard lock(registryLock());
            tagged = registry();
        }
        std::uintptr_t arenaBase = 0;
        std::size_t arenaBytes = 0;
        GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        std::map<std::string, Usage> owners;
        Usage privateUsage;
        Usage mappedUsage;
        Usage imageUsage;
        std::map<std::uintptr_t, std::uint64_t> seen;
        auto cursor = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
        const auto limit = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
        while (cursor < limit) {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)) break;
            if (memory.State == MEM_FREE) {
                cursor = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
                continue;
            }
            const auto allocation = memory.AllocationBase;
            const auto base = reinterpret_cast<std::uintptr_t>(allocation);
            const auto type = memory.Type;
            std::uint64_t committed = 0;
            std::uint64_t resident = 0;
            for (;;) {
                const auto regionStart = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
                const auto regionEnd = regionStart + memory.RegionSize;
                if (memory.State == MEM_COMMIT) {
                    committed += memory.RegionSize;
                    resident += counter.Count(regionStart, regionEnd);
                }
                cursor = regionEnd;
                if (cursor >= limit || VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory) || memory.State == MEM_FREE || memory.AllocationBase != allocation) break;
            }
            if (committed == 0) continue;
            auto& usage = type == MEM_MAPPED ? mappedUsage : type == MEM_IMAGE ? imageUsage : privateUsage;
            usage.Add(committed, resident);
            const auto owner = ownerOf(base, cursor, type, tagged, stacks, arenaBase, arenaBytes);
            owners[owner.category].Add(committed, resident);
            if (committed < ReportBytes) continue;
            seen.emplace(base, committed);
            const auto previous = reported.find(base);
            if (previous != reported.end() && previous->second * 10 <= committed * 11 && committed * 10 <= previous->second * 11) continue;
            std::fprintf(stderr, "[host-commit]   0x%llx: %.1f MiB committed, %.1f MiB resident, %s, %s%s%s\n", static_cast<unsigned long long>(base), static_cast<double>(committed) / MiB, static_cast<double>(resident) / MiB, typeName(type), owner.category.c_str(), owner.detail.empty() ? "" : " ", owner.detail.c_str());
        }
        reported = std::move(seen);
        const auto now = GetTickCount64();
        std::fprintf(stderr, "[host-commit] t+%.0f s: private %.2f / %.2f GiB (committed / resident), mapped %.2f / %.2f, image %.2f / %.2f\n", static_cast<double>(now - started) / 1000.0, static_cast<double>(privateUsage.committed) / GiB, static_cast<double>(privateUsage.resident) / GiB, static_cast<double>(mappedUsage.committed) / GiB, static_cast<double>(mappedUsage.resident) / GiB, static_cast<double>(imageUsage.committed) / GiB, static_cast<double>(imageUsage.resident) / GiB);
        std::vector<std::pair<std::string, Usage>> sorted(owners.begin(), owners.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& first, const auto& second) { return first.second.committed > second.second.committed; });
        for (const auto& [name, usage] : sorted) std::fprintf(stderr, "[host-commit]   %s: %.2f / %.2f GiB in %llu allocations\n", name.c_str(), static_cast<double>(usage.committed) / GiB, static_cast<double>(usage.resident) / GiB, static_cast<unsigned long long>(usage.allocations));
        printLogical();
        lastSweep = now;
    }

private:
    ResidentCounter counter;
    std::map<std::uintptr_t, std::uint64_t> reported;
    std::uint64_t started = GetTickCount64();
    std::uint64_t lastSweep = GetTickCount64();
};

DWORD WINAPI SweepLoop(void*) {
    Sweeper sweeper;
    for (;;) {
        Sleep(intervalSeconds() * 1000u);
        try {
            sweeper.Run();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[host-commit] sweep failed: %s\n", error.what());
        }
    }
}

const bool g_sweepStarted = [] {
    if (intervalSeconds() == 0) return false;
    const HANDLE thread = CreateThread(nullptr, 0, SweepLoop, nullptr, 0, nullptr);
    if (thread == nullptr) return false;
    SetThreadDescription(thread, L"host commit log");
    CloseHandle(thread);
    std::fprintf(stderr, "[host-commit] logging host commits of at least %zu MiB and an owner census every %u s (APS5_HOST_COMMIT_LOG)\n", ReportBytes >> 20u, intervalSeconds());
    return true;
}();
#endif

}

bool HostCommitLogEnabled_nid_no_patch() {
    return intervalSeconds() != 0;
}

void HostCommitNote_nid_no_patch(const char* owner, const void* address, std::size_t bytes) {
    if (intervalSeconds() == 0 || address == nullptr || bytes == 0) return;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    bool known = false;
    {
        std::lock_guard lock(registryLock());
        auto& ranges = registry();
        const auto found = ranges.find(start);
        known = found != ranges.end() && found->second.end == start + bytes;
        eraseRange(ranges, start, start + bytes);
        ranges.emplace(start, Tagged{start + bytes, owner});
    }
    if (!known && bytes >= ReportBytes) std::fprintf(stderr, "[host-commit] %s 0x%llx: %.1f MiB\n", owner, static_cast<unsigned long long>(start), static_cast<double>(bytes) / MiB);
}

void HostCommitCount_nid_no_patch(const char* owner, std::int64_t bytes, std::int64_t allocations) {
    if (intervalSeconds() == 0 || owner == nullptr) return;
    std::lock_guard lock(logicalLock());
    auto& counter = logicalRegistry()[owner];
    counter.bytes += bytes;
    counter.count += allocations;
}

void HostCommitForget_nid_no_patch(const void* address, std::size_t bytes) {
    if (intervalSeconds() == 0 || address == nullptr) return;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    std::lock_guard lock(registryLock());
    auto& ranges = registry();
    if (bytes == 0) ranges.erase(start);
    else eraseRange(ranges, start, start + bytes);
}

}
