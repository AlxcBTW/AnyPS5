#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP

#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <memory>
#include <vector>
#include <stdexcept>
#include <string>
#include <system_error>

namespace GuestArena {

class WindowsMappings {
public:
    static WindowsMappings& Get() {
        static WindowsMappings mappings;
        return mappings;
    }

    void* Reserve(void* address, std::size_t bytes) {
        return allocate(GetCurrentProcess(), address, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    }

    std::vector<std::pair<std::uintptr_t, std::size_t>> Commit(void* address, std::size_t bytes, DWORD protection, std::size_t granule, bool watched) {
        std::vector<std::pair<std::uintptr_t, std::size_t>> created;
        std::lock_guard lock(mutex);
        watchPrivate = watched;
        mappingSerial.fetch_add(1, std::memory_order_acq_rel);
        const auto end = reinterpret_cast<std::uintptr_t>(address) + bytes;
        for (auto cursor = reinterpret_cast<std::uintptr_t>(address); cursor < end;) {
            const auto memory = query(cursor);
            const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.State == MEM_RESERVE) {
                const auto limit = std::min(end, cursor + granule);
                auto placeholderEnd = stop;
                while (placeholderEnd < limit) {
                    const auto next = query(placeholderEnd);
                    if (next.State != MEM_RESERVE) break;
                    placeholderEnd = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
                }
                const auto size = std::min(limit, placeholderEnd) - cursor;
                reset(cursor, size);
                const DWORD flags = MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | (watched ? MEM_WRITE_WATCH : 0);
                if (!allocate(GetCurrentProcess(), reinterpret_cast<void*>(cursor), size, flags, protection, nullptr, 0)) fail("replace guest placeholder with private memory");
                if (!created.empty() && created.back().first + created.back().second == cursor) created.back().second += size;
                else created.emplace_back(cursor, size);
                if (watched) privateAllocations[cursor] = cursor + size;
                cursor += size;
            } else {
                if (memory.State != MEM_COMMIT) throw std::runtime_error("guest memory is not committed");
                const auto mapped = views.find(cursor & ~(pageBytes - 1));
                if (mapped != views.end()) {
                    mapped->second.protection = protection;
                    mapped->second.armed = false;
                    invalidate(*mapped->second.page);
                }
                DWORD previous;
                if (!VirtualProtect(reinterpret_cast<void*>(cursor), stop - cursor, protection, &previous)) fail("protect guest memory");
                cursor = stop;
            }
        }
        return created;
    }

    void Reset(void* address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        mappingSerial.fetch_add(1, std::memory_order_acq_rel);
        reset(reinterpret_cast<std::uintptr_t>(address), bytes);
    }

    void Map(void* address, std::size_t bytes, HANDLE section, std::uint64_t offset, DWORD protection) {
        std::lock_guard lock(mutex);
        mappingSerial.fetch_add(1, std::memory_order_acq_rel);
        auto cursor = reinterpret_cast<std::uintptr_t>(address);
        reset(cursor, bytes);
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) fail("keep shared guest section");
        const auto owned = std::make_shared<Section>(duplicate);
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            split(cursor + done, pageBytes);
            void* page = reinterpret_cast<void*>(cursor + done);
            if (!map(section, GetCurrentProcess(), page, offset + done, pageBytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) fail("map shared guest page");
            DWORD previous;
            if (!VirtualProtect(page, pageBytes, protection, &previous)) fail("protect shared guest page");
            const auto key = std::make_pair(reinterpret_cast<std::uintptr_t>(section), offset + done);
            auto shared = physical[key].lock();
            if (!shared) {
                shared = std::make_shared<SharedPage>();
                physical[key] = shared;
            }
            const auto base = cursor + done;
            shared->aliases.push_back(base);
            views.emplace(base, View{shared, protection, 0, false, owned, offset + done, 0});
            invalidate(*shared);
        }
    }

    void SetProtection(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        std::lock_guard lock(mutex);
        for (auto it = views.lower_bound(address); it != views.end() && it->first < address + bytes; ++it) {
            it->second.protection = protection;
            it->second.armed = false;
            invalidate(*it->second.page);
        }
    }

