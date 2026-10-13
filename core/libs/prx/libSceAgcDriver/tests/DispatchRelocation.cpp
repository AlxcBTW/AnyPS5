// Address-free dispatch-cache entries (Execution/include/Driver/Dispatch/DispatchRelocation.hpp):
// relative runs, relocatable V# fields and their guards, the content key, the user-word and field
// rules, the patch, and a relocated hit against a fresh materialization.
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchRelocation.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AgcDriver::DriverDetail;
using ShaderRecompiler::DescriptorRole;
using ShaderRecompiler::DescriptorValue;
using ShaderRecompiler::MemoryRegion;
using ShaderRecompiler::RecompileResult;
using ShaderRecompiler::SrtUserDataUse;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++failures;
}

// Guest memory as the tests see it: dword address -> word.
struct FakeMemory {
    std::map<std::uint64_t, std::uint32_t> words;

    void put(std::uint64_t address, std::span<const std::uint32_t> values) {
        for (std::size_t i = 0; i < values.size(); ++i) words[address + i * 4] = values[i];
    }
    bool read(std::uint64_t address, std::span<std::uint32_t> out) const {
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto found = words.find(address + i * 4);
            if (found == words.end()) return false;
            out[i] = found->second;
        }
        return true;
    }
};

// A buffer V#: base (48 bits), stride, records, word 3.
DescriptorValue vsharp(std::uint64_t base, std::uint32_t stride, std::uint32_t records, std::uint32_t word3 = 0x00027facu) {
    DescriptorValue value;
    value.dwordCount = 4;
    value.dwords[0] = static_cast<std::uint32_t>(base);
    value.dwords[1] = (stride << 16u) | static_cast<std::uint32_t>((base >> 32u) & 0xffffu);
    value.dwords[2] = records;
    value.dwords[3] = word3;
    return value;
}

std::vector<std::uint32_t> words(const DescriptorValue& value) {
    return {value.dwords[0], value.dwords[1], value.dwords[2], value.dwords[3]};
}

RecompileResult compiledWith(std::vector<std::vector<std::uint32_t>> buffers, std::vector<std::uint32_t> shaderData = {}, std::vector<std::uint32_t> forwarded = {}) {
    RecompileResult result;
    for (auto& element : buffers) {
        ShaderRecompiler::DescriptorBinding binding{};
        binding.role = DescriptorRole::GuestBuffers;
        binding.count = 1;
        binding.binding = static_cast<std::uint32_t>(result.bindings.size());
        binding.guestDescriptor = std::move(element);
        result.bindings.push_back(std::move(binding));
    }
    if (!shaderData.empty()) {
        ShaderRecompiler::DescriptorBinding binding{};
        binding.role = DescriptorRole::ShaderData;
        binding.count = 1;
        binding.binding = static_cast<std::uint32_t>(result.bindings.size());
        binding.guestDescriptor = shaderData;
        result.bindings.push_back(std::move(binding));
        result.pushConstants.resize(shaderData.size() * sizeof(std::uint32_t));
        std::memcpy(result.pushConstants.data(), shaderData.data(), result.pushConstants.size());
    }
    result.shaderDataUserWords = std::move(forwarded);
    return result;
}

void testSplitRelativeRuns() {
    // Table A (8 words at 0x1000) read through user word 2; table B (4 words at 0x5000) read
    // through a pointer loaded from memory.
    std::vector<std::uint32_t> a(8, 7u), b(4, 9u);
    const std::vector<MemoryRegion> regions{{0x1000, std::as_bytes(std::span(a))}, {0x5000, std::as_bytes(std::span(b))}};
    SrtUserDataUse use;
    use.asBase = (1u << 2u) | (1u << 5u);
    for (std::uint64_t i = 0; i < 8; ++i) use.reads.emplace_back(0x1000 + i * 4, 2);
    for (std::uint64_t i = 0; i < 4; ++i) use.reads.emplace_back(0x5000 + i * 4, -1);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    std::vector<std::int8_t> bases;
    std::vector<std::uint32_t> stored;
    auto used = SplitRelativeRuns(regions, use, use.asBase, runs, bases, stored);
    check(used == (1u << 2u), "only user word 2 has relative runs");
    check(runs.size() == 2 && runs[0] == std::make_pair<std::uint64_t, std::uint64_t>(0x1000, 0x1020) && runs[1] == std::make_pair<std::uint64_t, std::uint64_t>(0x5000, 0x5010), "runs keep the regions");
    check(bases.size() == 2 && bases[0] == 2 && bases[1] == -1, "the first run is relative to user word 2, the second absolute");
    check(stored.size() == 12 && stored[0] == 7u && stored[8] == 9u, "words keep the regions' order");

    // Half of table A read through the pointer, half absolute: the run splits.
    use.reads.clear();
    for (std::uint64_t i = 0; i < 4; ++i) use.reads.emplace_back(0x1000 + i * 4, 2);
    for (std::uint64_t i = 4; i < 8; ++i) use.reads.emplace_back(0x1000 + i * 4, -1);
    used = SplitRelativeRuns(std::span(regions).first(1), use, use.asBase, runs, bases, stored);
    check(used == (1u << 2u) && runs.size() == 2 && bases[0] == 2 && bases[1] == -1 && runs[0].second == 0x1010, "a run splits where its origin changes");

    // A word read through the pointer and also absolutely: the pointer word stays exact.
    use.reads.emplace_back(0x1008, -1);
    used = SplitRelativeRuns(std::span(regions).first(1), use, use.asBase, runs, bases, stored);
    check(used == 0 && runs.size() == 1 && bases[0] == -1, "an aliased read keeps every run absolute");

    // An unaligned read through the pointer: the same.
    use.reads.clear();
    use.reads.emplace_back(0x1002, 2);
    used = SplitRelativeRuns(std::span(regions).first(1), use, use.asBase, runs, bases, stored);
    check(used == 0, "an unaligned read keeps the pointer word exact");
}

