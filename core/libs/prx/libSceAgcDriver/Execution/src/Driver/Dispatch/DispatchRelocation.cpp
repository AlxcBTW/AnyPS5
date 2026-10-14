#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchRelocation.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

namespace AgcDriver::DriverDetail {

namespace {

// The range a guard treats as a V#'s buffer: at least this many bytes (a null-sized V# still names
// its base), at most 4 GiB.
constexpr std::uint64_t MinGuardBytes = 64;
constexpr std::uint64_t MaxGuardBytes = std::uint64_t{1} << 32u;

std::uint64_t BufferBase(const ShaderRecompiler::DescriptorValue& value) {
    return (static_cast<std::uint64_t>(value.dwords[1] & 0xffffu) << 32u) | value.dwords[0];
}

std::uint64_t BufferBytes(const ShaderRecompiler::DescriptorValue& value) {
    const auto stride = (value.dwords[1] >> 16u) & 0x3fffu;
    const auto records = static_cast<std::uint64_t>(value.dwords[2]);
    return std::min(stride == 0u ? records : records * stride, MaxGuardBytes);
}

bool Present(const ShaderRecompiler::DescriptorValue& value) {
    return value.dwordCount == 4u && value.dwords[2] != 0u && (value.dwords[0] != 0u || (value.dwords[1] & 0xffffu) != 0u);
}

bool SameWords(std::span<const std::uint32_t> words, std::size_t at, const ShaderRecompiler::DescriptorValue& value) {
    for (std::uint32_t i = 0; i < 4u; ++i) {
        if (words[at + i] != value.dwords[i]) return false;
    }
    return true;
}

const AddressField* FieldAt(std::span<const AddressField> fields, std::uint32_t position) {
    const auto found = std::lower_bound(fields.begin(), fields.end(), position, [](const AddressField& field, std::uint32_t at) { return field.position < at; });
    return found != fields.end() && found->position == position ? &*found : nullptr;
}

bool FieldValueFits(const AddressField& field, std::uint32_t stored, std::uint32_t live) {
    if ((live & ~field.mask) != (stored & ~field.mask)) return false;
    return field.mask != BufferBaseLowMask || (live & field.mask) != 0u;
}

}

std::uint64_t SplitRelativeRuns(std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, std::uint64_t candidates, std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs, std::vector<std::int8_t>& runBases, std::vector<std::uint32_t>& words) {
    runs.clear();
    runBases.clear();
    words.clear();
    std::uint64_t relocatable = candidates;
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
    const auto covered = [&](std::uint64_t address) {
        return std::any_of(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return address >= region.guestAddress && address - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size(); });
    };
    if (relocatable != 0) {
        reads = use.reads;
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        const auto drop = [&](std::int32_t origin) {
            if (origin >= 0 && origin < 64) relocatable &= ~(std::uint64_t{1} << origin);
        };
        for (std::size_t i = 0; i < reads.size(); ++i) {
            if (i + 1 < reads.size() && reads[i].first == reads[i + 1].first) {
                drop(reads[i].second);
                drop(reads[i + 1].second);
            }
            if (reads[i].second >= 0 && ((reads[i].first & 3u) != 0 || !covered(reads[i].first))) drop(reads[i].second);
        }
    }
    const auto origin = [&](std::uint64_t address) -> std::int8_t {
        const auto found = std::lower_bound(reads.begin(), reads.end(), std::make_pair(address, std::numeric_limits<std::int32_t>::min()));
        if (found == reads.end() || found->first != address || found->second < 0 || found->second >= 64) return -1;
        return (relocatable >> found->second & 1u) != 0 ? static_cast<std::int8_t>(found->second) : std::int8_t{-1};
    };
    std::uint64_t used = 0;
    for (const auto& region : regions) {
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        const auto offset = words.size();
        words.resize(offset + count);
        std::memcpy(words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        if (relocatable == 0) {
            runs.emplace_back(region.guestAddress, region.guestAddress + count * sizeof(std::uint32_t));
            runBases.push_back(-1);
            continue;
        }
        std::size_t start = 0;
        auto current = count != 0 ? origin(region.guestAddress) : std::int8_t{-1};
        for (std::size_t word = 1; word <= count; ++word) {
            const auto next = word < count ? origin(region.guestAddress + word * sizeof(std::uint32_t)) : std::int8_t{-2};
            if (next == current) continue;
            runs.emplace_back(region.guestAddress + start * sizeof(std::uint32_t), region.guestAddress + word * sizeof(std::uint32_t));
            runBases.push_back(current);
            if (current >= 0) used |= std::uint64_t{1} << current;
            start = word;
            current = next;
        }
    }
    return used;
}

void FindBufferFields(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, std::span<const std::uint32_t> userData, std::span<const ShaderRecompiler::DescriptorValue> buffers, std::span<const std::uint32_t> flattened, const ShaderRecompiler::RecompileResult& compiled, RelocationProfile& profile) {
    profile.wordFields.clear();
    profile.userFields.clear();
    profile.patches.clear();
    profile.maskPositions.clear();
    profile.relocatableBuffers = 0;
    profile.refused = {};
    for (const auto& binding : compiled.bindings) {
        // The shader reaches memory through raw addresses (a BDA page table): keep every V# exact.
        if (binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable) {
            profile.refused[static_cast<std::size_t>(FieldRefusal::Bda)] += static_cast<std::uint32_t>(buffers.size());
            return;
        }
    }
    // Where each descriptor's four words occur verbatim (positions among the words, then user words).
    std::vector<std::vector<std::uint32_t>> sources(buffers.size()), userSources(buffers.size());
    // (word, position) of every position that starts four words inside one run, sorted.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> starts;
    std::size_t first = 0;
    for (const auto& [begin, end] : runs) {
        const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
        for (std::size_t i = 0; i + 4 <= count && first + i + 4 <= words.size(); ++i) starts.emplace_back(words[first + i], static_cast<std::uint32_t>(first + i));
        first += count;
    }
    std::sort(starts.begin(), starts.end());
    for (std::size_t r = 0; r < buffers.size(); ++r) {
        const auto& value = buffers[r];
        if (value.dwordCount != 4u) continue;
        const auto [from, to] = std::equal_range(starts.begin(), starts.end(), value.dwords[0], [](const auto& a, const auto& b) {
            if constexpr (std::is_same_v<std::decay_t<decltype(a)>, std::uint32_t>) return a < b.first;
            else return a.first < b;
        });
        for (auto it = from; it != to; ++it) {
            if (SameWords(words, it->second, value)) sources[r].push_back(it->second);
        }
        for (std::size_t i = 0; i + 4 <= userData.size(); ++i) {
            if (SameWords(userData, i, value)) userSources[r].push_back(static_cast<std::uint32_t>(i));
        }
    }
    std::vector<std::uint32_t> taken;
    std::vector<std::uint32_t> takenUser;
    for (std::size_t r = 0; r < buffers.size(); ++r) {
        const auto& value = buffers[r];
        if (!Present(value)) {
            ++profile.refused[static_cast<std::size_t>(FieldRefusal::NotPresent)];
            continue;
        }
        // One verbatim source, among the walked runs or the user data.
        const auto& positions = sources[r];
        const auto& userPositions = userSources[r];
        if (positions.size() + userPositions.size() != 1) {
            ++profile.refused[static_cast<std::size_t>(positions.empty() && userPositions.empty() ? FieldRefusal::NoSource : FieldRefusal::Sources)];
            continue;
        }
        const bool user = !userPositions.empty();
        const auto source = user ? userPositions.front() : positions.front();
        if ((user ? std::find(takenUser.begin(), takenUser.end(), source) != takenUser.end() : std::find(taken.begin(), taken.end(), source) != taken.end())) {
            ++profile.refused[static_cast<std::size_t>(FieldRefusal::Sources)];
            continue;
        }
        if (user && (source >= 60 || ((profile.pointerWords >> source) & 0xfu) != 0)) {
            ++profile.refused[static_cast<std::size_t>(FieldRefusal::Pointer)];
            continue;
        }
        const auto low = BufferBase(value);
        const auto high = low + std::max(BufferBytes(value), MinGuardBytes);
        // The walk read nothing inside the buffer: a read through the V# would follow its base.
        if (std::any_of(runs.begin(), runs.end(), [&](const auto& run) { return run.first < high && low < run.second; })) {
            ++profile.refused[static_cast<std::size_t>(FieldRefusal::Walked)];
            continue;
        }
        // No descriptor the walk built (found verbatim nowhere) starts inside it: a view it may have
        // derived from the base.
        bool derived = false;
        for (std::size_t other = 0; other < buffers.size() && !derived; ++other) {
            if (other == r || buffers[other].dwordCount != 4u || !sources[other].empty() || !userSources[other].empty()) continue;
            const auto otherBase = BufferBase(buffers[other]);
            derived = otherBase >= low && otherBase < high;
        }
        if (derived) {
            ++profile.refused[static_cast<std::size_t>(FieldRefusal::Derived)];
            continue;
        }
        // Nothing copies the base anywhere but the elements that copy the whole V# (and, for a
        // user source, the shader-data slots that forward its words).
        if (std::find(flattened.begin(), flattened.end(), value.dwords[0]) != flattened.end() || std::find(flattened.begin(), flattened.end(), value.dwords[1]) != flattened.end()) {
            ++profile.refused[static_cast<std::size_t>(FieldRefusal::Flattened)];
            continue;
        }
        std::vector<ElementPatch> patches;
        bool copied = false;
        for (std::uint32_t b = 0; b < compiled.bindings.size() && !copied; ++b) {
            const auto& binding = compiled.bindings[b];
            const auto& descriptor = binding.guestDescriptor;
            if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers && descriptor.size() % 4u == 0u) {
                for (std::uint32_t e = 0; e + 4 <= descriptor.size(); e += 4) {
                    if (SameWords(descriptor, e, value)) patches.push_back({b, e, source, user});
                    else if (descriptor[e] == value.dwords[0]) copied = true;
                }
                continue;
            }
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData) {
                for (std::uint32_t slot = 0; slot < descriptor.size(); ++slot) {
                    if (descriptor[slot] != value.dwords[0] && descriptor[slot] != value.dwords[1]) continue;
                    const bool forwardedHere = user && slot < compiled.shaderDataUserWords.size() && (compiled.shaderDataUserWords[slot] == source || compiled.shaderDataUserWords[slot] == source + 1);
                    if (!forwardedHere) copied = true;
                }
                continue;
            }
            if (std::find(descriptor.begin(), descriptor.end(), value.dwords[0]) != descriptor.end()) copied = true;
        }
        for (std::size_t at = 0; at + sizeof(std::uint32_t) <= compiled.pushConstants.size() && !copied; at += sizeof(std::uint32_t)) {
            std::uint32_t word = 0;
            std::memcpy(&word, compiled.pushConstants.data() + at, sizeof(word));
            if (word != value.dwords[0] && word != value.dwords[1]) continue;
            const auto slot = at / sizeof(std::uint32_t);
            const bool forwardedHere = user && slot < compiled.shaderDataUserWords.size() && (compiled.shaderDataUserWords[slot] == source || compiled.shaderDataUserWords[slot] == source + 1);
            if (!forwardedHere) copied = true;
        }
        if (copied) {
            ++profile.refused[static_cast<std::size_t>(FieldRefusal::Copied)];
            continue;
        }
        (user ? takenUser : taken).push_back(source);
        auto& fields = user ? profile.userFields : profile.wordFields;
        fields.push_back({source, BufferBaseLowMask});
        fields.push_back({source + 1, BufferBaseHighMask});
        profile.patches.insert(profile.patches.end(), patches.begin(), patches.end());
        ++profile.relocatableBuffers;
    }
    const auto byPosition = [](const AddressField& a, const AddressField& b) { return a.position < b.position; };
    std::sort(profile.wordFields.begin(), profile.wordFields.end(), byPosition);
    std::sort(profile.userFields.begin(), profile.userFields.end(), byPosition);
    for (const auto& field : profile.wordFields) profile.maskPositions.push_back(field.position);
}

