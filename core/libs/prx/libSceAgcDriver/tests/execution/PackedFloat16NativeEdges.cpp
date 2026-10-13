#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t GroupThreads = 256;
constexpr std::uint32_t Results = 16;

constexpr std::array<std::uint16_t, 24> Edges{
    0x0000u, 0x8000u, 0x0001u, 0x8001u, 0x03ffu, 0x83ffu, 0x0400u, 0x8400u,
    0x3c00u, 0xbc00u, 0x3c01u, 0x3bffu, 0x1000u, 0x3e00u, 0x7bffu, 0xfbffu,
    0x7c00u, 0xfc00u, 0x7e00u, 0xfe55u, 0x7c01u, 0xfd55u, 0x0009u, 0x0007u,
};
constexpr std::uint32_t RandomRows = 16384;
constexpr std::uint32_t EdgeRows = Edges.size() * Edges.size() * Edges.size();
constexpr std::uint32_t Rows = (EdgeRows + RandomRows + GroupThreads - 1u) / GroupThreads * GroupThreads;

alignas(256) std::array<std::uint32_t, Rows * 4u> Input{};
alignas(256) std::array<std::uint32_t, Rows * Results> Output{};

struct PackedOp {
    const char* name;
    std::uint32_t op;
    std::uint32_t opSel;
    std::uint32_t opSelHi;
    std::uint32_t negLo;
    std::uint32_t negHi;
    bool clamp;
    bool ternary;
    std::uint32_t src1 = 0x105u;
};

constexpr std::uint32_t PkFma = 0x0e, PkAdd = 0x0f, PkMul = 0x10, PkMin = 0x11, PkMax = 0x12;

const std::array<PackedOp, Results> Ops{{
    {"v_pk_add_f16", PkAdd, 0, 7, 0, 0, false, false},
    {"v_pk_mul_f16", PkMul, 0, 7, 0, 0, false, false},
    {"v_pk_fma_f16", PkFma, 0, 7, 0, 0, false, true},
    {"v_pk_min_f16", PkMin, 0, 7, 0, 0, false, false},
    {"v_pk_max_f16", PkMax, 0, 7, 0, 0, false, false},
    {"v_pk_add_f16 clamp", PkAdd, 0, 7, 0, 0, true, false},
    {"v_pk_fma_f16 clamp", PkFma, 0, 7, 0, 0, true, true},
    {"v_pk_max_f16 clamp", PkMax, 0, 7, 0, 0, true, false},
    {"v_pk_mul_f16 op_sel:[1,0] op_sel_hi:[0,1]", PkMul, 1, 6, 0, 0, false, false},
    {"v_pk_add_f16 neg_lo:[0,1] neg_hi:[1,0]", PkAdd, 0, 7, 2, 1, false, false},
    {"v_pk_fma_f16 neg_lo:[1,0,1] op_sel_hi:[1,1,0]", PkFma, 0, 3, 5, 0, false, true},
    {"v_pk_min_f16 op_sel:[0,1] op_sel_hi:[0,1]", PkMin, 2, 6, 0, 0, false, false},
    {"v_pk_max_f16 neg_lo:[1,1] neg_hi:[1,1]", PkMax, 0, 7, 3, 3, false, false},
    {"v_pk_fma_f16 op_sel:[1,1,1] op_sel_hi:[0,0,0]", PkFma, 7, 0, 0, 0, false, true},
    {"v_pk_add_f16 v4, 1.0", PkAdd, 0, 7, 0, 0, false, false, 0xf2u},
    {"v_pk_mul_f16 op_sel:[1,1] op_sel_hi:[1,1]", PkMul, 3, 7, 0, 0, false, false},
}};

std::vector<std::uint32_t> Program() {
    std::vector<std::uint32_t> code{
        0x8f098808u,
        0x4a020009u,
        0x34040284u,
        0x34060286u,
        0xe0381000u, 0x80000402u,
        0xbf8c3f70u,
    };
    for (std::uint32_t index = 0; index < Results; ++index) {
        const auto& op = Ops[index];
        const std::uint32_t src2 = op.ternary ? 0x106u : 0x80u;
        code.push_back(0xcc000000u | (op.op << 16u) | (op.clamp ? 0x8000u : 0u) | (((op.opSelHi >> 2u) & 1u) << 14u) | (op.opSel << 11u) | (op.negHi << 8u) | (10u + index));
        code.push_back((op.negLo << 29u) | ((op.opSelHi & 3u) << 27u) | (src2 << 18u) | (op.src1 << 9u) | 0x104u);
    }
    for (std::uint32_t store = 0; store < Results / 4u; ++store) {
        code.push_back(0xe0781000u | (16u * store));
        code.push_back(0x80010003u | ((10u + 4u * store) << 8u));
    }
    code.push_back(0xbf810000u);
    return code;
}