    void Pin(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            auto& page = *it->second.page;
            ++page.pins;
            for (const auto alias : page.aliases) {
                auto& view = views.at(alias);
                if (!view.armed) continue;
                DWORD previous;
                if (!VirtualProtect(reinterpret_cast<void*>(alias), pageBytes, view.protection, &previous)) fail("pin shared guest page writable");
                view.armed = false;
            }
            invalidate(page);
        }
    }

    void Unpin(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            auto& page = *it->second.page;
            if (page.pins != 0) --page.pins;
            invalidate(page);
        }
    }

    bool HandleWrite(std::uintptr_t address) {
        std::lock_guard lock(mutex);
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (found == views.end() || !writable(found->second.protection)) return false;
        auto& view = found->second;
        invalidate(*view.page);
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(base), pageBytes, view.protection, &previous)) fail("resume shared memory write");
        view.armed = false;
        return true;
    }

    void BeginRetarget(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(retargetMutex);
        retargets.emplace_back(address, address + bytes);
        retargeting.fetch_add(1, std::memory_order_acq_rel);
        retargeted.store(true, std::memory_order_release);
    }

    void EndRetarget(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(retargetMutex);
        const auto found = std::find(retargets.begin(), retargets.end(), std::make_pair(address, address + bytes));
        if (found == retargets.end()) throw std::logic_error("ending a guest range switch that did not begin");
        retargets.erase(found);
        retargeting.fetch_sub(1, std::memory_order_acq_rel);
    }

    bool AwaitRetarget(std::uintptr_t address, ULONG_PTR access) {
        if (!retargeted.load(std::memory_order_acquire)) return false;
        bool waited = false;
        while (retargeting.load(std::memory_order_acquire) != 0) {
            {
                std::lock_guard lock(retargetMutex);
                const bool inside = std::any_of(retargets.begin(), retargets.end(), [&](const auto& range) { return address >= range.first && address < range.second; });
                if (!inside) break;
            }
            waited = true;
            SwitchToThread();
        }
        if (waited) return true;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT) return false;
        const auto protection = memory.Protect & 0xffu;
        if ((memory.Protect & PAGE_GUARD) != 0 || protection == PAGE_NOACCESS) return false;
        if (access == 1) return protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE;
        if (access == 8) return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE;
        return true;
    }

    bool BeginHostWrite(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto first = views.lower_bound(address & ~(pageBytes - 1));
        const auto end = address + bytes;
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            if (!writable(it->second.protection)) return false;
        }
        for (auto cursor = address; cursor < end;) {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)) return false;
            const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.Type == MEM_PRIVATE && (memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0 || !writable(memory.Protect & 0xffu))) return false;
            cursor = stop;
        }
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            auto& view = it->second;
            ++view.hostWrites;
            invalidate(*view.page);
            if (!view.armed) continue;
            DWORD previous;
            if (!VirtualProtect(reinterpret_cast<void*>(it->first), pageBytes, view.protection, &previous)) fail("open shared memory to a host write");
            view.armed = false;
        }
        return true;
    }

    void EndHostWrite(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto end = address + bytes;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
            --it->second.hostWrites;
            invalidate(*it->second.page);
        }
    }

    void* MapAlias(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto refuse = [&](const char* reason) {
            char text[192];
            std::snprintf(text, sizeof(text), "read-write alias of shared guest memory 0x%llx+0x%llx: %s", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), reason);
            return std::runtime_error(text);
        };
        if (address % pageBytes != 0 || bytes % pageBytes != 0 || bytes == 0) throw refuse("the range is not made of whole shared pages");
        auto view = views.find(address);
        if (view == views.end()) throw refuse("the range does not start at a shared view");
        std::vector<AliasRun> runs;
        for (std::size_t done = 0; done < bytes; done += pageBytes, ++view) {
            if (view == views.end() || view->first != address + done) throw refuse("the range is not made of shared views only");
            if (!runs.empty() && runs.back().offset + runs.back().bytes == view->second.offset && (runs.back().section == view->second.section || sameSection(view->second.section->handle, runs.back().section->handle))) runs.back().bytes += pageBytes;
            else runs.push_back({view->second.section, view->second.offset, pageBytes});
        }
        const auto failed = [&](const char* step) {
            char text[192];
            std::snprintf(text, sizeof(text), "%s of a read-write alias of shared guest memory 0x%llx+0x%llx", step, static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
            return std::system_error(static_cast<int>(GetLastError()), std::system_category(), text);
        };
        if (runs.size() == 1) {
            SYSTEM_INFO system{};
            GetSystemInfo(&system);
            const auto lead = runs.front().offset % system.dwAllocationGranularity;
            void* alias = map(runs.front().section->handle, GetCurrentProcess(), nullptr, runs.front().offset - lead, lead + bytes, 0, PAGE_READWRITE, nullptr, 0);
            if (alias == nullptr) throw failed("MapViewOfFile3");
            return static_cast<char*>(alias) + lead;
        }
        auto* base = static_cast<char*>(allocate(GetCurrentProcess(), nullptr, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (base == nullptr) throw failed("VirtualAlloc2");
        std::vector<std::size_t> sizes;
        std::size_t done = 0;
        for (const auto& run : runs) {
            const bool split = done + run.bytes == bytes || VirtualFree(base + done, run.bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            if (!split || map(run.section->handle, GetCurrentProcess(), base + done, run.offset, run.bytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) == nullptr) {
                const auto error = failed(split ? "MapViewOfFile3" : "split placeholder");
                std::size_t unmapped = 0;
                for (const auto size : sizes) {
                    unmap(GetCurrentProcess(), base + unmapped, 0);
                    unmapped += size;
                }
                VirtualFree(base + unmapped, 0, MEM_RELEASE);
                if (split && done + run.bytes != bytes) VirtualFree(base + done + run.bytes, 0, MEM_RELEASE);
                throw error;
            }
            sizes.push_back(run.bytes);
            done += run.bytes;
        }
        aliases.emplace(base, std::move(sizes));
        return base;
    }

    void UnmapAlias(void* alias) {
        if (alias == nullptr) return;
        {
            std::lock_guard lock(mutex);
            if (const auto found = aliases.find(alias); found != aliases.end()) {
                auto* cursor = static_cast<char*>(alias);
                for (const auto size : found->second) {
                    if (!unmap(GetCurrentProcess(), cursor, 0)) fail("unmap shared guest alias");
                    cursor += size;
                }
                aliases.erase(found);
                return;
            }
        }
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto base = reinterpret_cast<std::uintptr_t>(alias) & ~(static_cast<std::uintptr_t>(system.dwAllocationGranularity) - 1);
        if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(base), 0)) fail("unmap shared guest alias");
    }

    bool Protection(std::uintptr_t address, std::uint32_t* protection) {
        std::lock_guard lock(mutex);
        const auto found = views.find(address & ~(pageBytes - 1));
        if (found == views.end()) return false;
        *protection = found->second.protection;
        return true;
    }

    bool Collect(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear) {
        const bool profile = collectProfiled();
        const auto requested = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        std::lock_guard lock(mutex);
        const CollectTimer timer{*this, profile, requested, profile ? std::chrono::steady_clock::now() : requested};
        const auto capacity = *count;
        *count = 0;
        const auto end = address + bytes;
        if (clear && !collectable(address, end)) return false;
        std::uintptr_t cleanStart = 0;
        std::uintptr_t cleanEnd = 0;
        const auto flushClean = [&] {
            if (cleanEnd > cleanStart) rememberClean(cleanStart, cleanEnd);
            cleanStart = cleanEnd = 0;
        };
        auto hint = views.end();
        for (auto cursor = address; cursor < end;) {
            const auto nextClean = cleanRanges.upper_bound(cursor);
            if (nextClean != cleanRanges.begin()) {
                const auto clean = std::prev(nextClean);
                if (cursor < clean->second) {
                    cursor = std::min(end, clean->second);
                    continue;
                }
            }
            const auto base = cursor & ~(pageBytes - 1);
            auto found = views.end();
            if (hint != views.end()) {
                if (hint->first == base) found = hint;
                else if (const auto next = std::next(hint); next != views.end() && next->first == base) found = next;
            }
            if (found == views.end()) found = views.find(base);
            hint = found;
            if (found != views.end()) {
                auto& view = found->second;
                const auto stop = std::min(end, base + pageBytes);
                if (view.protection == PAGE_NOACCESS) {
                    flushClean();
                    return false;
                }
                const bool pinned = view.page->pins != 0;
                if (pinned || view.seen != view.page->generation) {
                    const auto needed = (stop - cursor + 4095) / 4096;
                    if (needed > capacity - *count) {
                        for (auto at = cursor; *count < capacity; at += 4096) pages[(*count)++] = reinterpret_cast<void*>(at);
                        flushClean();
                        return true;
                    }
                    for (auto at = cursor; at < stop; at += 4096) pages[(*count)++] = reinterpret_cast<void*>(at);
                }
                if (clear && !pinned) {
                    for (const auto alias : view.page->aliases) {
                        auto& other = views.at(alias);
                        if (!writable(other.protection) || other.armed || other.hostWrites != 0) continue;
                        DWORD previous;
                        const DWORD protection = other.protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
                        if (!VirtualProtect(reinterpret_cast<void*>(alias), pageBytes, protection, &previous)) fail("arm shared memory write tracking");
                        other.armed = true;
                    }
                    view.seen = view.page->generation;
                    if (cleanEnd != base) {
                        flushClean();
                        cleanStart = base;
                    }
                    cleanEnd = base + pageBytes;
                }
                cursor = stop;
            } else {
                flushClean();
                const auto allocation = findPrivate(cursor);
                if (allocation == privateAllocations.end()) return false;
                const auto stop = std::min(end, allocation->second);
                ULONG_PTR available = std::min<ULONG_PTR>(capacity - *count, (stop - cursor + 4095) / 4096);
                if (available == 0) return true;
                DWORD granularity = 0;
                const auto watchStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                if (GetWriteWatch(clear ? WRITE_WATCH_FLAG_RESET : 0, reinterpret_cast<void*>(cursor), stop - cursor, pages + *count, &available, &granularity) != 0) fail("collect private guest writes");
                if (profile) {
                    collectStats[CollectWatchCalls].fetch_add(1, std::memory_order_relaxed);
                    collectStats[CollectWatchNs].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - watchStart).count()), std::memory_order_relaxed);
                    collectStats[CollectWatchBytes].fetch_add(stop - cursor, std::memory_order_relaxed);
                    collectStats[CollectPagesReported].fetch_add(available, std::memory_order_relaxed);
                }
                *count += available;
                if (*count == capacity) return true;
                cursor = stop;
            }
        }
        flushClean();
        return true;
    }

    bool ProbeClean(std::uintptr_t address, std::size_t bytes, std::uint64_t* serial) {
        thread_local std::vector<std::pair<std::uintptr_t, std::uintptr_t>> pieces;
        pieces.clear();
        {
            std::lock_guard lock(mutex);
            *serial = mappingSerial.load(std::memory_order_relaxed);
            const auto end = address + bytes;
            for (auto cursor = address; cursor < end;) {
                const auto nextClean = cleanRanges.upper_bound(cursor);
                if (nextClean != cleanRanges.begin()) {
                    const auto clean = std::prev(nextClean);
                    if (cursor < clean->second) {
                        cursor = std::min(end, clean->second);
                        continue;
                    }
                }
                if (views.find(cursor & ~(pageBytes - 1)) != views.end()) return false;
                const auto allocation = findPrivate(cursor);
                if (allocation == privateAllocations.end()) return false;
                const auto stop = std::min(end, allocation->second);
                pieces.emplace_back(cursor, stop);
                cursor = stop;
            }
        }
        for (const auto& [begin, stop] : pieces) {
            void* page = nullptr;
            ULONG_PTR count = 1;
            DWORD granularity = 0;
            if (GetWriteWatch(0, reinterpret_cast<void*>(begin), stop - begin, &page, &count, &granularity) != 0 || count != 0) return false;
        }
        return true;
    }

    enum CollectStatistic : std::size_t { CollectCalls, CollectLockWaitNs, CollectHeldNs, CollectWatchCalls, CollectWatchNs, CollectWatchBytes, CollectPagesReported, CollectQueries, CollectStatisticCount };

    void CollectStatistics(std::uint64_t* values, std::size_t count) const {
        for (std::size_t i = 0; i < count && i < CollectStatisticCount; ++i) values[i] = collectStats[i].load(std::memory_order_relaxed);
    }

    std::uint64_t MappingSerial() const {
        return mappingSerial.load(std::memory_order_acquire);
    }