DispatchShape ShapeOf(const RelocationProfile& profile, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::int8_t> runBases) {
    DispatchShape shape;
    if (profile.pointerWords == 0 && profile.userFields.empty()) return shape;
    shape.userMasks.assign(profile.userData.size(), 0u);
    for (std::size_t i = 0; i < profile.userData.size() && i < 64; ++i) {
        if ((profile.pointerWords >> i & 1u) != 0) shape.userMasks[i] = PointerWordMask;
    }
    for (const auto& field : profile.userFields) {
        if (field.position < shape.userMasks.size()) shape.userMasks[field.position] |= field.mask;
    }
    std::size_t position = 0;
    std::size_t budget = ContentKeyMaxWords;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        const auto count = static_cast<std::size_t>((runs[r].second - runs[r].first) / sizeof(std::uint32_t));
        const auto base = r < runBases.size() ? runBases[r] : std::int8_t{-1};
        if (base >= 0 && static_cast<std::size_t>(base) < profile.userData.size() && budget != 0) {
            DispatchShape::Read read;
            read.word = static_cast<std::uint32_t>(base);
            read.offset = static_cast<std::int64_t>(runs[r].first) - static_cast<std::int64_t>(AlignedPointer(profile.userData[read.word]));
            const auto take = std::min(count, std::min<std::size_t>(budget, 64));
            budget -= take;
            read.masks.assign(take, 0u);
            for (std::size_t i = 0; i < take; ++i) {
                if (const auto* field = FieldAt(profile.wordFields, static_cast<std::uint32_t>(position + i))) read.masks[i] = field->mask;
            }
            shape.reads.push_back(std::move(read));
        }
        position += count;
    }
    return shape;
}

