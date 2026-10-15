#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Ops = 16;
constexpr std::uint32_t Slots = Ops + 1;
constexpr std::uint32_t ChunkB = 4096;
alignas(256) std::array<std::uint32_t, 65536 * Slots> Output{};

std::uint32_t Vop3(std::uint32_t op, std::uint32_t vdst, std::uint32_t abs = 0, std::uint32_t opsel = 0, bool clamp = false) {
    return 0xd4000000u | (op << 16u) | (clamp ? 0x8000u : 0u) | (opsel << 11u) | (abs << 8u) | vdst;
}
std::uint32_t Src(std::uint32_t src0, std::uint32_t src1, std::uint32_t src2 = 0x80u, std::uint32_t neg = 0) {
    return (neg << 29u) | (src2 << 18u) | (src1 << 9u) | src0;
}
std::uint32_t Vop3p(std::uint32_t op, std::uint32_t vdst, std::uint32_t opsel, std::uint32_t negHi, bool clamp) {
    return 0xcc000000u | (op << 16u) | (clamp ? 0x8000u : 0u) | 0x4000u | (opsel << 11u) | (negHi << 8u) | vdst;
}
std::uint32_t Vop3pSrc(std::uint32_t src0, std::uint32_t src1, std::uint32_t opselHi, std::uint32_t neg) {
    return (neg << 29u) | (opselHi << 27u) | (0x80u << 18u) | (src1 << 9u) | src0;
}
std::uint32_t Vop3pSrc3(std::uint32_t src0, std::uint32_t src1, std::uint32_t src2, std::uint32_t opselHi, std::uint32_t neg) {
    return (neg << 29u) | (opselHi << 27u) | (src2 << 18u) | (src1 << 9u) | src0;
}
constexpr std::uint32_t V(std::uint32_t index) { return 0x100u + index; }

