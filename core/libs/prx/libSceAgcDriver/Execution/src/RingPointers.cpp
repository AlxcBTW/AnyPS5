#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libSceAgcDriver/Execution/include/RingPointers.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace AgcDriver::Ring {

namespace {

// Names, not calls: the shader disk cache key's guard reviews every recompiler call a driver
// source makes (ShaderCacheVersion.cmake), and this file makes none.
using IrOpcode = ShaderRecompiler::IrOpcode;
using IrValue = ShaderRecompiler::IrValue;
using MemoryRegion = ShaderRecompiler::MemoryRegion;
using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

constexpr std::uint64_t AddressLimit = 0x0001000000000000ull;

const std::byte* Find(std::span<const MemoryRegion> regions, std::uint64_t address) {
    // Sorted by address and disjoint: the last region starting at or below the address.
    auto it = std::upper_bound(regions.begin(), regions.end(), address, [](std::uint64_t value, const MemoryRegion& region) { return value < region.guestAddress; });
    if (it == regions.begin()) return nullptr;
    --it;
    if (address + sizeof(std::uint32_t) > it->guestAddress + it->bytes.size()) return nullptr;
    return it->bytes.data() + (address - it->guestAddress);
}

const std::byte* FindAny(std::span<const MemoryRegion> regions, std::uint64_t address) {
    for (const auto& region : regions) {
        if (address >= region.guestAddress && address + sizeof(std::uint32_t) <= region.guestAddress + region.bytes.size()) return region.bytes.data() + (address - region.guestAddress);
    }
    return nullptr;
}

bool Overlaps(std::span<const MemoryRegion> regions, std::uint64_t begin, std::uint64_t end) {
    for (const auto& region : regions) {
        if (begin < region.guestAddress + region.bytes.size() && region.guestAddress < end) return true;
    }
    return false;
}

// The second walk's memory: `own` at its addresses and, moved by `shift`, at the shifted ones;
// `initial` (code, headers) as is. Every read is noted with its class.
struct Served {
    std::span<const MemoryRegion> own;
    std::span<const MemoryRegion> initial;
    std::int64_t shift = 0;
    enum Kind : std::uint8_t { Absolute, Relative, Initial };
    std::vector<std::pair<std::uint64_t, Kind>> reads;
    bool failed = false;

    const std::byte* locate(std::uint64_t address, Kind& kind) const {
        const auto from = address - static_cast<std::uint64_t>(shift);
        if (const auto* bytes = Find(own, from)) {
            kind = Relative;
            return bytes;
        }
        if (const auto* bytes = Find(own, address)) {
            kind = Absolute;
            return bytes;
        }
        if (const auto* bytes = FindAny(initial, address)) {
            kind = Initial;
            return bytes;
        }
        return nullptr;
    }

    static bool Read(void* context, std::uint64_t address, std::uint32_t* value) {
        auto& served = *static_cast<Served*>(context);
        Kind kind = Absolute;
        const auto* bytes = served.locate(address, kind);
        if (bytes == nullptr) {
            served.failed = true;
            return false;
        }
        std::memcpy(value, bytes, sizeof(*value));
        served.reads.emplace_back(kind == Relative ? address - static_cast<std::uint64_t>(served.shift) : address, kind);
        return true;
    }

    static bool Accessible(void* context, std::uint64_t address, std::uint64_t bytes) {
        const auto& served = *static_cast<const Served*>(context);
        for (std::uint64_t offset = 0; offset < bytes; offset += sizeof(std::uint32_t)) {
            Kind kind = Absolute;
            if (served.locate((address + offset) & ~std::uint64_t{3}, kind) == nullptr) return false;
        }
        return true;
    }

};

std::uint32_t DwordAt(const std::vector<std::byte>& bytes, std::size_t index) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + index * sizeof(std::uint32_t), sizeof(value));
    return value;
}

