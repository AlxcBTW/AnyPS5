#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP

#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
    enum class Collection { All, Armed, Fresh };

    struct Statistics {
        std::uint64_t viewsMapped = 0;
        std::uint64_t viewsUnmapped = 0;
        std::uint64_t pagesMapped = 0;
        std::uint64_t pagesUnmapped = 0;
        std::uint64_t splits = 0;
        std::uint64_t livePages = 0;
        std::uint64_t pendingBytes = 0;
        std::uint64_t pendingRanges = 0;
        std::uint64_t touchCommits = 0;
        std::uint64_t touchCommitBytes = 0;
    };

    static WindowsMappings& Get() {
        static WindowsMappings mappings;
        return mappings;
    }

    void* Reserve(void* address, std::size_t bytes) {
        return allocate(GetCurrentProcess(), address, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    }

    std::vector<std::pair<std::uintptr_t, std::size_t>> Commit(void* address, std::size_t bytes, DWORD protection, std::size_t granule, bool watched, bool onTouch = false) {
        std::vector<std::pair<std::uintptr_t, std::size_t>> created;
        std::lock_guard lock(mutex);
        collectableRanges.clear();
        const auto end = reinterpret_cast<std::uintptr_t>(address) + bytes;
        for (auto cursor = reinterpret_cast<std::uintptr_t>(address); cursor < end;) {
            const auto memory = query(cursor);
            const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.State == MEM_RESERVE && onTouch && granule != 0) {
                if (!pendingCovers(cursor, stop)) append(created, cursor, stop - cursor);
                setPending(cursor, stop, PendingRange{stop, protection, granule, watched});
                cursor = stop;
            } else if (memory.State == MEM_RESERVE) {
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
                append(created, cursor, size);
                cursor += size;
            } else {
                if (memory.State != MEM_COMMIT) throw std::runtime_error("guest memory is not committed");
                for (auto mapped = views.lower_bound(cursor & ~(pageBytes - 1)); mapped != views.end() && mapped->first < stop; ++mapped) {
                    mapped->second.protection = protection;
                    mapped->second.armed = 0;
                    invalidate(*mapped->second.page);
                }
                DWORD previous;
                if (!VirtualProtect(reinterpret_cast<void*>(cursor), stop - cursor, protection, &previous)) fail("protect guest memory");
                cursor = stop;
            }
        }
        return created;
    }

    void CommitPending(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        commitPendingRange(address, address + bytes);
    }

    bool Pending(std::uintptr_t address, std::uintptr_t* end, std::uint32_t* protection) {
        std::lock_guard lock(mutex);
        const auto found = pendingAt(address);
        if (found == pending.end()) return false;
        *end = found->second.end;
        *protection = found->second.protection;
        return true;
    }

    void Reset(void* address, std::size_t bytes) {
        std::unique_lock lock(mutex);
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        awaitHostWrites(lock, start, start + bytes);
        reset(start, bytes);
    }

    void Map(void* address, std::size_t bytes, HANDLE section, std::uint64_t offset, DWORD protection) {
        std::unique_lock lock(mutex);
        collectableRanges.clear();
        const auto cursor = reinterpret_cast<std::uintptr_t>(address);
        awaitHostWrites(lock, cursor, cursor + bytes);
        reset(cursor, bytes);
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) fail("keep shared guest section");
        const auto owned = std::make_shared<Section>(duplicate);
        const auto limit = coalesce ? viewBytes : pageBytes;
        for (auto at = cursor; at < cursor + bytes;) {
            const auto next = std::min(cursor + bytes, (at / limit + 1) * limit);
            split(at, next - at);
            mapView(section, at, offset + (at - cursor), next - at);
            DWORD previous;
            if (!VirtualProtect(reinterpret_cast<void*>(at), next - at, protection, &previous)) fail("protect shared guest page");
            at = next;
        }
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            const auto key = std::make_pair(reinterpret_cast<std::uintptr_t>(section), offset + done);
            auto shared = physical[key].lock();
            if (!shared) {
                shared = std::make_shared<SharedPage>();
                physical[key] = shared;
            }
            const auto base = cursor + done;
            shared->aliases.push_back(base);
            views.emplace(base, View{shared, protection, {}, 0, owned, offset + done, 0});
            invalidate(*shared);
        }
        remember(freshRanges, cursor, cursor + bytes);
        statistics.pagesMapped += bytes / pageBytes;
    }

    void SetProtection(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        std::lock_guard lock(mutex);
        collectableRanges.clear();
        for (auto it = views.lower_bound(address); it != views.end() && it->first < address + bytes; ++it) {
            it->second.protection = protection;
            it->second.armed = 0;
            invalidate(*it->second.page);
        }
        protectPending(address, address + bytes, protection);
    }

    void Pin(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        commitPendingRange(address, address + bytes);
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            auto& page = *it->second.page;
            ++page.pins;
            for (const auto alias : page.aliases) {
                auto& view = views.at(alias);
                if (view.armed == 0) continue;
                protectSubpages(alias, view.armed, view.protection, "pin shared guest page writable");
                view.armed = 0;
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
        return HandleAccess(address, 1);
    }

    bool HandleAccess(std::uintptr_t address, std::uint32_t kind) {
        std::lock_guard lock(mutex);
        try {
            if (commitPendingAt(address)) return true;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[guest-arena] commit on first touch of 0x%llx failed: %s\n", static_cast<unsigned long long>(address), error.what());
            return false;
        }
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (kind == 1 && found != views.end() && writable(found->second.protection)) {
            auto& view = found->second;
            const auto written = subpagesOf(base, address, address + 1);
            invalidate(*view.page, written);
            protectSubpages(base, written, view.protection, "resume shared memory write");
            view.armed &= static_cast<std::uint8_t>(~written);
            return true;
        }
        return accessibleNow(address, kind);
    }

    bool BeginHostWrite(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto end = address + bytes;
        commitPendingRange(address, end);
        const auto first = views.lower_bound(address & ~(pageBytes - 1));
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            if (!writable(it->second.protection)) return false;
        }
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            auto& view = it->second;
            ++view.hostWrites;
            const auto written = subpagesOf(it->first, address, end);
            invalidate(*view.page, written);
            const auto opened = static_cast<std::uint8_t>(view.armed & written);
            if (opened == 0) continue;
            protectSubpages(it->first, opened, view.protection, "open shared memory to a host write");
            view.armed &= static_cast<std::uint8_t>(~opened);
        }
        return true;
    }

    void EndHostWrite(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto end = address + bytes;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
            --it->second.hostWrites;
            invalidate(*it->second.page, subpagesOf(it->first, address, end));
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
        const auto section = view->second.section;
        const auto offset = view->second.offset;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        bool contiguous = true;
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            const auto page = views.find(address + done);
            if (page == views.end()) throw refuse("the range is not a run of shared views");
            if (page->second.offset != offset + done || (page->second.section != section && !sameSection(page->second.section->handle, section->handle))) contiguous = false;
        }
        if (!contiguous) return mapAliasRuns(address, bytes);
        const auto lead = offset % system.dwAllocationGranularity;
        void* alias = map(section->handle, GetCurrentProcess(), nullptr, offset - lead, lead + bytes, 0, PAGE_READWRITE, nullptr, 0);
        if (alias == nullptr) {
            char text[160];
            std::snprintf(text, sizeof(text), "MapViewOfFile3 of a read-write alias of shared guest memory 0x%llx+0x%llx", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), text);
        }
        return static_cast<char*>(alias) + lead;
    }

    void UnmapAlias(void* alias) {
        if (alias == nullptr) return;
        {
            std::lock_guard lock(mutex);
            if (const auto found = splitAliases.find(reinterpret_cast<std::uintptr_t>(alias)); found != splitAliases.end()) {
                const auto error = releaseAlias(found->first, found->second);
                splitAliases.erase(found);
                if (error != ERROR_SUCCESS) throw std::system_error(static_cast<int>(error), std::system_category(), "unmap a run of a shared guest alias");
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

    Statistics Snapshot() {
        std::lock_guard lock(mutex);
        auto result = statistics;
        result.livePages = views.size();
        result.pendingRanges = pending.size();
        for (const auto& [start, range] : pending) result.pendingBytes += range.end - start;
        return result;
    }

    bool Coalesced() const {
        return coalesce;
    }

    bool Collect(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear, Collection collection = Collection::All) {
        std::lock_guard lock(mutex);
        const auto capacity = *count;
        *count = 0;
        const auto end = address + bytes;
        if (collection == Collection::Fresh) {
            const auto fresh = freshRanges.upper_bound(address);
            if ((fresh == freshRanges.begin() || std::prev(fresh)->second <= address) && (fresh == freshRanges.end() || fresh->first >= end)) return true;
            for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
                if (it->second.protection != PAGE_NOACCESS) continue;
                for (auto at = std::max(address, it->first); at < std::min(end, it->first + pageBytes); at += 4096) {
                    if ((it->second.collected & (1u << ((at - it->first) / 4096))) == 0) return false;
                }
            }
        } else if (clear && !collectable(address, end)) return false;
        auto position = views.lower_bound(address & ~(pageBytes - 1));
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
            const auto found = viewAt(base, position);
            if (found != views.end()) {
                auto& view = found->second;
                auto stop = std::min(std::min(end, base + pageBytes), nextClean == cleanRanges.end() ? end : nextClean->first);
                if (collection != Collection::All) {
                    const auto selected = [&](std::uintptr_t at) {
                        const bool collected = (view.collected & (1u << ((at - base) / 4096))) != 0;
                        return collection == Collection::Armed ? collected : !collected;
                    };
                    while (cursor < stop && !selected(cursor)) cursor = std::min(stop, cursor + 4096);
                    if (cursor == stop) continue;
                    for (auto at = cursor + 4096; at < stop; at += 4096) {
                        if (!selected(at)) {
                            stop = at;
                            break;
                        }
                    }
                }
                if (view.protection == PAGE_NOACCESS) return false;
                const bool pinned = view.page->pins != 0;
                std::uint8_t settled = 0;
                auto settledEnd = cursor;
                bool full = false;
                for (std::size_t sub = 0; sub < subpages; ++sub) {
                    const auto from = std::max(cursor, base + sub * subpageBytes);
                    const auto to = std::min(stop, base + (sub + 1) * subpageBytes);
                    if (from >= to) continue;
                    if (collection == Collection::Fresh || pinned || view.seen[sub] != view.page->generation[sub]) {
                        if (*count == capacity) {
                            full = true;
                            break;
                        }
                        pages[(*count)++] = reinterpret_cast<void*>(from);
                    }
                    settled |= static_cast<std::uint8_t>(1u << sub);
                    settledEnd = to;
                }
                if (clear) {
                    for (std::size_t sub = 0; sub < subpages; ++sub) {
                        const auto bit = static_cast<std::uint8_t>(1u << sub);
                        if ((settled & bit) == 0 || (view.collected & bit) != 0) continue;
                        view.collected |= bit;
                        forget(freshRanges, base + sub * subpageBytes, base + (sub + 1) * subpageBytes);
                    }
                }
                if (clear && !pinned && settled != 0) {
                    std::uint8_t untracked = 0;
                    for (const auto alias : view.page->aliases) {
                        auto& other = views.at(alias);
                        const auto arming = static_cast<std::uint8_t>(settled & ~other.armed);
                        if (!writable(other.protection) || arming == 0) continue;
                        if (other.hostWrites != 0) {
                            untracked |= arming;
                            continue;
                        }
                        const DWORD protection = other.protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
                        protectSubpages(alias, arming, protection, "arm shared memory write tracking");
                        other.armed |= arming;
                    }
                    const auto tracked = static_cast<std::uint8_t>(settled & ~untracked);
                    auto cleanFrom = cursor;
                    auto cleanTo = cursor;
                    for (std::size_t sub = 0; sub < subpages; ++sub) {
                        if ((tracked & (1u << sub)) == 0) continue;
                        const auto from = std::max(cursor, base + sub * subpageBytes);
                        const auto to = std::min(settledEnd, base + (sub + 1) * subpageBytes);
                        if (from == base + sub * subpageBytes && to == base + (sub + 1) * subpageBytes) view.seen[sub] = view.page->generation[sub];
                        if (from != cleanTo) {
                            if (cleanFrom != cleanTo) rememberClean(cleanFrom, cleanTo);
                            cleanFrom = from;
                        }
                        cleanTo = to;
                    }
                    if (cleanFrom != cleanTo) rememberClean(cleanFrom, cleanTo);
                }
                if (full) return true;
                cursor = stop;
            } else {
                if (collection == Collection::Fresh) {
                    const auto next = views.lower_bound(cursor);
                    cursor = next == views.end() ? end : std::min(end, next->first);
                    continue;
                }
                const auto memory = query(cursor);
                if (memory.State == MEM_RESERVE) {
                    const auto untouched = pendingAt(cursor);
                    if (untouched == pending.end()) return false;
                    cursor = std::min({end, untouched->second.end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize});
                    continue;
                }
                if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
                const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                if (capacity == *count) return true;
                const auto touched = (((stop + 4095) & ~static_cast<std::uintptr_t>(4095)) - (cursor & ~static_cast<std::uintptr_t>(4095))) / 4096;
                ULONG_PTR available = std::min<ULONG_PTR>(capacity - *count, touched);
                DWORD granularity = 0;
                if (GetWriteWatch(clear ? WRITE_WATCH_FLAG_RESET : 0, reinterpret_cast<void*>(cursor), stop - cursor, pages + *count, &available, &granularity) != 0) fail("collect private guest writes");
                *count += available;
                if (*count == capacity) return true;
                cursor = stop;
            }
        }
        return true;
    }

private:
    bool collectable(std::uintptr_t address, std::uintptr_t end) {
        const auto proven = collectableRanges.upper_bound(address);
        if (proven != collectableRanges.begin() && std::prev(proven)->second >= end) return true;
        auto position = views.lower_bound(address & ~(pageBytes - 1));
        for (auto cursor = address; cursor < end;) {
            const auto base = cursor & ~(pageBytes - 1);
            if (const auto found = viewAt(base, position); found != views.end()) {
                if (found->second.protection == PAGE_NOACCESS) return false;
                cursor = std::min(end, base + pageBytes);
                continue;
            }
            const auto memory = query(cursor);
            const auto regionEnd = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            if (memory.State == MEM_RESERVE) {
                const auto untouched = pendingAt(cursor);
                if (untouched == pending.end()) return false;
                cursor = std::min({end, untouched->second.end, regionEnd});
                continue;
            }
            if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
            cursor = std::min(end, regionEnd);
        }
        remember(collectableRanges, address, end);
        return true;
    }

    static constexpr std::size_t pageBytes = 0x4000;
    static constexpr std::size_t viewBytes = 0x200000;
    static constexpr std::size_t subpageBytes = 0x1000;
    static_assert(subpageBytes == 4096, "Collect reports one page per subpage");
    static constexpr std::size_t subpages = pageBytes / subpageBytes;
    static constexpr std::uint8_t allSubpages = (1u << subpages) - 1;
    struct SharedPage {
        std::array<std::uint64_t, subpages> generation = [] {
            std::array<std::uint64_t, subpages> initial{};
            initial.fill(1);
            return initial;
        }();
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
    struct View {
        std::shared_ptr<SharedPage> page;
        DWORD protection;
        std::array<std::uint64_t, subpages> seen;
        std::uint8_t armed;
        std::shared_ptr<Section> section;
        std::uint64_t offset;
        std::uint32_t hostWrites;
        std::uint8_t collected = 0;
    };
    struct PendingRange {
        std::uintptr_t end;
        DWORD protection;
        std::size_t granule;
        bool watched;
    };
    using Ranges = std::map<std::uintptr_t, std::uintptr_t>;
    using PendingRanges = std::map<std::uintptr_t, PendingRange>;

    static void append(std::vector<std::pair<std::uintptr_t, std::size_t>>& ranges, std::uintptr_t start, std::size_t bytes) {
        if (!ranges.empty() && ranges.back().first + ranges.back().second == start) ranges.back().second += bytes;
        else ranges.emplace_back(start, bytes);
    }

    static void forget(Ranges& ranges, std::uintptr_t start, std::uintptr_t end) {
        auto it = ranges.lower_bound(start);
        if (it != ranges.begin() && std::prev(it)->second > start) --it;
        while (it != ranges.end() && it->first < end) {
            const auto first = it->first;
            const auto last = it->second;
            it = ranges.erase(it);
            if (first < start) ranges.emplace(first, start);
            if (last > end) it = ranges.emplace(end, last).first;
        }
    }

    static void remember(Ranges& ranges, std::uintptr_t start, std::uintptr_t end) {
        auto it = ranges.lower_bound(start);
        if (it != ranges.begin() && std::prev(it)->second >= start) --it;
        while (it != ranges.end() && it->first <= end) {
            start = std::min(start, it->first);
            end = std::max(end, it->second);
            it = ranges.erase(it);
        }
        ranges.emplace(start, end);
    }

    void forgetClean(std::uintptr_t start, std::uintptr_t end) {
        forget(cleanRanges, start, end);
    }

    void rememberClean(std::uintptr_t start, std::uintptr_t end) {
        remember(cleanRanges, start, end);
    }

    static bool samePending(const PendingRange& first, const PendingRange& second) {
        return first.protection == second.protection && first.granule == second.granule && first.watched == second.watched;
    }

    PendingRanges::iterator pendingAt(std::uintptr_t address) {
        auto it = pending.upper_bound(address);
        if (it == pending.begin()) return pending.end();
        --it;
        return address < it->second.end ? it : pending.end();
    }

    bool pendingCovers(std::uintptr_t start, std::uintptr_t end) {
        for (auto cursor = start; cursor < end;) {
            const auto found = pendingAt(cursor);
            if (found == pending.end()) return false;
            cursor = found->second.end;
        }
        return true;
    }

    void forgetPending(std::uintptr_t start, std::uintptr_t end) {
        auto it = pending.lower_bound(start);
        if (it != pending.begin() && std::prev(it)->second.end > start) --it;
        while (it != pending.end() && it->first < end) {
            const auto first = it->first;
            const auto range = it->second;
            it = pending.erase(it);
            if (first < start) pending.emplace(first, PendingRange{start, range.protection, range.granule, range.watched});
            if (range.end > end) it = pending.emplace(end, PendingRange{range.end, range.protection, range.granule, range.watched}).first;
        }
    }

    void setPending(std::uintptr_t start, std::uintptr_t end, PendingRange range) {
        forgetPending(start, end);
        range.end = end;
        auto it = pending.emplace(start, range).first;
        if (it != pending.begin()) {
            const auto before = std::prev(it);
            if (before->second.end == start && samePending(before->second, range)) {
                before->second.end = end;
                pending.erase(it);
                it = before;
            }
        }
        const auto after = std::next(it);
        if (after != pending.end() && after->first == it->second.end && samePending(after->second, it->second)) {
            it->second.end = after->second.end;
            pending.erase(after);
        }
    }

    void protectPending(std::uintptr_t start, std::uintptr_t end, DWORD protection) {
        std::vector<std::pair<std::uintptr_t, PendingRange>> changed;
        auto it = pending.lower_bound(start);
        if (it != pending.begin() && std::prev(it)->second.end > start) --it;
        for (; it != pending.end() && it->first < end; ++it) {
            const auto from = std::max(start, it->first);
            const auto to = std::min(end, it->second.end);
            if (from < to) changed.emplace_back(from, PendingRange{to, protection, it->second.granule, it->second.watched});
        }
        for (const auto& [from, range] : changed) setPending(from, range.end, range);
    }

    void commitChunk(std::uintptr_t start, std::uintptr_t end, const PendingRange& range) {
        reset(start, end - start);
        const DWORD flags = MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | (range.watched ? MEM_WRITE_WATCH : 0);
        if (!allocate(GetCurrentProcess(), reinterpret_cast<void*>(start), end - start, flags, range.protection, nullptr, 0)) {
            const auto error = GetLastError();
            setPending(start, end, range);
            throw std::system_error(static_cast<int>(error), std::system_category(), "commit guest memory on first touch");
        }
        ++statistics.touchCommits;
        statistics.touchCommitBytes += end - start;
    }

    bool commitPendingAt(std::uintptr_t address) {
        const auto found = pendingAt(address);
        if (found == pending.end() || found->second.protection == PAGE_NOACCESS) return false;
        const auto start = found->first;
        const auto range = found->second;
        const auto chunk = start + (address - start) / range.granule * range.granule;
        commitChunk(chunk, std::min(range.end, chunk + range.granule), range);
        return true;
    }

    void commitPendingRange(std::uintptr_t start, std::uintptr_t end) {
        for (auto cursor = start; cursor < end;) {
            auto it = pending.upper_bound(cursor);
            if (it != pending.begin() && std::prev(it)->second.end > cursor) --it;
            if (it == pending.end() || it->first >= end) return;
            const auto first = it->first;
            const auto range = it->second;
            if (range.protection == PAGE_NOACCESS) {
                cursor = range.end;
                continue;
            }
            const auto from = std::max(cursor, first);
            const auto chunk = first + (from - first) / range.granule * range.granule;
            const auto chunkEnd = std::min(range.end, chunk + range.granule);
            commitChunk(chunk, chunkEnd, range);
            cursor = chunkEnd;
        }
    }

    static bool accessibleNow(std::uintptr_t address, std::uint32_t kind) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory)) return false;
        if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        const auto protection = memory.Protect & 0xffu;
        if (kind == 1) return writable(protection) || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_WRITECOPY;
        if (kind == 8) return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        return true;
    }

    void invalidate(SharedPage& page, std::uint8_t mask = allSubpages) {
        for (std::size_t sub = 0; sub < subpages; ++sub) {
            if ((mask & (1u << sub)) == 0) continue;
            ++page.generation[sub];
            for (const auto alias : page.aliases) forgetClean(alias + sub * subpageBytes, alias + (sub + 1) * subpageBytes);
        }
    }

    static std::uint8_t subpagesOf(std::uintptr_t base, std::uintptr_t address, std::uintptr_t end) {
        const auto from = std::max(address, base);
        const auto to = std::min(end, base + pageBytes);
        if (to <= from) return 0;
        const auto first = (from - base) / subpageBytes;
        const auto last = (to - 1 - base) / subpageBytes;
        return static_cast<std::uint8_t>(((1u << (last + 1)) - 1) & ~((1u << first) - 1));
    }

    static void protectSubpages(std::uintptr_t base, std::uint8_t mask, DWORD protection, const char* operation) {
        for (std::size_t sub = 0; sub < subpages;) {
            if ((mask & (1u << sub)) == 0) {
                ++sub;
                continue;
            }
            auto stop = sub;
            while (stop < subpages && (mask & (1u << stop)) != 0) ++stop;
            DWORD previous;
            if (!VirtualProtect(reinterpret_cast<void*>(base + sub * subpageBytes), (stop - sub) * subpageBytes, protection, &previous)) fail(operation);
            sub = stop;
        }
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

    WindowsMappings() : coalesce(std::getenv("APS5_PAGE_VIEWS") == nullptr) {
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

    void mapView(HANDLE section, std::uintptr_t address, std::uint64_t offset, std::size_t bytes) {
        if (!map(section, GetCurrentProcess(), reinterpret_cast<void*>(address), offset, bytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) fail("map shared guest page");
        ++statistics.viewsMapped;
        for (auto cursor = address; cursor < address + bytes;) {
            const auto memory = query(cursor);
            const auto stop = std::min(address + bytes, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.State == MEM_RESERVE && !VirtualAlloc(reinterpret_cast<void*>(cursor), stop - cursor, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) fail("commit shared guest page");
            cursor = stop;
        }
    }

    static std::uintptr_t allocationEnd(const MEMORY_BASIC_INFORMATION& memory) {
        auto end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        for (;;) {
            MEMORY_BASIC_INFORMATION part{};
            if (VirtualQuery(reinterpret_cast<const void*>(end), &part, sizeof(part)) != sizeof(part) || part.AllocationBase != memory.AllocationBase) return end;
            end = reinterpret_cast<std::uintptr_t>(part.BaseAddress) + part.RegionSize;
        }
    }

    static bool viewSplitAt(std::uintptr_t at, MEMORY_BASIC_INFORMATION& memory) {
        if (VirtualQuery(reinterpret_cast<const void*>(at), &memory, sizeof(memory)) != sizeof(memory)) return false;
        return memory.State == MEM_COMMIT && memory.Type == MEM_MAPPED && reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != at;
    }

    bool splitBusy(std::uintptr_t at) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!viewSplitAt(at, memory)) return false;
        const auto last = allocationEnd(memory);
        for (auto it = views.lower_bound(reinterpret_cast<std::uintptr_t>(memory.AllocationBase)); it != views.end() && it->first < last; ++it) {
            if (it->second.hostWrites != 0) return true;
        }
        return false;
    }

    void awaitHostWrites(std::unique_lock<std::mutex>& lock, std::uintptr_t start, std::uintptr_t end) {
        while (splitBusy(start) || splitBusy(end)) {
            lock.unlock();
            Sleep(1);
            lock.lock();
        }
    }

    void restoreProtection(std::uintptr_t start, std::uintptr_t end) {
        auto runStart = start;
        DWORD runProtection = 0;
        const auto flush = [&](std::uintptr_t stop) {
            DWORD previous;
            if (runStart < stop && !VirtualProtect(reinterpret_cast<void*>(runStart), stop - runStart, runProtection, &previous)) fail("restore a split shared guest view");
        };
        for (auto page = start; page < end; page += pageBytes) {
            const auto& view = views.at(page);
            if (view.armed == 0 && runStart < page && view.protection == runProtection) continue;
            flush(page);
            if (view.armed == 0) {
                runStart = page;
                runProtection = view.protection;
                continue;
            }
            protectSubpages(page, static_cast<std::uint8_t>(allSubpages & ~view.armed), view.protection, "restore a split shared guest view");
            protectSubpages(page, view.armed, view.protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY, "restore armed pages of a split shared guest view");
            runStart = page + pageBytes;
        }
        flush(end);
    }

    void splitView(std::uintptr_t at) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!viewSplitAt(at, memory)) return;
        const auto base = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
        const auto last = allocationEnd(memory);
        const auto head = views.find(base);
        if (head == views.end()) return;
        if (at % pageBytes != 0) throw std::runtime_error("cannot split a shared guest view between its pages");
        const auto section = head->second.section;
        const auto offset = head->second.offset;
        if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(base), MEM_PRESERVE_PLACEHOLDER)) fail("unmap a shared guest view to split it");
        if (!VirtualFree(reinterpret_cast<void*>(base), at - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split a shared guest view placeholder");
        mapView(section->handle, base, offset, at - base);
        mapView(section->handle, at, offset + (at - base), last - at);
        restoreProtection(base, last);
        ++statistics.viewsUnmapped;
        ++statistics.splits;
    }

    void reset(std::uintptr_t address, std::size_t bytes) {
        collectableRanges.clear();
        const auto end = address + bytes;
        forgetClean(address, end);
        forgetPending(address, end);
        splitView(address);
        splitView(end);
        for (auto cursor = address; cursor < end;) {
            const auto memory = query(cursor);
            if (memory.State == MEM_RESERVE) {
                cursor = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                continue;
            }
            if (reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != cursor) throw std::runtime_error("cannot release part of a host allocation");
            auto allocationEnd = cursor;
            do {
                const auto part = query(allocationEnd);
                if (part.AllocationBase != memory.AllocationBase) break;
                allocationEnd = reinterpret_cast<std::uintptr_t>(part.BaseAddress) + part.RegionSize;
            } while (allocationEnd < end);
            if (allocationEnd > end || query(allocationEnd).AllocationBase == memory.AllocationBase) throw std::runtime_error("guest release truncates a host allocation");
            if (memory.Type == MEM_MAPPED) {
                if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(cursor), MEM_PRESERVE_PLACEHOLDER)) fail("unmap shared guest page");
                ++statistics.viewsUnmapped;
                for (auto it = views.lower_bound(cursor); it != views.end() && it->first < allocationEnd;) {
                    std::erase(it->second.page->aliases, it->first);
                    it = views.erase(it);
                    ++statistics.pagesUnmapped;
                }
                forget(freshRanges, cursor, allocationEnd);
            } else if (memory.Type == MEM_PRIVATE) {
                if (!VirtualFree(reinterpret_cast<void*>(cursor), allocationEnd - cursor, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("release private guest memory");
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

    Ranges cleanRanges;
    Ranges collectableRanges;
    Ranges freshRanges;
    PendingRanges pending;
    Statistics statistics;
    void* mapAliasRuns(std::uintptr_t address, std::size_t bytes) {
        auto* reserved = allocate(GetCurrentProcess(), nullptr, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (reserved == nullptr) fail("reserve a read-write alias of shared guest memory");
        const auto base = reinterpret_cast<std::uintptr_t>(reserved);
        try {
            for (std::size_t done = 0; done < bytes;) {
                const auto& first = views.at(address + done);
                auto run = pageBytes;
                while (done + run < bytes) {
                    const auto& next = views.at(address + done + run);
                    if (next.offset != first.offset + run || (next.section != first.section && !sameSection(next.section->handle, first.section->handle))) break;
                    run += pageBytes;
                }
                split(base + done, run);
                if (!map(first.section->handle, GetCurrentProcess(), reinterpret_cast<void*>(base + done), first.offset, run, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) fail("map a run of a read-write alias of shared guest memory");
                done += run;
            }
        } catch (...) {
            releaseAlias(base, bytes);
            throw;
        }
        splitAliases.emplace(base, bytes);
        return reserved;
    }

    DWORD releaseAlias(std::uintptr_t base, std::size_t bytes) {
        DWORD error = ERROR_SUCCESS;
        for (auto cursor = base; cursor < base + bytes;) {
            const auto memory = query(cursor);
            const auto released = memory.State == MEM_RESERVE ? VirtualFree(memory.BaseAddress, 0, MEM_RELEASE) : unmap(GetCurrentProcess(), memory.BaseAddress, 0);
            if (!released && error == ERROR_SUCCESS) error = GetLastError();
            cursor = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        }
        return error;
    }

    std::map<std::uintptr_t, View> views;

    using Views = std::map<std::uintptr_t, View>;

    Views::iterator viewAt(std::uintptr_t base, Views::iterator& position) {
        if (position != views.end() && position->first < base) {
            ++position;
            if (position != views.end() && position->first < base) position = views.lower_bound(base);
        }
        return position != views.end() && position->first == base ? position : views.end();
    }
    std::map<std::pair<std::uintptr_t, std::uint64_t>, std::weak_ptr<SharedPage>> physical;
    std::map<std::uintptr_t, std::size_t> splitAliases;
    std::mutex mutex;
    AllocateFunction allocate = nullptr;
    MapFunction map = nullptr;
    UnmapFunction unmap = nullptr;
    CompareFunction compare = nullptr;
    bool coalesce = true;
};

}
#endif

#endif
