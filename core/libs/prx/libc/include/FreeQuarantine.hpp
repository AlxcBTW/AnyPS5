#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_FREEQUARANTINE_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_FREEQUARANTINE_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

// Crash hunt (exp, never push): the FIFO behind APS5_EXP_FREE_QUARANTINE=<seconds>[,<MiB cap>] (glue in
// src/specifics/windows/FreeQuarantine.cpp). Blocks the guest frees wait here and go to the real free oldest
// first: once they are <seconds> old, or earlier while the held bytes exceed the cap or the caller reports
// memory pressure. Plain data without locks or clocks, so it can be tested on its own; the caller locks it,
// passes the time in, and calls the real free for what TakeDue returns after unlocking.
namespace FreeQuarantine {

constexpr std::uint64_t DefaultCapMiB = 512;

struct Entry {
    std::uint64_t pointer = 0;
    std::uint64_t poolId = 0;     // handed back to the real free unchanged
    std::uint64_t bytes = 0;
    std::uint64_t heldAt = 0;     // microseconds, same clock as TakeDue's `now`
    std::uint64_t caller = 0;     // return address of the free call
    std::uint64_t contentHash = 0;
};

enum class Release : std::uint8_t { Age, Cap, Pressure };

struct Taken {
    Entry entry;
    std::uint64_t age = 0;
    Release why = Release::Age;
};

class Fifo {
public:
    Fifo(std::uint64_t holdMicros, std::uint64_t capBytes) : hold_(holdMicros), cap_(capBytes) {}

    void Push(const Entry& entry) {
        if (count_ == ring_.size()) Grow();
        ring_[(head_ + count_) % ring_.size()] = entry;
        ++count_;
        bytes_ += entry.bytes;
    }

    // Moves up to `capacity` due entries, oldest first, into `out` and returns how many. The oldest entry is due
    // when it is at least the hold time old, while the held bytes exceed the cap (0 = no cap), or while `pressure`
    // is set. A younger entry never leaves before an older one.
    std::size_t TakeDue(std::uint64_t now, bool pressure, Taken* out, std::size_t capacity) {
        std::size_t taken = 0;
        while (taken < capacity && count_ != 0) {
            const Entry& front = ring_[head_];
            const std::uint64_t age = now > front.heldAt ? now - front.heldAt : 0;
            Release why;
            if (age >= hold_) why = Release::Age;
            else if (cap_ != 0 && bytes_ > cap_) why = Release::Cap;
            else if (pressure) why = Release::Pressure;
            else break;
            out[taken].entry = front;
            out[taken].age = age;
            out[taken].why = why;
            ++taken;
            bytes_ -= front.bytes;
            head_ = (head_ + 1) % ring_.size();
            --count_;
        }
        return taken;
    }

    std::size_t Count() const { return count_; }
    std::uint64_t Bytes() const { return bytes_; }
    std::uint64_t HoldMicros() const { return hold_; }
    std::uint64_t CapBytes() const { return cap_; }
    std::uint64_t OldestAge(std::uint64_t now) const {
        if (count_ == 0) return 0;
        const std::uint64_t heldAt = ring_[head_].heldAt;
        return now > heldAt ? now - heldAt : 0;
    }

private:
    void Grow() {
        std::vector<Entry> larger(ring_.empty() ? 4096 : ring_.size() * 2);
        for (std::size_t i = 0; i < count_; ++i) larger[i] = ring_[(head_ + i) % ring_.size()];
        ring_.swap(larger);
        head_ = 0;
    }

    std::vector<Entry> ring_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t hold_;
    std::uint64_t cap_;
};

// "<seconds>[,<MiB cap>]": seconds > 0 (fractions allowed), cap in MiB (0 = no cap, default DefaultCapMiB).
inline bool ParseSpec(const char* text, std::uint64_t& holdMicros, std::uint64_t& capBytes) {
    if (text == nullptr || *text == 0) return false;
    char* end = nullptr;
    const double seconds = std::strtod(text, &end);
    if (end == text || !(seconds > 0.0) || seconds > 1e7) return false;
    std::uint64_t capMiB = DefaultCapMiB;
    if (*end == ',') {
        const char* capText = end + 1;
        if (*capText < '0' || *capText > '9') return false;
        capMiB = std::strtoull(capText, &end, 10);
        if (capMiB > (1ull << 30)) return false;
    }
    if (*end != 0) return false;
    holdMicros = static_cast<std::uint64_t>(seconds * 1e6);
    capBytes = capMiB << 20;
    return true;
}

// Windows glue only: whether one of the traced addresses (APS5_EXP_FREE_QUARANTINE_TRACE) lies in a block held
// right now. Lock-free, so a write-watch exception handler may ask.
bool HoldsTraced(std::uint64_t address);

}  // namespace FreeQuarantine

#endif