// Where `moved` (materialized with the word at `shiftedValue`) differs from `compiled` (the word at
// `value`): only in push constants or shader-data words that hold the word itself.
bool DiffResults(const ShaderRecompiler::RecompileResult& compiled, const ShaderRecompiler::RecompileResult& moved, std::uint32_t value, std::uint32_t shiftedValue, std::uint32_t word, StageRecord& record) {
    using DescriptorRole = ShaderRecompiler::DescriptorRole;
    if (compiled.variantId != moved.variantId || compiled.specializationId != moved.specializationId || compiled.specialization.size() != moved.specialization.size()) return false;
    if (compiled.bindings.size() != moved.bindings.size() || compiled.pushConstants.size() != moved.pushConstants.size() || compiled.poisonedSrtReads != moved.poisonedSrtReads) return false;
    if (compiled.pushConstants.size() % sizeof(std::uint32_t) != 0) return compiled.pushConstants == moved.pushConstants;
    for (std::size_t d = 0; d < compiled.pushConstants.size() / sizeof(std::uint32_t); ++d) {
        const auto a = DwordAt(compiled.pushConstants, d);
        const auto b = DwordAt(moved.pushConstants, d);
        if (a == b) continue;
        if (a != value || b != shiftedValue) return false;
        record.push.emplace_back(static_cast<std::uint32_t>(d), word);
    }
    for (std::size_t i = 0; i < compiled.bindings.size(); ++i) {
        const auto& a = compiled.bindings[i];
        const auto& b = moved.bindings[i];
        if (a.kind != b.kind || a.role != b.role || a.binding != b.binding || a.count != b.count || a.guestDescriptor.size() != b.guestDescriptor.size()) return false;
        for (std::size_t w = 0; w < a.guestDescriptor.size(); ++w) {
            if (a.guestDescriptor[w] == b.guestDescriptor[w]) continue;
            if (a.role != DescriptorRole::ShaderData || a.guestDescriptor[w] != value || b.guestDescriptor[w] != shiftedValue) return false;
            record.bindingWords.push_back({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(w), word});
        }
    }
    return true;
}

void AppendRegionWords(std::uint64_t begin, std::span<const std::uint32_t> values, Runs& runs, std::vector<std::uint32_t>& words) {
    runs.emplace_back(begin, begin + values.size() * sizeof(std::uint32_t));
    words.insert(words.end(), values.begin(), values.end());
}

// Consecutive (address, word) pairs, address ordered, as runs.
void AppendWords(const std::vector<std::pair<std::uint64_t, std::uint32_t>>& pairs, Runs& runs, std::vector<std::uint32_t>& words) {
    for (std::size_t i = 0; i < pairs.size();) {
        std::size_t j = i + 1;
        while (j < pairs.size() && pairs[j].first == pairs[j - 1].first + sizeof(std::uint32_t)) ++j;
        runs.emplace_back(pairs[i].first, pairs[j - 1].first + sizeof(std::uint32_t));
        for (std::size_t k = i; k < j; ++k) words.push_back(pairs[k].second);
        i = j;
    }
}

// The walk's raw scalar read (Detail::IsRawRead, the evaluator's rule): an address or constant
// buffer load whose memory info says scalar address or scalar buffer.
bool RawRead(const ShaderRecompiler::IrResourcePlan& plan, const IrValue& value) {
    const auto opcode = value.Opcode();
    if (opcode != IrOpcode::LoadAddressU32 && opcode != IrOpcode::ReadConstBuffer) return false;
    const auto index = value.Flags<ShaderRecompiler::MemoryFlags>().index;
    if (index >= plan.memoryInfo.size()) return false;
    const auto kind = plan.memoryInfo[index].kind;
    return (opcode == IrOpcode::LoadAddressU32 && kind == ShaderRecompiler::ResourceKind::ScalarAddress) || (opcode == IrOpcode::ReadConstBuffer && kind == ShaderRecompiler::ResourceKind::ScalarBuffer);
}

struct MaskStore {
    std::shared_mutex mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<const Mask>> masks;
};

MaskStore& Store() {
    static MaskStore store;
    return store;
}

}

int Mode() {
    static const int mode = [] {
        const char* text = std::getenv("APS5_EXP_RING_POINTERS");
        return text == nullptr ? 0 : std::atoi(text);
    }();
    return mode;
}

bool Draws() { return Mode() >= 1; }

bool Dispatches() { return Mode() >= 2; }

