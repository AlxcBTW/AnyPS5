#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRegisterKey.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace AgcDriver;
using namespace AgcDriver::DriverDetail;
using Graphics::ContentKeyComponent;
using Graphics::ContentKeyDiff;
using Graphics::ShaderResources;
using ShaderRecompiler::DescriptorRole;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Binding {
    DescriptorRole role;
    std::vector<std::uint32_t> words;
    std::uint32_t count = 1;
};

std::vector<std::uint32_t> shaderKey(bool dataWords, bool movable, const std::vector<Binding>& bindings) {
    std::vector<std::uint32_t> key{dataWords ? 1u : 0u, 0u, 7u, 0u, static_cast<std::uint32_t>(bindings.size())};
    for (const auto& binding : bindings) {
        const bool data = binding.role == DescriptorRole::ShaderData || binding.role == DescriptorRole::FlattenedSrt;
        for (const auto word : {0u, static_cast<std::uint32_t>(binding.role), 0u, 0u, binding.count, 0u, 0u, static_cast<std::uint32_t>(binding.words.size())}) key.push_back(word);
        const bool emitted = (movable && binding.role == DescriptorRole::GuestBuffers && binding.words.size() == binding.count * 4u) || (dataWords && !movable) || !data;
        if (emitted) key.insert(key.end(), binding.words.begin(), binding.words.end());
        for (int flags = 0; flags < 9; ++flags) key.push_back(0u);
        key.push_back(0u);
    }
    return key;
}

std::vector<std::uint32_t> drawKey(std::uint32_t device, const std::vector<std::uint32_t>& stage, std::uint32_t target) {
    std::vector<std::uint32_t> key{0xffffffffu, device, 0u, 1u, static_cast<std::uint32_t>(stage.size())};
    key.insert(key.end(), stage.begin(), stage.end());
    key.push_back(1u);
    for (const auto word : {target, 0u, 0x100u, 0u, 0u, 0u, 0u, 0u}) key.push_back(word);
    return key;
}

std::vector<Binding> bindings() {
    return {{DescriptorRole::GuestBuffers, {10, 11, 12, 13}}, {DescriptorRole::GuestImages, {20, 21, 22, 23, 24, 25, 26, 27}}, {DescriptorRole::GuestSamplers, {30, 31, 32, 33}}, {DescriptorRole::ShaderData, {40, 41, 42, 43}}};
}

void expectOnly(const ContentKeyDiff& diff, std::optional<ContentKeyComponent> component, const char* message) {
    for (std::size_t i = 0; i < diff.size(); ++i) {
        check(diff[i] == (component && i == static_cast<std::size_t>(*component)), message);
    }
}

void testDispatchKey() {
    ContentKeyDiff diff;
    const auto base = shaderKey(true, false, bindings());
    check(ShaderResources::DiffContentKeys(base, base, false, diff), "equal keys were not comparable");
    expectOnly(diff, std::nullopt, "equal keys reported a difference");
    const auto change = [&](std::size_t binding, std::size_t word, ContentKeyComponent expected, const char* message) {
        auto changed = bindings();
        changed[binding].words[word] += 1;
        check(ShaderResources::DiffContentKeys(base, shaderKey(true, false, changed), false, diff), message);
        expectOnly(diff, expected, message);
    };
    change(0, 0, ContentKeyComponent::BufferWord0, "V# word 0");
    change(0, 1, ContentKeyComponent::BufferWord1, "V# word 1");
    change(0, 2, ContentKeyComponent::BufferWord2, "V# word 2");
    change(0, 3, ContentKeyComponent::BufferWord3, "V# word 3");
    change(1, 5, ContentKeyComponent::Image, "image descriptor word");
    change(2, 1, ContentKeyComponent::Sampler, "sampler word");
    change(3, 2, ContentKeyComponent::ShaderData, "shader data word");
    auto two = bindings();
    two[0].words[0] += 1;
    two[0].words[3] += 1;
    check(ShaderResources::DiffContentKeys(base, shaderKey(true, false, two), false, diff), "two components");
    check(diff[static_cast<std::size_t>(ContentKeyComponent::BufferWord0)] && diff[static_cast<std::size_t>(ContentKeyComponent::BufferWord3)] && !diff[static_cast<std::size_t>(ContentKeyComponent::BufferWord1)], "two components in one miss are both counted");
    auto fewer = bindings();
    fewer.pop_back();
    check(!ShaderResources::DiffContentKeys(base, shaderKey(true, false, fewer), false, diff), "different shapes were compared");
    expectOnly(diff, ContentKeyComponent::Layout, "shape change is a layout change");
    const auto noData = shaderKey(false, false, bindings());
    auto changedData = bindings();
    changedData[3].words[0] += 1;
    check(ShaderResources::DiffContentKeys(noData, shaderKey(false, false, changedData), false, diff), "data words left out");
    expectOnly(diff, std::nullopt, "data words are not part of the key without dataWords");
}