// One table at `table` (8 words: 2 data words, a V# at positions 2..5, 2 data words) read
// through user word 0; the V# names a buffer far away.
struct Scene {
    std::vector<std::uint32_t> user;
    std::vector<std::uint32_t> table;
    DescriptorValue buffer;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    std::vector<std::int8_t> bases;
    std::vector<std::uint32_t> stored;
    RecompileResult compiled;
    RelocationProfile profile;
};

Scene makeScene(std::uint32_t table, std::uint64_t base, std::uint32_t data = 0x1234u) {
    Scene scene;
    scene.user = {table, 0x13u, 0x77u};
    scene.buffer = vsharp(base, 16, 64);
    scene.table = {data, 0xabcdu};
    const auto v = words(scene.buffer);
    scene.table.insert(scene.table.end(), v.begin(), v.end());
    scene.table.push_back(0x5u);
    scene.table.push_back(0x6u);
    const std::vector<MemoryRegion> regions{{table, std::as_bytes(std::span(scene.table))}};
    SrtUserDataUse use;
    use.asBase = 1u;
    for (std::uint64_t i = 0; i < scene.table.size(); ++i) use.reads.emplace_back(table + i * 4, 0);
    scene.profile.userData = scene.user;
    scene.profile.pointerWords = SplitRelativeRuns(regions, use, use.asBase, scene.runs, scene.bases, scene.stored);
    scene.compiled = compiledWith({v}, {0x77u}, {2u});
    const std::vector<DescriptorValue> buffers{scene.buffer};
    FindBufferFields(scene.runs, scene.stored, scene.user, buffers, std::vector<std::uint32_t>{0xabcdu}, scene.compiled, scene.profile);
    scene.profile.forwarded = scene.compiled.shaderDataUserWords;
    return scene;
}

