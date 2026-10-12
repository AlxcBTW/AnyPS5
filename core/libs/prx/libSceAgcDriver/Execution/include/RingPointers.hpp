#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_RINGPOINTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_RINGPOINTERS_HPP

#include "Recompiler.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// exp (APS5_EXP_RING_POINTERS=1 draws, =2 draws and dispatches): ring-pointer keys.
//
// UE5 hands every draw a pointer into its transient uniform ring in one user-data word per stage,
// a few hundred bytes further on each draw, so draw keys that hash every user word almost never
// repeat and almost every draw captures and materializes its stages again. A user word the
// program uses only as the low address word of scalar reads' handles (Candidates: nothing the CPU
// walk evaluates sees its value otherwise) is left out of the draw key once every program of the
// draw that reads its register agrees. The variants under such a key record the reads through the
// word relative to it (RecordStage: the capture is walked a second time with the word moved by a
// large shift, the reads that moved are relative, and the second materialize shows which push
// constant and shader-data words carry the pointer). A lookup moves those runs by the live
// pointer's delta (Rebase), validates them with the pure flat slots masked (a data hit, as for
// dispatches), and draws with a copy of the compiled stage whose flattened slots and pointer words
// hold the live values (Patch). APS5_EXP_RING_VERIFY=N rebuilds every Nth such hit the old way and
// aborts on any difference.
namespace ShaderRecompiler {
struct IrResourcePlan;
struct ResourceCapture;
struct SrtReadTrace;
struct SrtRuntime;
}

