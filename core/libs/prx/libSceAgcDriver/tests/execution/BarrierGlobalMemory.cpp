#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 128;
constexpr std::uint32_t Groups = 64;
constexpr std::uint32_t Rounds = 4;
constexpr std::uint32_t Sentinel = 0xdeadbeefu;
alignas(256) std::array<std::uint32_t, Threads * Groups> Exchange{};
alignas(256) std::array<std::uint32_t, Threads * Groups> Output{};

alignas(256) constexpr std::array<std::uint32_t, 16> ExchangeCode{
    0x34020082u, 0x4c0400ffu, 0x0000007fu, 0x34040482u, 0x8f098908u, 0x4a060009u, 0xe0705000u, 0x09000301u,
    0xbbfd0000u, 0xbf8a0000u, 0xe0305000u, 0x09000402u, 0xbf8c3f70u, 0xe0701000u, 0x09010401u, 0xbf810000u,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

ShaderRecompiler::RecompileResult Translate(AgcDriver::VulkanDevice& device, std::uint32_t waveSize) {
    std::vector<std::uint32_t> userData(8, 0u);
    const auto exchange = BufferDescriptor(Exchange.data(), static_cast<std::uint32_t>(Exchange.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(exchange.begin(), exchange.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(ExchangeCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

void CheckBarrierSemantics(const ShaderRecompiler::RecompileResult& result, std::uint32_t waveSize) {
    const std::span<const std::uint32_t> words(result.spirv.data(), result.spirv.size());
    const std::string wave = "wave" + std::to_string(waveSize) + " ";
    std::map<std::uint32_t, std::uint32_t> constants;
    std::vector<std::array<std::uint32_t, 3>> barriers;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> 16u;
        const auto opcode = words[cursor] & 0xffffu;
        Require(count != 0 && count <= words.size() - cursor, wave + "barrier global memory: truncated SPIR-V instruction");
        if (opcode == spv::OpConstant && count == 4u) constants[words[cursor + 2]] = words[cursor + 3];
        if (opcode == spv::OpControlBarrier && count == 4u) barriers.push_back({words[cursor + 1], words[cursor + 2], words[cursor + 3]});
        cursor += count;
    }
    Require(!barriers.empty(), wave + "s_barrier emitted no OpControlBarrier");
    const auto value = [&](std::uint32_t id) {
        const auto found = constants.find(id);
        Require(found != constants.end(), wave + "an OpControlBarrier operand is not a 32-bit constant");
        return found->second;
    };
    const std::uint32_t required = spv::MemorySemanticsAcquireReleaseMask | spv::MemorySemanticsUniformMemoryMask | spv::MemorySemanticsWorkgroupMemoryMask | spv::MemorySemanticsImageMemoryMask;
    for (const auto& [execution, scope, semantics] : barriers) {
        Require(value(execution) == spv::ScopeWorkgroup && value(scope) == spv::ScopeWorkgroup, wave + "s_barrier is not a workgroup barrier over workgroup memory scope");
        Require((value(semantics) & required) == required, wave + "s_barrier semantics " + Hex(value(semantics)) + " do not order buffer, workgroup and image memory with acquire-release (" + Hex(required) + ")");
    }
}

void Run(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& result, std::uint32_t waveSize, std::uint32_t round) {
    Exchange.fill(Sentinel);
    Output.fill(Sentinel);
    device.Dispatch(result, Groups, 1, 1, {}, reinterpret_cast<std::uintptr_t>(ExchangeCode.data()));
    device.WaitIdle();
    for (std::uint32_t group = 0; group < Groups; ++group) {
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            const std::uint32_t index = group * Threads + tid;
            const std::uint32_t expected = group * Threads * 4u + (Threads - 1u - tid);
            const std::string what = "wave" + std::to_string(waveSize) + " round " + std::to_string(round) + " group " + std::to_string(group) + " thread " + std::to_string(tid);
            Require(Exchange[index] == group * Threads * 4u + tid, what + " stored " + Hex(Exchange[index]));
            Require(Output[index] == expected, what + " read " + Hex(Output[index]) + " after s_barrier, expected " + Hex(expected));
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (const std::uint32_t waveSize : {32u, 64u}) {
            const auto result = Translate(*device, waveSize);
            for (std::uint32_t round = 0; round < Rounds; ++round) Run(*device, result, waveSize, round);
            CheckBarrierSemantics(result, waveSize);
        }
        std::puts("barrier global memory tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