void FillInput() {
    std::uint32_t row = 0;
    for (std::uint32_t i = 0; i < Edges.size(); ++i) {
        for (std::uint32_t j = 0; j < Edges.size(); ++j) {
            for (std::uint32_t k = 0; k < Edges.size(); ++k, ++row) {
                Input[row * 4u + 0u] = Edges[i] | (static_cast<std::uint32_t>(Edges[k]) << 16u);
                Input[row * 4u + 1u] = Edges[j] | (static_cast<std::uint32_t>(Edges[i]) << 16u);
                Input[row * 4u + 2u] = Edges[k] | (static_cast<std::uint32_t>(Edges[j]) << 16u);
                Input[row * 4u + 3u] = 0u;
            }
        }
    }
    std::uint32_t state = 0x9e3779b9u;
    const auto next = [&] {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        return state;
    };
    for (; row < Rows; ++row) {
        for (std::uint32_t word = 0; word < 3u; ++word) Input[row * 4u + word] = next();
        Input[row * 4u + 3u] = 0u;
    }
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::vector<std::uint32_t> Run(AgcDriver::VulkanDevice& device, const std::vector<std::uint32_t>& code, const std::optional<ShaderRecompiler::ShaderFloatMode>& mode, bool native) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> words(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(words.data()), std::as_bytes(words)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{GroupThreads, 1, 1}, 0u, {true, false, false}, false, 1};
    auto target = device.ComputeTarget(32);
    target.nativeFloat16Arithmetic = native;
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(words.data()), words, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    request.context.floatMode = mode;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, Rows / GroupThreads, 1, 1, {}, reinterpret_cast<std::uintptr_t>(words.data()));
    device.WaitIdle();
    return std::vector<std::uint32_t>(Output.begin(), Output.end());
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
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
        FillInput();
        const auto code = Program();
        using FM = ShaderRecompiler::ShaderFloatMode;
        std::vector<std::pair<std::string, std::optional<FM>>> modes{{"unknown mode", std::nullopt}};
        for (std::uint32_t bits = 0; bits < 8u; ++bits) {
            const FM mode{0xf0u, (bits & 1u) != 0u, (bits & 2u) != 0u, (bits & 4u) != 0u};
            modes.push_back({"0xf0" + std::string(mode.dx10Clamp ? " DX10" : "") + (mode.ieeeMode ? " IEEE" : "") + (mode.fp16Overflow ? " FP16_OVFL" : ""), mode});
        }
        modes.push_back({"0xc3 DX10 IEEE", FM{0xc3u, true, true, false}});
        bool sawNative = false;
        for (const auto& [name, mode] : modes) {
            const auto expected = Run(*device, code, mode, false);
            const auto actual = Run(*device, code, mode, true);
            for (std::size_t slot = 0; slot < expected.size(); ++slot) {
                if (expected[slot] == actual[slot]) continue;
                const auto row = static_cast<std::uint32_t>(slot / Results);
                const auto& op = Ops[slot % Results];
                throw std::runtime_error("native packed f16, " + name + ": " + op.name + " with a=" + Hex(Input[row * 4u]) + " b=" + Hex(Input[row * 4u + 1u]) + " c=" + Hex(Input[row * 4u + 2u]) +
                    " gives " + Hex(actual[slot]) + ", the f32 lowering " + Hex(expected[slot]));
            }
            sawNative = sawNative || std::any_of(actual.begin(), actual.end(), [](std::uint32_t value) { return value != 0xdeadbeefu; });
        }
        Require(sawNative, "f16 edge test: the program wrote no results");
        const std::uint32_t tieRow = 10u * Edges.size() * Edges.size() + 11u * Edges.size() + 22u;
        const auto lowering = Run(*device, code, std::nullopt, true);
        Require((lowering[tieRow * Results + 2u] & 0xffffu) == 0x3c01u, "v_pk_fma_f16 (1 + 2^-10) * (1 - 2^-11) + 9 * 2^-24 is not rounded once: " + Hex(lowering[tieRow * Results + 2u]));
        std::puts("native packed f16 edge tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