namespace AgcDriver::Ring {

int Mode();
bool Draws();
bool Dispatches();
std::uint64_t VerifyEvery();

// The user-data words (indices into the request's userData) the plan reads only as the low word of
// raw scalar reads' handles: every GetUserData of the word is argument 0 of a handle that only raw
// reads (LoadAddressU32 / ReadConstBuffer, Detail::IsRawRead) use, as their argument 0, and no
// descriptor dword, condition or uniform-fill value is the word itself. A plan the pure-slot
// analysis disqualifies (incomplete SRT plan, specialization memory, image tables, an invalid
// uniform fill) has none. Candidates memoizes per plan.
std::vector<std::uint32_t> ComputeCandidates(const ShaderRecompiler::IrResourcePlan& plan);
std::vector<std::uint32_t> Candidates(const std::shared_ptr<const ShaderRecompiler::IrResourcePlan>& plan);

struct Word {
    std::uint32_t userWord = 0;
    std::uint32_t value = 0;
    bool relative = false;
};

// What the second walks taught about one stage: its relative words, per dword of its own read set
// (the regions the capture read, in their order) the classes (bit 0 read absolutely, bit 1 + w read
// through words[w]), and the push-constant dwords and shader-data binding words that carry a
// word's value.
struct StageRecord {
    std::vector<Word> words;
    std::vector<std::uint32_t> classes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> push;
    std::vector<std::array<std::uint32_t, 3>> bindingWords;
};

enum class RecordFailure : std::uint32_t { None, Overlap, Walk, Diverged, Result, Count };

using CaptureFunction = std::function<std::shared_ptr<const ShaderRecompiler::ResourceCapture>(const ShaderRecompiler::SrtRuntime&)>;
using MaterializeFunction = std::function<std::shared_ptr<const ShaderRecompiler::RecompileResult>(const ShaderRecompiler::ResourceCapture&, std::span<const std::uint32_t>)>;

// Records the stage relative to `words` (candidates the caller still wants): per word one more walk
// (`capture`, served only from `own` and `initial`, the moved word's reads from `own` at the
// unmoved address) and materialize. `own` is the stage capture's read set (sorted, disjoint),
// `initial` the regions the capture's memory held from the start (code, headers). Empty when a
// walk reads anything else, leaves one of `own`'s dwords unread, or the second result differs
// anywhere but in words equal to the pointer.
std::optional<StageRecord> RecordStage(std::span<const std::uint32_t> words, std::span<const std::uint32_t> userData, std::uint64_t shaderBase, std::span<const ShaderRecompiler::MemoryRegion> own, std::span<const ShaderRecompiler::MemoryRegion> initial, const ShaderRecompiler::RecompileResult& compiled, const CaptureFunction& capture, const MaterializeFunction& materialize, RecordFailure* failure = nullptr);

// What a cache variant keeps beyond its runs and words: the words left out of its key (relative
// ones, and exact ones a hit must repeat), per run the word it is relative to (0 absolute, w + 1
// relative to words[w]), and the pointer's push and shader-data words.
struct Layout {
    std::vector<Word> words;
    std::vector<std::uint8_t> runWord;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> push;
    std::vector<std::array<std::uint32_t, 3>> bindingWords;
};

// A variant's runs and words: `own` (the stage's reads) and `extra` (the vertex decode's), address
// ordered. With `leftOut` (the user words the key left out), a layout too: the own dwords `record`
// read through a left-out relative word go to runs of their own after the absolute ones; a
// left-out word without a relative record is exact.
void BuildRuns(const StageRecord* record, std::span<const std::uint32_t> leftOut, std::span<const std::uint32_t> userData, std::span<const ShaderRecompiler::MemoryRegion> own, std::span<const ShaderRecompiler::MemoryRegion> extra, std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs, std::vector<std::uint32_t>& words, std::shared_ptr<const Layout>& layout);

// The data positions (DataWordPositions) of a variant built by BuildRuns: per group of runs (the
// absolute ones, then each relative word's), a leaf read in more than one group is not data.
void DataPositions(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, const Layout* layout, const ShaderRecompiler::SrtReadTrace& trace, std::span<const std::uint32_t> flattenedSrt, std::vector<std::uint32_t>& positions, std::vector<std::uint32_t>& slots);

// The variant's runs moved by the live words' deltas into `moved` (only when one moved: `movedAny`).
// False when an exact word differs, a relative word is null or a run leaves the address space.
bool Rebase(const Layout& layout, std::span<const std::uint32_t> liveUserData, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::vector<std::pair<std::uint64_t, std::uint64_t>>& moved, bool& movedAny);

// A copy of `compiled` with the flattened slots of the data positions from `liveWords` and the
// layout's pointer words from the live user data.
std::shared_ptr<ShaderRecompiler::RecompileResult> Patch(const ShaderRecompiler::RecompileResult& compiled, const Layout* layout, std::span<const std::uint32_t> liveUserData, std::uint32_t flatBinding, std::span<const std::uint32_t> dataPositions, std::span<const std::uint32_t> dataSlots, std::span<const std::uint32_t> liveWords);
// The same patch written over `target`, a copy of the same compiled stage that earlier patches of
// these positions left otherwise unchanged (every data position and pointer word is rewritten).
void PatchInto(ShaderRecompiler::RecompileResult& target, const Layout* layout, std::span<const std::uint32_t> liveUserData, std::uint32_t flatBinding, std::span<const std::uint32_t> dataPositions, std::span<const std::uint32_t> dataSlots, std::span<const std::uint32_t> liveWords);

// A user word's shader register (the user-data block of its stage), none for the words in front of
// a merged stage's user SGPRs; and back.
std::optional<std::uint32_t> UserWordRegister(ShaderRecompiler::ShaderStage stage, std::uint32_t firstUserSgpr, std::uint32_t userDataBase, std::uint32_t word);
std::optional<std::uint32_t> RegisterUserWord(ShaderRecompiler::ShaderStage stage, std::uint32_t firstUserSgpr, std::uint32_t userDataBase, std::size_t userWords, std::uint32_t reg);

// The registers left out of the draw keys of one program set, learned from the first misses
// (LearnMask; an empty answer is retried a few times, then kept).
struct Mask {
    std::vector<std::uint32_t> registers;
    std::vector<std::shared_ptr<const void>> pins;
    std::uint32_t attempts = 0;
    bool frozen = false;
};
std::shared_ptr<const Mask> FindMask(std::uint64_t set);
void LearnMask(std::uint64_t set, std::vector<std::uint32_t> registers, std::vector<std::shared_ptr<const void>> pins);

// The calling thread's current draw: its program set and the mask its key used.
struct DrawKeyState {
    std::uint64_t set = 0;
    std::shared_ptr<const Mask> mask;
    std::vector<std::shared_ptr<const void>> pins;
    bool keyed = false;
};
DrawKeyState& ThisDraw();

struct Counters {
    std::atomic<std::uint64_t> recorded{0}, recordWords{0}, exactWords{0}, hits{0}, moved{0}, dataHits{0}, rebaseMisses{0}, verified{0}, masks{0}, maskRegisters{0}, orphans{0}, keysMasked{0};
    std::atomic<std::uint64_t> dispatchKeysMasked{0}, dispatchRecorded{0}, dispatchMoved{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(RecordFailure::Count)> failures{};
};
Counters& Totals();
std::string Report();

}

#endif