void testBufferFields() {
    const auto scene = makeScene(0x20001000u, 0x1340000000ull);
    check(scene.profile.relocatableBuffers == 1, "the V# copied verbatim from the table is relocatable");
    check(scene.profile.wordFields == std::vector<AddressField>{{2, BufferBaseLowMask}, {3, BufferBaseHighMask}}, "its word 0 and word 1 are masked fields");
    check(scene.profile.patches == std::vector<ElementPatch>{{0, 0, 2, false}}, "the guest-buffer element copying it is patched from position 2");
    check(scene.profile.maskPositions == std::vector<std::uint32_t>{2, 3}, "mask positions are the field positions");

    const auto refused = [](auto&& adjust, const char* what) {
        auto scene = makeScene(0x20001000u, 0x1340000000ull);
        adjust(scene);
        const std::vector<DescriptorValue> buffers{scene.buffer};
        RelocationProfile profile;
        profile.userData = scene.user;
        profile.pointerWords = scene.profile.pointerWords;
        FindBufferFields(scene.runs, scene.stored, scene.user, buffers, std::vector<std::uint32_t>{}, scene.compiled, profile);
        check(profile.relocatableBuffers == 0 && profile.wordFields.empty(), what);
    };
    refused([](Scene& s) { const std::vector<std::uint32_t> copy(s.stored.begin() + 2, s.stored.begin() + 6); s.stored.insert(s.stored.end(), copy.begin(), copy.end()); s.runs.emplace_back(0x30000000, 0x30000010); s.bases.push_back(-1); }, "a V# found twice stays exact");
    refused([](Scene& s) { s.runs.emplace_back(0x1340000020, 0x1340000024); s.stored.push_back(0); s.bases.push_back(-1); }, "a V# whose buffer the walk read stays exact");
    refused([](Scene& s) { s.compiled.bindings.back().guestDescriptor[0] = s.buffer.dwords[0]; }, "a V# base copied into an unforwarded shader-data slot stays exact");
    refused([](Scene& s) { s.compiled.bindings[0].role = DescriptorRole::BdaPagetable; }, "a program that reads raw addresses keeps every V# exact");
    refused([](Scene& s) { s.compiled.bindings.push_back(s.compiled.bindings[0]); s.compiled.bindings.back().guestDescriptor[2] = 8; }, "a descriptor sharing the base with other words stays exact");
    {
        auto scene = makeScene(0x20001000u, 0x1340000000ull);
        const std::vector<DescriptorValue> buffers{scene.buffer};
        RelocationProfile profile;
        profile.userData = scene.user;
        FindBufferFields(scene.runs, scene.stored, scene.user, buffers, std::vector<std::uint32_t>{scene.buffer.dwords[0]}, scene.compiled, profile);
        check(profile.wordFields.empty(), "a V# base in the flattened SRT stays exact");
        const std::vector<DescriptorValue> views{scene.buffer, vsharp(0x1340000100ull, 16, 4)};
        FindBufferFields(scene.runs, scene.stored, scene.user, views, std::vector<std::uint32_t>{}, scene.compiled, profile);
        check(profile.wordFields.empty(), "a V# with another descriptor starting inside it stays exact");
    }
    {
        // An inline V# in user words 4..7, forwarded into shader data.
        const auto value = vsharp(0x1500000040ull, 0, 256);
        std::vector<std::uint32_t> user{0x1000u, 0, 0, 0};
        const auto v = words(value);
        user.insert(user.end(), v.begin(), v.end());
        auto compiled = compiledWith({v}, {v[0], v[1]}, {4u, 5u});
        RelocationProfile profile;
        profile.userData = user;
        const std::vector<DescriptorValue> buffers{value};
        FindBufferFields({}, {}, user, buffers, std::vector<std::uint32_t>{}, compiled, profile);
        check(profile.userFields == std::vector<AddressField>{{4, BufferBaseLowMask}, {5, BufferBaseHighMask}} && profile.patches == std::vector<ElementPatch>{{0, 0, 4, true}}, "an inline V# in the user words is relocatable when only forwarded slots copy it");
    }
}

void testContentKey() {
    const auto first = makeScene(0x20001000u, 0x1340000000ull);
    const auto shape = ShapeOf(first.profile, first.runs, first.bases);
    check(shape.userMasks.size() == 3 && shape.userMasks[0] == PointerWordMask && shape.userMasks[1] == 0 && shape.reads.size() == 1 && shape.reads[0].offset == 0 && shape.reads[0].masks.size() == 8 && shape.reads[0].masks[2] == BufferBaseLowMask && shape.reads[0].masks[3] == BufferBaseHighMask, "the shape masks the pointer word and the V# base bits behind it");

    FakeMemory memory;
    const auto place = [&](const Scene& scene) { memory.put(AlignedPointer(scene.user[0]), scene.table); };
    const auto keyOf = [&](const Scene& scene) { return DispatchContentKey(0x42, shape, scene.user, [&](std::uint64_t address, std::span<std::uint32_t> out) { return memory.read(address, out); }); };
    const auto moved = makeScene(0x20008000u, 0x1380000000ull);
    const auto otherData = makeScene(0x20010000u, 0x1340000000ull, 0x9999u);
    place(first);
    place(moved);
    place(otherData);
    check(keyOf(first) == keyOf(moved), "a moved table with a moved V# keeps the content key");
    check(keyOf(first) != keyOf(otherData), "other words behind the pointer change the content key");
    auto user = first.user;
    user[2] = 0x78u;
    check(DispatchContentKey(0x42, shape, user, [&](std::uint64_t address, std::span<std::uint32_t> out) { return memory.read(address, out); }) != keyOf(first), "another value user word changes the content key");
    check(DispatchContentKey(0x43, shape, first.user, [&](std::uint64_t address, std::span<std::uint32_t> out) { return memory.read(address, out); }) != keyOf(first), "another identity changes the content key");
    auto stride = makeScene(0x20018000u, 0x1340000000ull);
    stride.table[3] = (32u << 16u) | 0x13u;
    place(stride);
    check(keyOf(stride) != keyOf(first), "another V# stride changes the content key");
}