private:
    static bool collectProfiled() {
        static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        return profiled;
    }

    struct CollectTimer {
        WindowsMappings& owner;
        bool profile;
        std::chrono::steady_clock::time_point requested;
        std::chrono::steady_clock::time_point locked;
        ~CollectTimer() {
            if (!profile) return;
            const auto now = std::chrono::steady_clock::now();
            owner.collectStats[CollectCalls].fetch_add(1, std::memory_order_relaxed);
            owner.collectStats[CollectLockWaitNs].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(locked - requested).count()), std::memory_order_relaxed);
            owner.collectStats[CollectHeldNs].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - locked).count()), std::memory_order_relaxed);
        }
    };

    bool collectable(std::uintptr_t address, std::uintptr_t end) {
        auto hint = views.end();
        for (auto cursor = address; cursor < end;) {
            if (const auto nextClean = cleanRanges.upper_bound(cursor); nextClean != cleanRanges.begin()) {
                if (const auto clean = std::prev(nextClean); cursor < clean->second) {
                    cursor = std::min(end, clean->second);
                    continue;
                }
            }
            const auto base = cursor & ~(pageBytes - 1);
            auto found = views.end();
            if (hint != views.end()) {
                if (const auto next = std::next(hint); next != views.end() && next->first == base) found = next;
            }
            if (found == views.end()) found = views.find(base);
            hint = found;
            if (found != views.end()) {
                if (found->second.protection == PAGE_NOACCESS) return false;
                cursor = std::min(end, base + pageBytes);
                continue;
            }
            const auto allocation = findPrivate(cursor);
            if (allocation == privateAllocations.end()) return false;
            cursor = std::min(end, allocation->second);
        }
        return true;
    }

    static constexpr std::size_t pageBytes = 0x4000;
    struct SharedPage {
        std::uint64_t generation = 1;
        std::vector<std::uintptr_t> aliases;
        std::uint32_t pins = 0;
    };
    struct Section {
        HANDLE handle;
        explicit Section(HANDLE handle) : handle(handle) {}
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;
        ~Section() { CloseHandle(handle); }
    };
    struct AliasRun {
        std::shared_ptr<Section> section;
        std::uint64_t offset;
        std::size_t bytes;
    };
    struct View {
        std::shared_ptr<SharedPage> page;
        DWORD protection;
        std::uint64_t seen;
        bool armed;
        std::shared_ptr<Section> section;
        std::uint64_t offset;
        std::uint32_t hostWrites;
    };
    void forgetClean(std::uintptr_t start, std::uintptr_t end) {
        auto it = cleanRanges.lower_bound(start);
        if (it != cleanRanges.begin() && std::prev(it)->second > start) --it;
        while (it != cleanRanges.end() && it->first < end) {
            const auto first = it->first;
            const auto last = it->second;
            it = cleanRanges.erase(it);
            if (first < start) cleanRanges.emplace(first, start);
            if (last > end) it = cleanRanges.emplace(end, last).first;
        }
    }

    void rememberClean(std::uintptr_t start, std::uintptr_t end) {
        auto it = cleanRanges.lower_bound(start);
        if (it != cleanRanges.begin() && std::prev(it)->second >= start) --it;
        while (it != cleanRanges.end() && it->first <= end) {
            start = std::min(start, it->first);
            end = std::max(end, it->second);
            it = cleanRanges.erase(it);
        }
        cleanRanges.emplace(start, end);
    }

    void invalidate(SharedPage& page) {
        ++page.generation;
        for (const auto alias : page.aliases) forgetClean(alias, alias + pageBytes);
    }

    bool sameSection(HANDLE first, HANDLE second) const {
        return compare != nullptr && compare(first, second);
    }

    static bool writable(DWORD protection) {
        return protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE;
    }
    using AllocateFunction = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapFunction = PVOID (WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using UnmapFunction = BOOL (WINAPI*)(HANDLE, PVOID, ULONG);
    using CompareFunction = BOOL (WINAPI*)(HANDLE, HANDLE);

    WindowsMappings() {
        const auto module = GetModuleHandleW(L"KernelBase.dll");
        if (!module) fail("load Windows memory API");
        allocate = reinterpret_cast<AllocateFunction>(GetProcAddress(module, "VirtualAlloc2"));
        map = reinterpret_cast<MapFunction>(GetProcAddress(module, "MapViewOfFile3"));
        unmap = reinterpret_cast<UnmapFunction>(GetProcAddress(module, "UnmapViewOfFile2"));
        if (!allocate || !map || !unmap) throw std::runtime_error("Windows placeholder memory APIs are required");
        compare = reinterpret_cast<CompareFunction>(GetProcAddress(module, "CompareObjectHandles"));
    }

    [[noreturn]] static void fail(const char* operation) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }

    static MEMORY_BASIC_INFORMATION query(std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)) fail("query guest memory");
        return memory;
    }

    static void split(std::uintptr_t address, std::size_t bytes) {
        auto memory = query(address);
        memory = query(reinterpret_cast<std::uintptr_t>(memory.AllocationBase));
        if (memory.State != MEM_RESERVE) throw std::runtime_error("guest mapping requires a placeholder");
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (address != base) {
            if (!VirtualFree(reinterpret_cast<void*>(base), address - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder prefix");
            memory = query(address);
        }
        if (memory.RegionSize < bytes) throw std::runtime_error("guest placeholder is too small");
        if (memory.RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder suffix");
    }

    void reset(std::uintptr_t address, std::size_t bytes) {
        const auto end = address + bytes;
        forgetClean(address, end);
        for (auto cursor = address; cursor < end;) {
            const auto memory = query(cursor);
            if (memory.State == MEM_RESERVE) {
                cursor = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                continue;
            }
            const auto allocationBase = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
            auto allocationEnd = cursor;
            do {
                const auto part = query(allocationEnd);
                if (part.AllocationBase != memory.AllocationBase) break;
                allocationEnd = reinterpret_cast<std::uintptr_t>(part.BaseAddress) + part.RegionSize;
            } while (allocationEnd < end);
            const bool beyond = allocationEnd > end || query(allocationEnd).AllocationBase == memory.AllocationBase;
            if (memory.Type == MEM_PRIVATE && (allocationBase != cursor || beyond)) {
                while (query(allocationEnd).AllocationBase == memory.AllocationBase) allocationEnd = reinterpret_cast<std::uintptr_t>(query(allocationEnd).BaseAddress) + query(allocationEnd).RegionSize;
                releasePartOfPrivate(allocationBase, allocationEnd, cursor, std::min(end, allocationEnd));
                cursor = std::min(end, allocationEnd);
                continue;
            }
            if (allocationBase != cursor) throw std::runtime_error("cannot release part of a host allocation");
            if (beyond) throw std::runtime_error("guest release truncates a host allocation");
            if (memory.Type == MEM_MAPPED) {
                if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(cursor), MEM_PRESERVE_PLACEHOLDER)) fail("unmap shared guest page");
                const auto found = views.find(cursor);
                if (found != views.end()) {
                    std::erase(found->second.page->aliases, cursor);
                    views.erase(found);
                }
            } else if (memory.Type == MEM_PRIVATE) {
                if (!VirtualFree(reinterpret_cast<void*>(cursor), allocationEnd - cursor, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("release private guest memory");
                privateAllocations.erase(cursor);
            } else {
                throw std::runtime_error("unsupported guest mapping type");
            }
            cursor = allocationEnd;
        }
        const auto last = query(reinterpret_cast<std::uintptr_t>(query(end - 1).AllocationBase));
        const auto lastBase = reinterpret_cast<std::uintptr_t>(last.BaseAddress);
        if (lastBase + last.RegionSize > end) split(lastBase, end - lastBase);
        const auto first = query(address);
        split(address, std::min(bytes, reinterpret_cast<std::uintptr_t>(first.BaseAddress) + first.RegionSize - address));
        if (query(address).RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) fail("coalesce guest placeholders");
    }

    void releasePartOfPrivate(std::uintptr_t base, std::uintptr_t end, std::uintptr_t start, std::uintptr_t stop) {
        struct Region {
            std::uintptr_t begin;
            std::size_t bytes;
            DWORD protection;
        };
        struct Kept {
            std::uintptr_t begin;
            std::uintptr_t end;
            std::vector<Region> regions;
            std::vector<std::byte> content;
        };
        std::vector<Kept> kept;
        if (base < start) kept.push_back({base, start, {}, {}});
        if (stop < end) kept.push_back({stop, end, {}, {}});
        BeginRetarget(base, end - base);
        try {
            for (auto& part : kept) {
                for (auto cursor = part.begin; cursor < part.end;) {
                    const auto memory = query(cursor);
                    const auto regionEnd = std::min(part.end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                    part.regions.push_back({cursor, static_cast<std::size_t>(regionEnd - cursor), memory.Protect});
                    DWORD previous;
                    if (!VirtualProtect(reinterpret_cast<void*>(cursor), regionEnd - cursor, PAGE_READONLY, &previous)) fail("hold private guest memory for a split");
                    cursor = regionEnd;
                }
                part.content.resize(part.end - part.begin);
                std::memcpy(part.content.data(), reinterpret_cast<const void*>(part.begin), part.content.size());
            }
            if (!VirtualFree(reinterpret_cast<void*>(base), end - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("release private guest memory for a split");
            privateAllocations.erase(base);
            for (auto& part : kept) {
                const auto bytes = part.end - part.begin;
                split(part.begin, bytes);
                const DWORD flags = MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | (watchPrivate ? MEM_WRITE_WATCH : 0);
                if (!allocate(GetCurrentProcess(), reinterpret_cast<void*>(part.begin), bytes, flags, PAGE_READWRITE, nullptr, 0)) fail("commit a kept part of private guest memory");
                if (watchPrivate) privateAllocations[part.begin] = part.end;
                std::memcpy(reinterpret_cast<void*>(part.begin), part.content.data(), bytes);
                for (const auto& region : part.regions) {
                    DWORD previous;
                    if (!VirtualProtect(reinterpret_cast<void*>(region.begin), region.bytes, region.protection, &previous)) fail("restore a kept part's protection");
                }
            }
        } catch (...) {
            EndRetarget(base, end - base);
            throw;
        }
        EndRetarget(base, end - base);
    }

    std::map<std::uintptr_t, std::uintptr_t>::iterator findPrivate(std::uintptr_t address) {
        auto it = privateAllocations.upper_bound(address);
        if (it == privateAllocations.begin()) return privateAllocations.end();
        --it;
        return address < it->second ? it : privateAllocations.end();
    }

    bool watchPrivate = true;
    std::map<std::uintptr_t, std::uintptr_t> privateAllocations;
    std::map<std::uintptr_t, std::uintptr_t> cleanRanges;
    std::map<std::uintptr_t, View> views;
    std::mutex retargetMutex;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> retargets;
    std::atomic<std::uint32_t> retargeting{0};
    std::atomic<bool> retargeted{false};
    std::map<void*, std::vector<std::size_t>> aliases;
    std::map<std::pair<std::uintptr_t, std::uint64_t>, std::weak_ptr<SharedPage>> physical;
    std::mutex mutex;
    std::atomic<std::uint64_t> mappingSerial{0};
    std::array<std::atomic<std::uint64_t>, CollectStatisticCount> collectStats{};
    AllocateFunction allocate = nullptr;
    MapFunction map = nullptr;
    UnmapFunction unmap = nullptr;
    CompareFunction compare = nullptr;
};

}
#endif

#endif