std::vector<std::uint32_t> Program(bool packed) {
    std::vector<std::uint32_t> code;
    const auto add = [&](std::initializer_list<std::uint32_t> words) { code.insert(code.end(), words); };
    add({Vop3(0x36f, 1), Src(0x08, 0x88, V(0))});
    add({0x3a0402ffu, 0x00005a5au});
    add({Vop3(0x36f, 3), Src(V(2), 0x90, V(1))});
    for (std::uint32_t k = 0; k < Ops; ++k) add({0x7e000280u | ((20u + k) << 17u)});
    add({0xbe8a0304u});
    add({0xbe8d03ffu, 0x01000193u});
    add({0x8f0c9006u, 0x880c060cu});
    const auto loop = static_cast<std::uint32_t>(code.size());
    add({0x8f0b900au, 0x880b0a0bu});
    const std::uint32_t s10 = 0x0a, s11 = 0x0b, s12 = 0x0c;
    if (packed) {
        add({Vop3p(0x0f, 4, 0, 0, false), Vop3pSrc(V(3), s11, 3, 0)});
        add({Vop3p(0x10, 5, 0, 0, false), Vop3pSrc(V(3), s11, 3, 0)});
        add({Vop3p(0x0e, 6, 0, 0, false), Vop3pSrc3(V(3), s11, s12, 3, 0)});
        add({Vop3p(0x0f, 7, 1, 1, false), Vop3pSrc(V(3), s11, 2, 2)});
        add({Vop3p(0x10, 8, 0, 2, false), Vop3pSrc(V(3), s11, 3, 1)});
        add({Vop3p(0x0f, 9, 0, 0, true), Vop3pSrc(V(3), s11, 3, 0)});
        add({Vop3p(0x10, 10, 0, 0, true), Vop3pSrc(V(3), s11, 3, 0)});
        add({Vop3p(0x0e, 11, 0, 0, true), Vop3pSrc3(V(3), s11, s12, 3, 0)});
        add({Vop3p(0x0e, 12, 0, 1, false), Vop3pSrc3(s11, V(3), s12, 3, 4)});
        add({Vop3p(0x0e, 13, 0, 0, false), Vop3pSrc3(V(3), s12, s11, 3, 0)});
        add({0x7e1c020cu});
        add({(0x3cu << 25u) | (14u << 17u) | (3u << 9u) | s11});
        add({Vop3p(0x10, 15, 0, 0, false), Vop3pSrc(V(3), V(2), 3, 0)});
        add({Vop3p(0x0e, 16, 2, 6, false), Vop3pSrc3(V(3), s11, s12, 1, 1)});
        add({Vop3p(0x0f, 17, 3, 0, false), Vop3pSrc(V(3), V(3), 0, 3)});
        add({Vop3p(0x10, 18, 0, 0, true), Vop3pSrc(V(3), s12, 3, 2)});
        add({Vop3p(0x0e, 19, 0, 0, true), Vop3pSrc3(s12, V(3), s11, 3, 7)});
    } else {
        add({Vop3(0x132, 4), Src(V(1), s10)});
        add({Vop3(0x133, 5), Src(V(1), s10)});
        add({Vop3(0x134, 6), Src(V(1), s10)});
        add({Vop3(0x135, 7), Src(V(1), s10)});
        add({Vop3(0x132, 8, 1), Src(V(1), s10, 0x80, 2)});
        add({Vop3(0x135, 9, 2, 0, true), Src(V(1), s10, 0x80, 1)});
        add({Vop3(0x132, 10, 0, 0, true), Src(V(1), s10)});
        add({Vop3(0x34b, 11), Src(V(1), s10, s12)});
        add({Vop3(0x34b, 12), Src(V(1), s12, s10)});
        add({Vop3(0x34b, 13, 4, 0, true), Src(V(1), s10, s12, 1)});
        add({0x7e1c020cu});
        add({Vop3(0x136, 14), Src(V(1), s10)});
        add({Vop3(0x133, 15, 3), Src(V(1), s10)});
        add({Vop3(0x34b, 16, 0, 1), Src(V(3), s10, s12)});
        add({(0x32u << 25u) | (17u << 17u) | (1u << 9u) | s10});
        add({(0x37u << 25u) | (18u << 17u) | (1u << 9u) | s10, 0x3c01u});
        add({(0x38u << 25u) | (19u << 17u) | (1u << 9u) | s10, 0xc248u});
    }
    for (std::uint32_t k = 0; k < Ops; ++k) {
        add({(0x25u << 25u) | ((20u + k) << 17u) | ((4u + k) << 9u) | V(20u + k)});
        add({Vop3(0x169, 20u + k), Src(V(20u + k), 0x0d)});
    }
    add({0x800a810au, 0xbf07050au});
    const auto branch = static_cast<std::uint32_t>(code.size());
    const auto offset = static_cast<std::int32_t>(loop) - static_cast<std::int32_t>(branch + 1u);
    add({0xbf850000u | (static_cast<std::uint32_t>(offset) & 0xffffu)});
    add({0x165002ffu, Slots * 4u});
    add({0xe0701000u, 0x80000128u});
    for (std::uint32_t k = 0; k < Ops; ++k) add({0xe0701000u | (4u * (k + 1u)), 0x80000000u | ((20u + k) << 8u) | 0x28u});
    add({0xbf810000u});
    return code;
}

struct Mode {
    std::string name;
    bool packed;
    bool known;
    ShaderRecompiler::ShaderFloatMode mode;
};

std::vector<std::uint32_t> CodeStorage;