void testDrawKey() {
    ContentKeyDiff diff;
    const auto base = drawKey(5, shaderKey(true, true, bindings()), 0x1000);
    const auto compare = [&](const std::vector<std::uint32_t>& key, std::optional<ContentKeyComponent> expected, const char* message) {
        check(ShaderResources::DiffContentKeys(base, key, true, diff), message);
        expectOnly(diff, expected, message);
    };
    compare(drawKey(5, shaderKey(true, true, bindings()), 0x2000), ContentKeyComponent::DrawTarget, "draw target");
    compare(drawKey(6, shaderKey(true, true, bindings()), 0x1000), ContentKeyComponent::Other, "device");
    auto buffer = bindings();
    buffer[0].words[0] += 1;
    compare(drawKey(5, shaderKey(true, true, buffer), 0x1000), ContentKeyComponent::BufferWord0, "draw V# word 0");
    auto data = bindings();
    data[3].words[0] += 1;
    compare(drawKey(5, shaderKey(true, true, data), 0x1000), std::nullopt, "movable draw keys leave data words out");
}

std::uint32_t firstOffset(Graphics::RegisterBank bank) {
    for (const auto& range : Graphics::DrawKeyRegisters) {
        if (range.bank != bank) continue;
        for (auto offset = range.first; offset < range.first + range.count; ++offset) {
            const bool runtime = offset - 0x00cu < 32u || offset - 0x08cu < 32u || offset - 0x10cu < 32u || offset == 0x082u || offset == 0x083u || offset == 0x102u || offset == 0x103u;
            if (!runtime) return offset;
        }
    }
    throw std::runtime_error("no key register in bank");
}

void testRegisterKey() {
    QueueState base;
    base.shader[0x8b] = 1u << 1u;
    base.shader[0x8c] = 5;
    const auto parts = RegisterStateParts(base);
    const auto only = [&](const QueueState& changed, RegisterKeyPart expected, const char* message) {
        RegisterKeyMissTracker tracker;
        tracker.Note(1, parts, true);
        tracker.Note(1, RegisterStateParts(changed), true);
        const auto counts = tracker.Take();
        check(counts.misses == 2 && counts.noCandidate == 1 && counts.identical == 0, message);
        for (std::size_t i = 0; i < counts.differing.size(); ++i) check(counts.differing[i] == (i == static_cast<std::size_t>(expected) ? 1u : 0u), message);
    };
    auto context = base;
    context.context[firstOffset(Graphics::RegisterBank::Context)] = 9;
    only(context, RegisterKeyPart::Context, "context register");
    auto shader = base;
    shader.shader[firstOffset(Graphics::RegisterBank::Shader)] = 9;
    only(shader, RegisterKeyPart::ShaderRegisters, "shader register");
    auto user = base;
    user.shader[0x8c] = 6;
    only(user, RegisterKeyPart::UserData, "user-data word");
    auto config = base;
    config.userConfig[firstOffset(Graphics::RegisterBank::UserConfig)] = 9;
    only(config, RegisterKeyPart::UserConfig, "user-config register");

    RegisterKeyMissTracker tracker;
    auto shaderChanged = parts;
    shaderChanged[static_cast<std::size_t>(RegisterKeyPart::Shader)] = 77;
    tracker.Note(2, parts, false);
    tracker.Note(2, shaderChanged, true);
    tracker.Note(2, shaderChanged, true);
    tracker.Note(3, parts, true);
    const auto counts = tracker.Take();
    check(counts.misses == 3 && counts.noCandidate == 1 && counts.identical == 1 && counts.differing[static_cast<std::size_t>(RegisterKeyPart::Shader)] == 1, "shader part and identical parts");
    check(tracker.Take().misses == 0, "window not reset");
}

}

int main() {
    try {
        testDispatchKey();
        testDrawKey();
        testRegisterKey();
        std::puts("Key miss attribution passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
