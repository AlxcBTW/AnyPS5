#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include <array>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace AgcDriver;
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

std::vector<std::uint32_t> withDevice(std::vector<std::uint32_t> key, std::uint32_t device) {
    key.push_back(device);
    key.push_back(0u);
    return key;
}

void testDispatchKeyWithDevice() {
    ContentKeyDiff diff;
    const auto base = withDevice(shaderKey(false, false, bindings()), 5);
    check(ShaderResources::DiffContentKeys(base, base, false, diff), "dispatch key with device words was not comparable");
    expectOnly(diff, std::nullopt, "equal dispatch keys with device words reported a difference");
    auto buffer = bindings();
    buffer[0].words[0] += 1;
    check(ShaderResources::DiffContentKeys(base, withDevice(shaderKey(false, false, buffer), 5), false, diff), "dispatch V# word 0 with device words");
    expectOnly(diff, ContentKeyComponent::BufferWord0, "dispatch V# word 0 with device words");
    check(ShaderResources::DiffContentKeys(base, withDevice(shaderKey(false, false, bindings()), 6), false, diff), "dispatch device change");
    expectOnly(diff, ContentKeyComponent::Other, "dispatch device change");
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

std::array<std::uint32_t, 4> bufferWords(std::uint64_t address, std::uint32_t stride, std::uint32_t records, std::uint32_t format) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, format};
}

struct AddressFreeShader {
    ShaderRecompiler::RecompileResult program;
    Graphics::CompiledShader shader{ShaderRecompiler::ShaderStage::Compute, &program, 0};

    AddressFreeShader(std::array<std::uint32_t, 4> readOnly, std::array<std::uint32_t, 4> written) {
        program.variantId = 0x1234;
        ShaderRecompiler::DescriptorBinding binding{ShaderRecompiler::DescriptorKind::StorageBuffer, DescriptorRole::GuestBuffers, 0, 3, 2, {}};
        binding.guestDescriptor.insert(binding.guestDescriptor.end(), readOnly.begin(), readOnly.end());
        binding.guestDescriptor.insert(binding.guestDescriptor.end(), written.begin(), written.end());
        binding.bufferWritten = {false, true};
        program.bindings.push_back(binding);
        ShaderRecompiler::DescriptorBinding data{ShaderRecompiler::DescriptorKind::StorageBuffer, DescriptorRole::ShaderData, 0, 4, 1, {40, 41, 42, 43}};
        program.bindings.push_back(data);
    }

    std::vector<std::uint32_t> key(bool addressFree) const { return ShaderResources::ContentKey(shader, false, false, addressFree); }
};

void testAddressFreeKey() {
    constexpr std::uint64_t Ring = 0x10'2000'0000ull;
    constexpr std::uint64_t Output = 0x10'3000'0000ull;
    const AddressFreeShader base(bufferWords(Ring, 16, 4096, 0x31016facu), bufferWords(Output, 0, 256, 0x31016facu));
    const auto same = [&](const AddressFreeShader& other, bool addressFree, const char* message) { check(base.key(addressFree) == other.key(addressFree), message); };
    const auto differs = [&](const AddressFreeShader& other, bool addressFree, const char* message) { check(base.key(addressFree) != other.key(addressFree), message); };
    const AddressFreeShader moved(bufferWords(Ring + 0x40100, 16, 4096, 0x31016facu), bufferWords(Output, 0, 256, 0x31016facu));
    same(moved, true, "a read-only buffer that moved changed the address-free key");
    differs(moved, false, "a read-only buffer that moved kept the keyed address");
    const AddressFreeShader movedHigh(bufferWords(Ring + (3ull << 32u), 16, 4096, 0x31016facu), bufferWords(Output, 0, 256, 0x31016facu));
    same(movedHigh, true, "the high address bits of a read-only buffer stayed in the key");
    const AddressFreeShader writtenMoved(bufferWords(Ring, 16, 4096, 0x31016facu), bufferWords(Output + 0x100, 0, 256, 0x31016facu));
    differs(writtenMoved, true, "a written buffer that moved kept the key");
    check(ShaderResources::AddressFreeKeyHash(base.shader, true) == ShaderResources::AddressFreeKeyHash(writtenMoved.shader, true), "the written-too census key kept the written address");
    check(ShaderResources::AddressFreeKeyHash(base.shader, false) != ShaderResources::AddressFreeKeyHash(writtenMoved.shader, false), "the read-only census key lost the written address");
    differs(AddressFreeShader(bufferWords(Ring, 16, 2048, 0x31016facu), bufferWords(Output, 0, 256, 0x31016facu)), true, "a read-only size change kept the key");
    differs(AddressFreeShader(bufferWords(Ring, 32, 4096, 0x31016facu), bufferWords(Output, 0, 256, 0x31016facu)), true, "a read-only stride change kept the key");
    differs(AddressFreeShader(bufferWords(Ring, 16, 4096, 0x31016fadu), bufferWords(Output, 0, 256, 0x31016facu)), true, "a read-only format change kept the key");
    differs(AddressFreeShader(bufferWords(0, 16, 4096, 0x31016facu), bufferWords(Output, 0, 256, 0x31016facu)), true, "a null read-only buffer met the key of a bound one");
    auto data = moved;
    data.program.bindings[1].guestDescriptor[0] += 1;
    data.shader.program = &data.program;
    same(data, true, "shader data words entered a template key");
    ContentKeyDiff diff;
    check(ShaderResources::DiffContentKeys(base.key(true), writtenMoved.key(true), false, diff), "address-free keys were not comparable");
    expectOnly(diff, ContentKeyComponent::BufferWord0, "a moved written buffer is charged to V# word 0");
    check(ShaderResources::DiffContentKeys(base.key(true), movedHigh.key(true), false, diff), "address-free keys with moved high bits were not comparable");
    expectOnly(diff, std::nullopt, "moved high address bits of a read-only buffer were charged");
}

}

int main() {
    try {
        testDispatchKey();
        testDispatchKeyWithDevice();
        testDrawKey();
        testAddressFreeKey();
        std::puts("Key miss attribution passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