bool UserWordsFit(const RelocationProfile& profile, std::span<const std::uint32_t> live) {
    if (live.size() != profile.userData.size()) return false;
    for (std::size_t i = 0; i < live.size(); ++i) {
        const auto stored = profile.userData[i];
        if (live[i] == stored) continue;
        if (i < 64 && (profile.pointerWords >> i & 1u) != 0) {
            if (live[i] == 0u || (live[i] & 3u) != (stored & 3u)) return false;
            continue;
        }
        const auto* field = FieldAt(profile.userFields, static_cast<std::uint32_t>(i));
        if (field == nullptr || !FieldValueFits(*field, stored, live[i])) return false;
    }
    return true;
}

bool FieldsFit(const RelocationProfile& profile, std::span<const std::uint32_t> stored, std::span<const std::pair<std::uint32_t, std::uint32_t>> live) {
    for (const auto& [position, value] : live) {
        const auto* field = FieldAt(profile.wordFields, position);
        if (field == nullptr) continue;
        if (position >= stored.size() || !FieldValueFits(*field, stored[position], value)) return false;
    }
    return true;
}

void ApplyRelocation(ShaderRecompiler::RecompileResult& result, const RelocationProfile& profile, std::span<const std::uint32_t> liveWords, std::span<const std::uint32_t> liveUser) {
    for (const auto& patch : profile.patches) {
        const auto& source = patch.user ? liveUser : liveWords;
        if (patch.binding >= result.bindings.size() || patch.source + 1 >= source.size()) continue;
        auto& descriptor = result.bindings[patch.binding].guestDescriptor;
        if (patch.word + 1 >= descriptor.size()) continue;
        descriptor[patch.word] = source[patch.source];
        descriptor[patch.word + 1] = source[patch.source + 1];
    }
    for (std::uint32_t slot = 0; slot < profile.forwarded.size(); ++slot) {
        const auto word = profile.forwarded[slot];
        if (word >= liveUser.size() || word >= profile.userData.size() || liveUser[word] == profile.userData[word]) continue;
        const auto value = liveUser[word];
        const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
        if (at + sizeof(std::uint32_t) <= result.pushConstants.size()) std::memcpy(result.pushConstants.data() + at, &value, sizeof(value));
        for (auto& binding : result.bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) binding.guestDescriptor[slot] = value;
        }
    }
}

