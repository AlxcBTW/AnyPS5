#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SPECULATIVECAPTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SPECULATIVECAPTURE_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <span>

namespace AgcDriver::DriverDetail {

// APS5_SPECULATIVE_CAPTURE: a draw's shader capture reads the words that recorded GPU work still
// writes without waiting for that work, when they are only the leaves of pure flat-SRT slots (the
// walk never consumes them); the draw then copies those words into its flattened-SRT buffer on the
// GPU, behind the writer (Graphics::SrtFill). Off (unset, 0): every such read waits through the flush
// hook as before. On (1): speculate. Verify: speculate, then capture again the exact way (waiting)
// and compare the two captures outside the filled words; a difference is logged and counted
// (APS5_SPECULATIVE_CAPTURE_ABORT=1 aborts on it).
enum class SpeculativeMode : std::uint8_t { Off, On, Verify };
SpeculativeMode SpeculativeCaptureMode();
bool SpeculativeCaptureAbort();

// Why the query for a word answered Volatile (or, for Label and Sample, still Sync) with speculation
// on (WriteEvidence.cpp), for the counter line.
enum class VolatileReason : std::uint8_t { NoWriter, Foreign, LargeRange, EvidenceShort, WriterChanged, PendingImage, Shadow, Label, Sample, Count };

// Why a stage left speculation: its capture had to be repeated exactly, and its program skips
// speculation for a while (Speculation::Refuse).
enum class RefusalReason : std::uint8_t { NoPureSlots, PlanUnsafe, ConsumedByWalk, NotALeaf, FillsDontFit, Count };

struct SpeculationCounts {
    std::uint64_t captures = 0;        // stage captures that speculated
    std::uint64_t accepted = 0;        // ... that left words to the draw
    std::uint64_t fillWords = 0;       // ... and how many words
    std::uint64_t repeated = 0;        // ... that were repeated exactly (a speculated word was not only a pure leaf)
    std::uint64_t refusedStages = 0;   // captures of programs that were refused before
    std::uint64_t indirectCpu = 0;     // draws that turned out CPU-side indirect: their fills were read on the CPU
    std::uint64_t speculatedWords = 0; // word reads that skipped the flush hook
    std::uint64_t observedUnchanged = 0, observedChanged = 0; // evidence from completed speculations
    std::uint64_t verified = 0, verifyMismatches = 0, verifyStale = 0, verifyFresh = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(VolatileReason::Count)> volatileReads{};
    std::array<std::uint64_t, static_cast<std::size_t>(RefusalReason::Count)> refusals{};
    std::uint64_t partial = 0;         // captures that kept their pure leaves speculated and re-read only the consumed words
    // Draw captures' reads through the flush hook by kind (ShaderMemory::ReadKind), and how many waited.
    std::array<std::uint64_t, 5> hookReads{};
    std::array<std::uint64_t, 5> hookWaited{};
};

namespace Speculation {

// A program whose speculative capture failed skips speculation for its next captures (structural
// reasons for many, the others for a thousand), then tries again; the address reused by other code
// only costs that code its speculation for a while.
bool Refused(std::uint64_t program);
void Refuse(std::uint64_t program, RefusalReason reason);
void CountVolatile(VolatileReason reason);
void Count(std::uint64_t SpeculationCounts::*field, std::uint64_t amount = 1);
SpeculationCounts Take();

// Words a speculating capture read while their writer was pending: remembered with the bytes read,
// and compared once no recorded work writes them any more. The result is the evidence the
// speculated reads could not collect by waiting (Driver::observePendingWrite).
void Remember(std::span<const std::uint64_t> addresses, std::span<const std::uint32_t> values);
// Hands every remembered word that `pending` no longer reports to `finished` (address, bytes read).
void Resolve(const std::function<bool(std::uint64_t)>& pending, const std::function<void(std::uint64_t, std::uint32_t)>& finished);
// Every Nth Driver::resolveSpeculations call does the work.
constexpr std::uint32_t ResolveEvery = 32;

struct Remembered {
    std::uint64_t address;
    std::uint32_t value;
};

}

}

#endif