std::uint64_t VerifyEvery() {
    static const std::uint64_t every = [] {
        const char* text = std::getenv("APS5_EXP_RING_VERIFY");
        return text == nullptr ? 0ull : std::strtoull(text, nullptr, 10);
    }();
    return every;
}

std::vector<std::uint32_t> ComputeCandidates(const ShaderRecompiler::IrResourcePlan& plan) {
    using UniformFillKind = ShaderRecompiler::UniformFillKind;
    std::vector<std::uint32_t> result;
    const bool disqualified = !plan.srtPlanComplete || plan.requiresSpecializationMemory
        || (plan.uniformFill.fill.kind != UniformFillKind::None && plan.uniformFill.fill.words > plan.uniformFill.values.size());
    if (disqualified || plan.srtReads.empty()) return result;
    // Every value the CPU walk evaluates, with its users (value, argument index); the slots a
    // ReadConst names lead to their raw reads as the walk's evaluator follows them.
    std::unordered_map<const IrValue*, std::vector<std::pair<const IrValue*, std::size_t>>> users;
    std::unordered_set<const IrValue*> roots;
    std::unordered_set<const IrValue*> seen;
    std::vector<const IrValue*> pending;
    const auto root = [&](const IrValue* value) {
        if (value == nullptr) return;
        value = value->Resolve();
        roots.insert(value);
        pending.push_back(value);
    };
    for (const auto& source : plan.descriptorSources) {
        for (std::uint32_t i = 0; i < source.dwordCount && i < source.dwords.size(); ++i) root(source.dwords[i]);
    }
    for (const auto& block : plan.controlFlow) root(block.condition);
    if (plan.uniformFill.fill.kind != UniformFillKind::None) {
        for (std::uint32_t i = 0; i < plan.uniformFill.fill.words; ++i) root(plan.uniformFill.values[i]);
    }
    for (const auto& read : plan.srtReads) {
        if (read.value == nullptr) continue;
        pending.push_back(read.value->Resolve());
    }
    std::vector<const IrValue*> userReads;
    while (!pending.empty()) {
        const IrValue* value = pending.back();
        pending.pop_back();
        if (!seen.insert(value).second) continue;
        if (value->Opcode() == IrOpcode::GetUserData) userReads.push_back(value);
        for (std::size_t i = 0; i < value->ArgumentCount(); ++i) {
            const IrValue* argument = value->Argument(i);
            if (argument == nullptr) continue;
            argument = argument->Resolve();
            users[argument].emplace_back(value, i);
            pending.push_back(argument);
        }
        if (value->Opcode() == IrOpcode::ReadConst && value->ArgumentCount() >= 2 && value->Argument(1) != nullptr) {
            const IrValue* slot = value->Argument(1)->Resolve();
            if (slot->HasImmediate() && slot->Type() == ShaderRecompiler::IrType::U32 && slot->ImmediateU32() < plan.srtReads.size() && plan.srtReads[slot->ImmediateU32()].value != nullptr) {
                pending.push_back(plan.srtReads[slot->ImmediateU32()].value->Resolve());
            }
        }
    }
    // Per word: whether every read of it qualifies, and whether one feeds a raw read at all.
    std::unordered_map<std::uint32_t, bool> qualifies;
    for (const IrValue* read : userReads) {
        if (read->ArgumentCount() == 0 || read->Argument(0) == nullptr) continue;
        // RegIndex(ScalarReg) is the register's number.
        const auto reg = read->Argument(0)->Register().index;
        if (reg < plan.userDataBase) continue;
        const auto word = reg - plan.userDataBase;
        bool good = !roots.contains(read);
        const auto found = users.find(read);
        if (found == users.end() || found->second.empty()) good = false;
        else {
            for (const auto& [handle, index] : found->second) {
                if (index != 0 || roots.contains(handle) || handle->ArgumentCount() < 2) {
                    good = false;
                    break;
                }
                const auto handleUsers = users.find(handle);
                if (handleUsers == users.end() || handleUsers->second.empty()) {
                    good = false;
                    break;
                }
                for (const auto& [raw, rawIndex] : handleUsers->second) {
                    if (rawIndex != 0 || !RawRead(plan, *raw)) {
                        good = false;
                        break;
                    }
                }
                if (!good) break;
            }
        }
        auto [entry, inserted] = qualifies.emplace(word, good);
        if (!inserted) entry->second = entry->second && good;
    }
    for (const auto& [word, good] : qualifies) {
        if (good) result.push_back(word);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::uint32_t> Candidates(const std::shared_ptr<const ShaderRecompiler::IrResourcePlan>& plan) {
    if (plan == nullptr) return {};
    struct Memo {
        std::mutex mutex;
        std::unordered_map<const ShaderRecompiler::IrResourcePlan*, std::pair<std::shared_ptr<const ShaderRecompiler::IrResourcePlan>, std::vector<std::uint32_t>>> plans;
    };
    static Memo memo;
    {
        std::lock_guard lock(memo.mutex);
        const auto found = memo.plans.find(plan.get());
        if (found != memo.plans.end()) return found->second.second;
    }
    auto words = ComputeCandidates(*plan);
    std::lock_guard lock(memo.mutex);
    // The memo pins its plans, so a key is never a freed plan's address.
    if (memo.plans.size() >= 16384) memo.plans.clear();
    memo.plans.emplace(plan.get(), std::make_pair(plan, words));
    return words;
}

std::optional<StageRecord> RecordStage(std::span<const std::uint32_t> words, std::span<const std::uint32_t> userData, std::uint64_t shaderBase, std::span<const MemoryRegion> own, std::span<const MemoryRegion> initial, const ShaderRecompiler::RecompileResult& compiled, const CaptureFunction& capture, const MaterializeFunction& materialize, RecordFailure* failure) {
    const auto fail = [&](RecordFailure reason) -> std::optional<StageRecord> {
        if (failure != nullptr) *failure = reason;
        return std::nullopt;
    };
    StageRecord record;
    std::size_t ownWords = 0;
    for (const auto& region : own) ownWords += region.bytes.size() / sizeof(std::uint32_t);
    record.classes.assign(ownWords, 0u);
    std::vector<std::uint32_t> absolute(ownWords, 0u);
    std::vector<std::uint32_t> shifted(userData.begin(), userData.end());
    for (const auto word : words) {
        if (word >= userData.size() || record.words.size() >= 30) continue;
        const auto value = userData[word];
        // A shift that moves no own dword onto an own or initial one.
        std::optional<std::int64_t> shift;
        std::uint32_t movedValue = 0;
        for (const std::uint32_t delta : {0x80000000u, 0x40000000u, 0xc0000000u, 0x20000000u, 0xe0000000u}) {
            const auto candidate = value + delta;
            const auto by = static_cast<std::int64_t>(candidate & ~3u) - static_cast<std::int64_t>(value & ~3u);
            bool clear = (candidate & ~3u) != 0;
            for (const auto& region : own) {
                if (!clear) break;
                const auto begin = region.guestAddress + static_cast<std::uint64_t>(by);
                const auto end = begin + region.bytes.size();
                clear = !Overlaps(own, begin, end) && !Overlaps(initial, begin, end);
            }
            if (clear) {
                shift = by;
                movedValue = candidate;
                break;
            }
        }
        if (!shift) return fail(RecordFailure::Overlap);
        Served served{own, initial, *shift, {}, false};
        shifted[word] = movedValue;
        ShaderRecompiler::SrtRuntime runtime;
        runtime.userData = shifted;
        runtime.shaderBase = shaderBase;
        runtime.userContext = &served;
        runtime.readMemory = &Served::Read;
        runtime.readSpecializationMemory = &Served::Read;
        runtime.accessible = &Served::Accessible;
        std::shared_ptr<const ShaderRecompiler::ResourceCapture> second;
        try {
            second = capture(runtime);
        } catch (...) {
            second = nullptr;
        }
        if (second == nullptr || served.failed || !second->snapshot.srtPoison.empty()) return fail(RecordFailure::Walk);
        const auto bit = 2u << record.words.size();
        bool relative = false;
        for (const auto& [address, kind] : served.reads) {
            if (kind == Served::Initial) continue;
            // The dword's index in `own`.
            std::size_t index = 0;
            bool found = false;
            for (const auto& region : own) {
                const auto count = region.bytes.size() / sizeof(std::uint32_t);
                if (address >= region.guestAddress && address < region.guestAddress + count * sizeof(std::uint32_t)) {
                    index += static_cast<std::size_t>((address - region.guestAddress) / sizeof(std::uint32_t));
                    found = true;
                    break;
                }
                index += count;
            }
            if (!found) return fail(RecordFailure::Walk);
            if (kind == Served::Relative) {
                record.classes[index] |= bit;
                relative = true;
            } else {
                absolute[index] |= 1u;
            }
        }
        std::shared_ptr<const ShaderRecompiler::RecompileResult> moved;
        try {
            moved = materialize(*second, shifted);
        } catch (...) {
            moved = nullptr;
        }
        shifted[word] = value;
        if (moved == nullptr) return fail(RecordFailure::Result);
        if (!DiffResults(compiled, *moved, value, movedValue, static_cast<std::uint32_t>(record.words.size()), record)) return fail(RecordFailure::Result);
        // A word nothing read through in this capture stays relative: it moves no run.
        static_cast<void>(relative);
        record.words.push_back({word, value, true});
        // A dword read absolutely is absolute only if every walk read it so.
        for (std::size_t i = 0; i < ownWords; ++i) {
            if (record.words.size() == 1) record.classes[i] |= absolute[i];
            else if ((absolute[i] & 1u) == 0) record.classes[i] &= ~1u;
            absolute[i] = 0;
        }
    }
    if (record.words.empty()) return fail(RecordFailure::Walk);
    for (const auto classes : record.classes) {
        if (classes == 0) return fail(RecordFailure::Diverged);
    }
    if (failure != nullptr) *failure = RecordFailure::None;
    return record;
}

void BuildRuns(const StageRecord* record, std::span<const std::uint32_t> leftOut, std::span<const std::uint32_t> userData, std::span<const MemoryRegion> own, std::span<const MemoryRegion> extra, Runs& runs, std::vector<std::uint32_t>& words, std::shared_ptr<const Layout>& layout) {
    runs.clear();
    words.clear();
    layout = nullptr;
    const auto plain = [&] {
        std::vector<MemoryRegion> regions(own.begin(), own.end());
        regions.insert(regions.end(), extra.begin(), extra.end());
        std::stable_sort(regions.begin(), regions.end(), [](const MemoryRegion& a, const MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
        for (const auto& region : regions) {
            const auto count = region.bytes.size() / sizeof(std::uint32_t);
            runs.emplace_back(region.guestAddress, region.guestAddress + count * sizeof(std::uint32_t));
            const auto offset = words.size();
            words.resize(offset + count);
            std::memcpy(words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        }
    };
    if (leftOut.empty()) {
        plain();
        return;
    }
    auto built = std::make_shared<Layout>();
    // Record word -> layout word, for the record's words the key left out.
    std::vector<std::int32_t> mapped(record != nullptr ? record->words.size() : 0u, -1);
    for (const auto word : leftOut) {
        std::int32_t recordIndex = -1;
        if (record != nullptr) {
            for (std::size_t w = 0; w < record->words.size(); ++w) {
                if (record->words[w].userWord == word) recordIndex = static_cast<std::int32_t>(w);
            }
        }
        const auto value = word < userData.size() ? userData[word] : 0u;
        if (recordIndex >= 0) mapped[static_cast<std::size_t>(recordIndex)] = static_cast<std::int32_t>(built->words.size());
        built->words.push_back({word, value, recordIndex >= 0});
    }
    std::vector<std::pair<std::uint64_t, std::uint32_t>> absolute;
    std::vector<std::vector<std::pair<std::uint64_t, std::uint32_t>>> relative(built->words.size());
    std::size_t index = 0;
    for (const auto& region : own) {
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        for (std::size_t i = 0; i < count; ++i, ++index) {
            std::uint32_t value = 0;
            std::memcpy(&value, region.bytes.data() + i * sizeof(std::uint32_t), sizeof(value));
            const auto address = region.guestAddress + i * sizeof(std::uint32_t);
            const auto classes = record != nullptr && index < record->classes.size() ? record->classes[index] : 1u;
            bool isAbsolute = (classes & 1u) != 0;
            for (std::size_t w = 0; record != nullptr && w < record->words.size(); ++w) {
                if ((classes & (2u << w)) == 0) continue;
                if (mapped[w] < 0) isAbsolute = true;
                else relative[static_cast<std::size_t>(mapped[w])].emplace_back(address, value);
            }
            if (isAbsolute) absolute.emplace_back(address, value);
        }
    }
    // The absolute group: own dwords and the extra regions, address ordered (extra ones may repeat
    // own dwords, as the plain layout's do).
    {
        Runs groupRuns;
        std::vector<std::uint32_t> groupWords;
        AppendWords(absolute, groupRuns, groupWords);
        struct Piece {
            std::uint64_t begin;
            std::span<const std::uint32_t> values;
        };
        std::vector<Piece> pieces;
        std::size_t offset = 0;
        for (const auto& [begin, end] : groupRuns) {
            const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
            pieces.push_back({begin, std::span<const std::uint32_t>(groupWords).subspan(offset, count)});
            offset += count;
        }
        std::vector<std::vector<std::uint32_t>> extraWords;
        extraWords.reserve(extra.size());
        for (const auto& region : extra) {
            const auto count = region.bytes.size() / sizeof(std::uint32_t);
            extraWords.emplace_back(count);
            std::memcpy(extraWords.back().data(), region.bytes.data(), count * sizeof(std::uint32_t));
        }
        for (std::size_t e = 0; e < extra.size(); ++e) pieces.push_back({extra[e].guestAddress, extraWords[e]});
        std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.begin < b.begin; });
        for (const auto& piece : pieces) AppendRegionWords(piece.begin, piece.values, runs, words);
        built->runWord.assign(runs.size(), 0u);
    }
    for (std::size_t w = 0; w < relative.size(); ++w) {
        AppendWords(relative[w], runs, words);
        built->runWord.resize(runs.size(), static_cast<std::uint8_t>(w + 1));
    }
    if (record != nullptr) {
        for (const auto& [dword, w] : record->push) {
            if (w < mapped.size() && mapped[w] >= 0) built->push.emplace_back(dword, static_cast<std::uint32_t>(mapped[w]));
        }
        for (const auto& entry : record->bindingWords) {
            if (entry[2] < mapped.size() && mapped[entry[2]] >= 0) built->bindingWords.push_back({entry[0], entry[1], static_cast<std::uint32_t>(mapped[entry[2]])});
        }
    }
    layout = std::move(built);
}

void DataPositions(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, const Layout* layout, const ShaderRecompiler::SrtReadTrace& trace, std::span<const std::uint32_t> flattenedSrt, std::vector<std::uint32_t>& positions, std::vector<std::uint32_t>& slots) {
    positions.clear();
    slots.clear();
    if (layout == nullptr) {
        static_cast<void>(DataWordPositions(runs, trace.leaves, trace.otherReads, words, flattenedSrt, positions, slots));
        return;
    }
    // Groups: consecutive runs with one runWord value.
    struct Group {
        std::size_t firstRun, endRun, firstWord;
    };
    std::vector<Group> groups;
    std::size_t wordOffset = 0;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        if (groups.empty() || layout->runWord[r] != layout->runWord[groups.back().firstRun]) groups.push_back({r, r, wordOffset});
        groups.back().endRun = r + 1;
        wordOffset += static_cast<std::size_t>((runs[r].second - runs[r].first) / sizeof(std::uint32_t));
    }
    const auto inGroup = [&](const Group& group, std::uint64_t address) {
        for (std::size_t r = group.firstRun; r < group.endRun; ++r) {
            if (address >= runs[r].first && address + sizeof(std::uint32_t) <= runs[r].second) return true;
        }
        return false;
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;
    for (const auto& group : groups) {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> leaves;
        for (const auto& leaf : trace.leaves) {
            if (!inGroup(group, leaf.second)) continue;
            const auto elsewhere = std::any_of(groups.begin(), groups.end(), [&](const Group& other) { return &other != &group && inGroup(other, leaf.second); });
            if (!elsewhere) leaves.push_back(leaf);
        }
        if (leaves.empty()) continue;
        std::size_t groupWords = 0;
        for (std::size_t r = group.firstRun; r < group.endRun; ++r) groupWords += static_cast<std::size_t>((runs[r].second - runs[r].first) / sizeof(std::uint32_t));
        std::vector<std::uint32_t> groupPositions, groupSlots;
        static_cast<void>(DataWordPositions(runs.subspan(group.firstRun, group.endRun - group.firstRun), leaves, trace.otherReads, words.subspan(group.firstWord, groupWords), flattenedSrt, groupPositions, groupSlots));
        for (std::size_t k = 0; k < groupPositions.size(); ++k) found.emplace_back(static_cast<std::uint32_t>(groupPositions[k] + group.firstWord), groupSlots[k]);
    }
    std::sort(found.begin(), found.end());
    for (const auto& [position, slot] : found) {
        positions.push_back(position);
        slots.push_back(slot);
    }
}

bool Rebase(const Layout& layout, std::span<const std::uint32_t> liveUserData, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::vector<std::pair<std::uint64_t, std::uint64_t>>& moved, bool& movedAny) {
    movedAny = false;
    std::array<std::int64_t, 32> deltas{};
    if (layout.words.size() > deltas.size()) return false;
    for (std::size_t w = 0; w < layout.words.size(); ++w) {
        const auto& word = layout.words[w];
        if (word.userWord >= liveUserData.size()) return false;
        const auto live = liveUserData[word.userWord];
        if (!word.relative) {
            if (live != word.value) return false;
            continue;
        }
        if ((live & ~3u) == 0) return false;
        deltas[w] = static_cast<std::int64_t>(live & ~3u) - static_cast<std::int64_t>(word.value & ~3u);
        if (deltas[w] != 0) movedAny = true;
    }
    if (!movedAny) return true;
    moved.assign(runs.begin(), runs.end());
    for (std::size_t r = 0; r < moved.size() && r < layout.runWord.size(); ++r) {
        const auto w = layout.runWord[r];
        if (w == 0) continue;
        const auto delta = deltas[w - 1u];
        if (delta == 0) continue;
        const auto begin = static_cast<std::int64_t>(moved[r].first) + delta;
        const auto end = static_cast<std::int64_t>(moved[r].second) + delta;
        if (begin <= 0 || end <= begin || static_cast<std::uint64_t>(end) > AddressLimit) return false;
        moved[r] = {static_cast<std::uint64_t>(begin), static_cast<std::uint64_t>(end)};
    }
    return true;
}

std::shared_ptr<ShaderRecompiler::RecompileResult> Patch(const ShaderRecompiler::RecompileResult& compiled, const Layout* layout, std::span<const std::uint32_t> liveUserData, std::uint32_t flatBinding, std::span<const std::uint32_t> dataPositions, std::span<const std::uint32_t> dataSlots, std::span<const std::uint32_t> liveWords) {
    auto patched = std::make_shared<ShaderRecompiler::RecompileResult>(compiled);
    if (flatBinding < patched->bindings.size()) {
        auto& descriptor = patched->bindings[flatBinding].guestDescriptor;
        for (std::size_t k = 0; k < dataPositions.size() && k < dataSlots.size(); ++k) {
            if (dataSlots[k] < descriptor.size() && dataPositions[k] < liveWords.size()) descriptor[dataSlots[k]] = liveWords[dataPositions[k]];
        }
    }
    if (layout != nullptr) {
        const auto live = [&](std::uint32_t w) { return w < layout->words.size() && layout->words[w].userWord < liveUserData.size() ? liveUserData[layout->words[w].userWord] : 0u; };
        for (const auto& [dword, w] : layout->push) {
            const auto value = live(w);
            if ((dword + 1u) * sizeof(std::uint32_t) <= patched->pushConstants.size()) std::memcpy(patched->pushConstants.data() + dword * sizeof(std::uint32_t), &value, sizeof(value));
        }
        for (const auto& [binding, word, w] : layout->bindingWords) {
            if (binding < patched->bindings.size() && word < patched->bindings[binding].guestDescriptor.size()) patched->bindings[binding].guestDescriptor[word] = live(w);
        }
    }
    return patched;
}

std::optional<std::uint32_t> UserWordRegister(ShaderRecompiler::ShaderStage stage, std::uint32_t firstUserSgpr, std::uint32_t userDataBase, std::uint32_t word) {
    // A merged stage (first user SGPR 0, not the pixel stage) carries 8 words in front.
    const std::uint32_t prefix = stage != ShaderRecompiler::ShaderStage::Fragment && firstUserSgpr == 0 ? 8u : 0u;
    if (word < prefix || word - prefix >= 32u) return std::nullopt;
    return userDataBase + (word - prefix);
}

std::optional<std::uint32_t> RegisterUserWord(ShaderRecompiler::ShaderStage stage, std::uint32_t firstUserSgpr, std::uint32_t userDataBase, std::size_t userWords, std::uint32_t reg) {
    const std::uint32_t prefix = stage != ShaderRecompiler::ShaderStage::Fragment && firstUserSgpr == 0 ? 8u : 0u;
    if (reg < userDataBase || reg - userDataBase >= 32u) return std::nullopt;
    const auto word = reg - userDataBase + prefix;
    if (word >= userWords) return std::nullopt;
    return word;
}

std::shared_ptr<const Mask> FindMask(std::uint64_t set) {
    auto& store = Store();
    std::shared_lock lock(store.mutex);
    const auto found = store.masks.find(set);
    return found == store.masks.end() ? nullptr : found->second;
}

void LearnMask(std::uint64_t set, std::vector<std::uint32_t> registers, std::vector<std::shared_ptr<const void>> pins) {
    auto& store = Store();
    std::unique_lock lock(store.mutex);
    auto& slot = store.masks[set];
    if (slot != nullptr && slot->frozen) return;
    auto mask = std::make_shared<Mask>();
    std::sort(registers.begin(), registers.end());
    registers.erase(std::unique(registers.begin(), registers.end()), registers.end());
    mask->attempts = slot != nullptr ? slot->attempts + 1u : 1u;
    mask->frozen = !registers.empty() || mask->attempts >= 8u;
    if (!registers.empty()) {
        ++Totals().masks;
        Totals().maskRegisters += registers.size();
    }
    mask->registers = std::move(registers);
    mask->pins = std::move(pins);
    slot = std::move(mask);
    if (store.masks.size() > 65536u) store.masks.clear();
}

DrawKeyState& ThisDraw() {
    struct DrawKeyTag {};
    return HostThreadLocal<DrawKeyState, DrawKeyTag>();
}

Counters& Totals() {
    static Counters counters;
    return counters;
}

std::string Report() {
    auto& t = Totals();
    char text[640];
    std::snprintf(text, sizeof(text), "[exp-ring] dispatches: keys masked %llu, recorded %llu, moved hits %llu; draws: keys masked %llu, sets with a mask %llu (%llu registers); stages recorded %llu (%llu words), exact %llu, record failures overlap %llu walk %llu diverged %llu result %llu; hits %llu (moved %llu, data %llu), rebase misses %llu, orphan inserts %llu, verified %llu",
        static_cast<unsigned long long>(t.dispatchKeysMasked.load()), static_cast<unsigned long long>(t.dispatchRecorded.load()), static_cast<unsigned long long>(t.dispatchMoved.load()), static_cast<unsigned long long>(t.keysMasked.load()), static_cast<unsigned long long>(t.masks.load()), static_cast<unsigned long long>(t.maskRegisters.load()), static_cast<unsigned long long>(t.recorded.load()), static_cast<unsigned long long>(t.recordWords.load()), static_cast<unsigned long long>(t.exactWords.load()),
        static_cast<unsigned long long>(t.failures[static_cast<std::size_t>(RecordFailure::Overlap)].load()), static_cast<unsigned long long>(t.failures[static_cast<std::size_t>(RecordFailure::Walk)].load()), static_cast<unsigned long long>(t.failures[static_cast<std::size_t>(RecordFailure::Diverged)].load()), static_cast<unsigned long long>(t.failures[static_cast<std::size_t>(RecordFailure::Result)].load()),
        static_cast<unsigned long long>(t.hits.load()), static_cast<unsigned long long>(t.moved.load()), static_cast<unsigned long long>(t.dataHits.load()), static_cast<unsigned long long>(t.rebaseMisses.load()), static_cast<unsigned long long>(t.orphans.load()), static_cast<unsigned long long>(t.verified.load()));
    return text;
}

}