bool SameMaterialization(const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b, std::string& why) {
    char text[160];
    if (a.variantId != b.variantId || a.specializationId != b.specializationId || a.spirv.size() != b.spirv.size()) {
        std::snprintf(text, sizeof(text), "module (variant %llx/%llx, specialization %llx/%llx)", static_cast<unsigned long long>(a.variantId), static_cast<unsigned long long>(b.variantId), static_cast<unsigned long long>(a.specializationId), static_cast<unsigned long long>(b.specializationId));
        why = text;
        return false;
    }
    if (a.bindings.size() != b.bindings.size()) {
        why = "binding count";
        return false;
    }
    for (std::size_t i = 0; i < a.bindings.size(); ++i) {
        const auto& left = a.bindings[i];
        const auto& right = b.bindings[i];
        if (left.role != right.role || left.count != right.count || left.binding != right.binding || left.guestDescriptor.size() != right.guestDescriptor.size()) {
            std::snprintf(text, sizeof(text), "binding %zu shape", i);
            why = text;
            return false;
        }
        for (std::size_t w = 0; w < left.guestDescriptor.size(); ++w) {
            if (left.guestDescriptor[w] == right.guestDescriptor[w]) continue;
            std::snprintf(text, sizeof(text), "binding %zu (role %d) word %zu: %08x vs %08x", i, static_cast<int>(left.role), w, left.guestDescriptor[w], right.guestDescriptor[w]);
            why = text;
            return false;
        }
    }
    if (a.pushConstants != b.pushConstants) {
        why = "push constants";
        return false;
    }
    return true;
}

}