void testFitAndPatch() {
    const auto first = makeScene(0x20001000u, 0x1340000000ull);
    const auto& profile = first.profile;
    auto user = first.user;
    user[0] = 0x20008000u;
    check(UserWordsFit(profile, user), "a moved pointer word fits");
    user[0] = 0x20008002u;
    check(!UserWordsFit(profile, user), "a pointer word with other low bits does not fit");
    user[0] = 0;
    check(!UserWordsFit(profile, user), "a null pointer word does not fit");
    user = first.user;
    user[2] = 0x78u;
    check(!UserWordsFit(profile, user), "another value user word does not fit");

    const std::vector<std::pair<std::uint32_t, std::uint32_t>> movedBase{{2, 0x80000000u}, {3, (16u << 16u) | 0x13u}};
    check(FieldsFit(profile, first.stored, movedBase), "a moved V# base fits");
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> otherStride{{3, (32u << 16u) | 0x13u}};
    check(!FieldsFit(profile, first.stored, otherStride), "another V# stride does not fit");
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> nullBase{{2, 0u}};
    check(!FieldsFit(profile, first.stored, nullBase), "a null V# base does not fit");
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> misaligned{{2, 0x80000001u}};
    check(!FieldsFit(profile, first.stored, misaligned), "a V# base with other low bits does not fit");
}

// The relocated hit end to end: the stored variant (first), the live dispatch (moved table, moved
// V#), the masked compare as validateCaptured does it, the patch, and the result a fresh capture
// materializes for the live dispatch.
void testRelocatedHit() {
    const auto first = makeScene(0x20001000u, 0x1340000000ull);
    const auto live = makeScene(0x20008000u, 0x1380000040ull);
    const auto& profile = first.profile;
    check(UserWordsFit(profile, live.user), "the live user words fit");
    std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
    bool differs = false;
    for (std::uint32_t position = 0; position < first.stored.size(); ++position) {
        if (first.stored[position] == live.stored[position]) continue;
        if (!std::binary_search(profile.maskPositions.begin(), profile.maskPositions.end(), position)) differs = true;
        else liveData.emplace_back(position, live.stored[position]);
    }
    check(!differs && FieldsFit(profile, first.stored, liveData), "only masked words differ, and they fit");
    auto liveWords = first.stored;
    for (const auto& [position, value] : liveData) liveWords[position] = value;
    auto patched = first.compiled;
    ApplyRelocation(patched, profile, liveWords, live.user);
    std::string why;
    check(SameMaterialization(patched, live.compiled, why), "the patched result binds what a fresh capture binds");
    if (!why.empty()) std::fprintf(stderr, "  (%s)\n", why.c_str());

    // Content that differs outside the masks: the compare misses.
    const auto other = makeScene(0x20008000u, 0x1380000040ull, 0x4242u);
    bool otherDiffers = false;
    for (std::uint32_t position = 0; position < first.stored.size(); ++position) {
        if (first.stored[position] != other.stored[position] && !std::binary_search(profile.maskPositions.begin(), profile.maskPositions.end(), position)) otherDiffers = true;
    }
    check(otherDiffers, "a data word that differs makes the variant differ");

    // The forwarded pointer word follows the live user word.
    auto forwardedPatch = first.compiled;
    auto pointerMoved = first.user;
    pointerMoved[0] = 0x20008000u;
    auto forwardingProfile = profile;
    forwardingProfile.forwarded = {0u};
    ApplyRelocation(forwardedPatch, forwardingProfile, first.stored, pointerMoved);
    std::uint32_t pushed = 0;
    std::memcpy(&pushed, forwardedPatch.pushConstants.data(), sizeof(pushed));
    check(pushed == 0x20008000u && forwardedPatch.bindings.back().guestDescriptor[0] == 0x20008000u, "a forwarded pointer word is patched into push constants and shader data");

    auto broken = live.compiled;
    broken.bindings[0].guestDescriptor[0] ^= 0x100u;
    check(!SameMaterialization(patched, broken, why) && !why.empty(), "a different descriptor word is reported");
}

