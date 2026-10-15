#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTCOMMITMETER_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTCOMMITMETER_HPP

#include "prx/libc/include/HostCommitLog.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

// Owner accounting for C++ containers in the general heap (APS5_HOST_COMMIT_LOG): the census walk
// sees them only as "host-private", so the owners count their bytes logically through
// HostCommitCount_nid_no_patch. Every helper checks one cached flag first and does nothing when the
// log is off.
namespace HostCommitLog {

inline bool Active() {
    static const bool active = HostCommitLogEnabled_nid_no_patch();
    return active;
}

inline void Count(const char* owner, std::int64_t bytes, std::int64_t allocations) {
    if (Active()) HostCommitCount_nid_no_patch(owner, bytes, allocations);
}

// The bytes and allocations one object holds for an owner, kept current with Set / Add and given
// back when the object dies. A copy starts empty (the copied container is not counted until the
// copy calls Set); a move takes the count along.
class Meter {
public:
    Meter() = default;
    explicit Meter(const char* owner) noexcept : owner(owner) {}
    Meter(const Meter& other) noexcept : owner(other.owner) {}
    Meter& operator=(const Meter& other) noexcept {
        if (this != &other && owner == nullptr) owner = other.owner;
        return *this;
    }
    Meter(Meter&& other) noexcept : owner(other.owner), bytes(other.bytes), allocations(other.allocations) {
        other.bytes = 0;
        other.allocations = 0;
    }
    Meter& operator=(Meter&& other) noexcept {
        if (this == &other) return *this;
        Release();
        if (other.owner != nullptr) owner = other.owner;
        bytes = other.bytes;
        allocations = other.allocations;
        other.bytes = 0;
        other.allocations = 0;
        return *this;
    }
    ~Meter() { Release(); }

    // Names the owner (once) and sets the absolute size.
    void Track(const char* name, std::size_t size, std::int64_t count = 1) noexcept {
        if (owner == nullptr) owner = name;
        Set(size, count);
    }
    void Set(std::size_t size, std::int64_t count = 1) noexcept {
        if (owner == nullptr || !Active()) return;
        const auto target = static_cast<std::int64_t>(size);
        const auto targetCount = size == 0 ? std::int64_t{0} : count;
        HostCommitCount_nid_no_patch(owner, target - bytes, targetCount - allocations);
        bytes = target;
        allocations = targetCount;
    }
    void Add(std::int64_t delta, std::int64_t count = 1) noexcept {
        if (owner == nullptr || !Active()) return;
        HostCommitCount_nid_no_patch(owner, delta, count);
        bytes += delta;
        allocations += count;
    }

private:
    void Release() noexcept {
        if (owner != nullptr && (bytes != 0 || allocations != 0) && Active()) HostCommitCount_nid_no_patch(owner, -bytes, -allocations);
        bytes = 0;
        allocations = 0;
    }

    const char* owner = nullptr;
    std::int64_t bytes = 0;
    std::int64_t allocations = 0;
};

// An allocator that counts the capacity of the vector it serves for one owner: growth, shrink and
// destruction keep the total current without touching the code that uses the vector. The owner
// travels with the memory (container copy, move and swap propagate it).
template<class T>
class CountingAllocator {
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    CountingAllocator() noexcept : owner("host-vector") {}
    explicit CountingAllocator(const char* owner) noexcept : owner(owner) {}
    template<class U>
    CountingAllocator(const CountingAllocator<U>& other) noexcept : owner(other.owner) {}

    T* allocate(std::size_t count) {
        T* memory = std::allocator<T>().allocate(count);
        Count(owner, static_cast<std::int64_t>(count * sizeof(T)), 1);
        return memory;
    }
    void deallocate(T* memory, std::size_t count) noexcept {
        std::allocator<T>().deallocate(memory, count);
        Count(owner, -static_cast<std::int64_t>(count * sizeof(T)), -1);
    }

    template<class U>
    bool operator==(const CountingAllocator<U>& other) const noexcept { return owner == other.owner; }

private:
    template<class U>
    friend class CountingAllocator;
    const char* owner;
};

template<class T>
using CountedVector = std::vector<T, CountingAllocator<T>>;

}

#endif