std::vector<std::uint32_t> Run(AgcDriver::VulkanDevice& device, const Mode& mode, std::uint32_t chunk, std::uint32_t round, bool native) {
    static constexpr std::array<std::uint32_t, 64> Third{0x0000u, 0x8000u, 0x3c00u, 0xbc00u, 0x7c00u, 0xfc00u, 0x7e00u, 0x7c01u, 0xfe55u, 0x0001u, 0x8001u, 0x03ffu, 0x0400u, 0x7bffu, 0xfbffu, 0x3555u, 0x7dffu, 0xfdffu, 0xfe00u, 0xfc01u, 0x7fffu, 0xffffu, 0x83ffu, 0x8400u, 0x0200u, 0x8200u, 0x3bffu, 0x3c01u, 0xc000u, 0x4000u, 0x5640u, 0xd640u, 0xafc0u, 0x2755u, 0xb80bu, 0x6e12u, 0x51c7u, 0xa66au, 0xd522u, 0x81c3u, 0x013bu, 0xbba7u, 0x9e7au, 0x235cu, 0xaff4u, 0xc160u, 0xc85au, 0x880cu, 0x1febu, 0xa66fu, 0x3ba0u, 0x17dbu, 0x3beeu, 0x77ccu, 0xb8a0u, 0x502eu, 0xd9dbu, 0x72f4u, 0xc458u, 0xce8eu, 0x4091u, 0x28ceu, 0x44f8u, 0x0327u};
    Output.fill(0xdeadbeefu);
    const auto address = reinterpret_cast<std::uintptr_t>(Output.data());
    std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), static_cast<std::uint32_t>(Output.size() * 4u), 0x31016facu,
        chunk * ChunkB, (chunk + 1u) * ChunkB, Third[(chunk + 16u * round) % Third.size()], 0u};
    const std::span<const std::uint32_t> code(CodeStorage);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{256, 1, 1}, 0u, {true, false, false}, false, 1};
    auto target = device.ComputeTarget(32);
    target.nativeFloat16Arithmetic = native;
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    if (mode.known) request.context.floatMode = mode.mode;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 256, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    for (std::uint32_t a = 0; a < 65536u; ++a) Require(Output[a * Slots] == a, std::string("thread mapping broken at a=") + std::to_string(a));
    return std::vector<std::uint32_t>(Output.begin(), Output.end());
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_NATIVE_FLOAT16", "1");
#else
        setenv("APS5_NATIVE_FLOAT16", "1", 1);
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!device->ComputeTarget(32).nativeFloat16Arithmetic) {
            std::puts("skipped, the device has no native f16 arithmetic");
            return VulkanTestSkipped;
        }
        using FM = ShaderRecompiler::ShaderFloatMode;
        std::vector<Mode> modes;
        for (const bool packed : {false, true}) {
            const std::string prefix = packed ? "packed " : "scalar ";
            modes.push_back({prefix + "unknown mode", packed, false, {}});
            modes.push_back({prefix + "0xf0", packed, true, FM{0xf0u, false, false, false}});
            modes.push_back({prefix + "0xf0 DX10 IEEE FP16_OVFL", packed, true, FM{0xf0u, true, true, true}});
            modes.push_back({prefix + "0xc3 DX10 FP16_OVFL", packed, true, FM{0xc3u, true, false, true}});
        }
        for (std::uint32_t index = 0; index < modes.size(); ++index) {
            const auto& mode = modes[index];
            CodeStorage = Program(mode.packed);
            for (const std::uint32_t chunk : {0u, 7u, 8u, 15u}) {
                const auto expected = Run(*device, mode, chunk, index % 4u, false);
                const auto actual = Run(*device, mode, chunk, index % 4u, true);
                for (std::size_t slot = 0; slot < expected.size(); ++slot) {
                    if (expected[slot] == actual[slot]) continue;
                    const auto a = static_cast<std::uint32_t>(slot / Slots);
                    const auto op = static_cast<std::uint32_t>(slot % Slots) - 1u;
                    throw std::runtime_error("native f16 arithmetic, " + mode.name + ": operation " + std::to_string(op) + " with a = " + std::to_string(a) + " over b in [" + std::to_string(chunk * ChunkB) + ", " + std::to_string((chunk + 1u) * ChunkB) + ") differs from the f32 lowering");
                }
            }
        }
        std::puts("native f16 arithmetic tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