// The SotS compute layout (sots/shader/*.req): an inline uniform-buffer V# in user words 8..11, a
// ring pointer in user words 12..13 to a table holding another V# whose 64-byte buffer ends right
// where the table starts (the same ring). Both V#s and the pointer relocate; the next dispatch, with
// everything moved by its own delta, keeps the content key and patches equal to a fresh result.
void testRingLayout() {
    const auto layout = [](std::uint64_t ring, std::uint64_t inlineBase) {
        Scene scene;
        const auto table = ring + 0x40;
        scene.buffer = vsharp(ring, 16, 4);
        const auto inlineV = vsharp(inlineBase, 16, 1, 0x0004dfacu);
        scene.user = {0x30683c00u, 0xc1600000u, 0x7fc7ffu, 0x91b44204u, 0u, 0x700060u, 0u, 0u};
        const auto v = words(inlineV);
        scene.user.insert(scene.user.end(), v.begin(), v.end());
        scene.user.push_back(static_cast<std::uint32_t>(table));
        scene.user.push_back(static_cast<std::uint32_t>(table >> 32u));
        scene.table = {0x30683c00u, 0xc1600000u, 0x7fc7ffu, 0x91b55204u};
        const auto t = words(scene.buffer);
        scene.table.insert(scene.table.end(), t.begin(), t.end());
        const std::vector<MemoryRegion> regions{{table, std::as_bytes(std::span(scene.table))}};
        SrtUserDataUse use;
        use.asBase = 1u << 12u;
        use.asValue = (0xfffu) | (1u << 13u);
        for (std::uint64_t i = 0; i < scene.table.size(); ++i) use.reads.emplace_back(table + i * 4, 12);
        scene.profile.userData = scene.user;
        scene.profile.pointerWords = SplitRelativeRuns(regions, use, use.asBase & ~use.asValue, scene.runs, scene.bases, scene.stored);
        scene.compiled = compiledWith({v, t});
        const std::vector<DescriptorValue> buffers{inlineV, scene.buffer};
        FindBufferFields(scene.runs, scene.stored, scene.user, buffers, std::vector<std::uint32_t>{}, scene.compiled, scene.profile);
        scene.profile.forwarded = scene.compiled.shaderDataUserWords;
        return scene;
    };
    const auto first = layout(0x31bbdcc020ull, 0x30542defd0ull);
    const auto& profile = first.profile;
    check(profile.pointerWords == (1u << 12u), "the ring pointer in user word 12 is relative");
    check(profile.relocatableBuffers == 2 && profile.userFields.size() == 2 && profile.wordFields.size() == 2, "the inline V# and the table's V# (buffer ending at the table) both relocate");
    const auto shape = ShapeOf(profile, first.runs, first.bases);
    check(shape.userMasks.size() == 14 && shape.userMasks[12] == PointerWordMask && shape.userMasks[8] == BufferBaseLowMask && shape.userMasks[9] == BufferBaseHighMask && shape.userMasks[13] == 0, "the shape leaves out the pointer and the inline V# base bits");

    const auto next = layout(0x31bbdcd000ull, 0x30542df000ull);
    FakeMemory memory;
    const auto tableOf = [](const Scene& scene) { return (static_cast<std::uint64_t>(scene.user[13]) << 32u) | scene.user[12]; };
    memory.put(tableOf(first), first.table);
    memory.put(tableOf(next), next.table);
    const auto read = [&](std::uint64_t address, std::span<std::uint32_t> out) { return memory.read(address, out); };
    check(DispatchContentKey(7, shape, first.user, read) == DispatchContentKey(7, shape, next.user, read), "the next dispatch keeps the content key");
    check(UserWordsFit(profile, next.user), "the next dispatch's user words fit");
    std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
    for (std::uint32_t position = 0; position < first.stored.size(); ++position) {
        if (first.stored[position] != next.stored[position]) liveData.emplace_back(position, next.stored[position]);
    }
    check(std::all_of(liveData.begin(), liveData.end(), [&](const auto& live) { return std::binary_search(profile.maskPositions.begin(), profile.maskPositions.end(), live.first); }) && FieldsFit(profile, first.stored, liveData), "only the table's V# base differs, and it fits");
    auto liveWords = first.stored;
    for (const auto& [position, value] : liveData) liveWords[position] = value;
    auto patched = first.compiled;
    ApplyRelocation(patched, profile, liveWords, next.user);
    std::string why;
    check(SameMaterialization(patched, next.compiled, why), "the patched result binds what a fresh capture of the next dispatch binds");
    auto otherImage = next.user;
    otherImage[3] = 0x91b66204u;
    check(!UserWordsFit(profile, otherImage), "another inline T# word does not fit");
}

}

int main() {
    try {
        testSplitRelativeRuns();
        testBufferFields();
        testContentKey();
        testFitAndPatch();
        testRelocatedHit();
        testRingLayout();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "exception: %s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("dispatch relocation: all checks passed\n");
    return 0;
}
