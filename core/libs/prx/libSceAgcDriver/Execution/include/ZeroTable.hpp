#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ZEROTABLE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ZEROTABLE_HPP

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver {

template<typename TValue>
class ZeroTable {
public:
    static constexpr std::size_t ChunkBytes = 4096;
    static constexpr std::size_t ChunkEntries = ChunkBytes / sizeof(TValue);
    static_assert(ChunkBytes % sizeof(TValue) == 0, "table entries must tile a chunk");

    ZeroTable() = default;
    ZeroTable(const ZeroTable&) = delete;
    ZeroTable& operator=(const ZeroTable&) = delete;

    ~ZeroTable() {
        release();
    }

    bool Allocate(std::size_t entries) {
        release();
        if (entries == 0) return false;
        const auto chunkCount = (entries + ChunkEntries - 1) / ChunkEntries;
#ifdef _WIN32
        static const bool eager = std::getenv("APS5_EAGER_HOST_TABLES") != nullptr;
        values = static_cast<TValue*>(VirtualAlloc(nullptr, chunkCount * ChunkBytes, eager ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE, PAGE_READWRITE));
        if (values == nullptr) return false;
        committed = std::make_unique<std::atomic<bool>[]>(chunkCount);
        for (std::size_t chunk = 0; chunk < chunkCount; ++chunk) committed[chunk].store(eager, std::memory_order_relaxed);
#else
        values = static_cast<TValue*>(std::calloc(chunkCount * ChunkEntries, sizeof(TValue)));
        if (values == nullptr) return false;
#endif
        count = entries;
        chunks = chunkCount;
        return true;
    }

    std::size_t Size() const {
        return count;
    }

    const void* Data() const {
        return values;
    }

    std::size_t Bytes() const {
        return chunks * ChunkBytes;
    }

    static std::size_t ChunkEnd(std::size_t index) {
        return (index / ChunkEntries + 1) * ChunkEntries;
    }

    bool Committed(std::size_t index) const {
#ifdef _WIN32
        return committed[index / ChunkEntries].load(std::memory_order_acquire);
#else
        static_cast<void>(index);
        return true;
#endif
    }

    const TValue* Raw() const {
        return values;
    }

    TValue Get(std::size_t index) const {
        return Committed(index) ? values[index] : TValue{};
    }

    TValue& At(std::size_t index) {
#ifdef _WIN32
        const auto chunk = index / ChunkEntries;
        if (!committed[chunk].load(std::memory_order_acquire)) {
            std::lock_guard lock(commitLock);
            if (!committed[chunk].load(std::memory_order_relaxed)) {
                if (VirtualAlloc(reinterpret_cast<std::byte*>(values) + chunk * ChunkBytes, ChunkBytes, MEM_COMMIT, PAGE_READWRITE) == nullptr) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "commit a host table chunk");
                committed[chunk].store(true, std::memory_order_release);
            }
        }
#endif
        return values[index];
    }

    TValue Max(std::size_t first, std::size_t end) const {
        TValue result{};
        for (auto index = first; index < end;) {
            const auto stop = std::min(end, ChunkEnd(index));
            if (Committed(index)) {
                for (; index < stop; ++index) result = std::max(result, values[index]);
            }
            index = stop;
        }
        return result;
    }

private:
    void release() {
        if (values == nullptr) return;
#ifdef _WIN32
        VirtualFree(values, 0, MEM_RELEASE);
        committed.reset();
#else
        std::free(values);
#endif
        values = nullptr;
        count = 0;
        chunks = 0;
    }

    TValue* values = nullptr;
    std::size_t count = 0;
    std::size_t chunks = 0;
#ifdef _WIN32
    std::unique_ptr<std::atomic<bool>[]> committed;
    std::mutex commitLock;
#endif
};

}

#endif
