#include "Translation/TranslationContext.hpp"
#include "RdnaDecoder/RdnaVectorOpDecoder.hpp"
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace ShaderRecompiler;

namespace {

enum class Tininess {
    None,
    Checked,
};

constexpr std::size_t TinyCheckInstructions = 6;

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("f32 constant factor: " + message);
}

std::unordered_set<const IrValue*> Cone(const IrValue* root, const std::unordered_set<const IrValue*>& instructions) {
    std::unordered_set<const IrValue*> cone;
    std::vector<const IrValue*> pending{root};
    while (!pending.empty()) {
        const IrValue* value = pending.back();
        pending.pop_back();
        if (!instructions.contains(value) || !cone.insert(value).second) continue;
        for (const IrValue* argument : value->Arguments()) pending.push_back(argument);
    }
    return cone;
}

std::size_t TinyCheckSize(const std::string& name, const IrBlock& block, const IrValue* compare) {
    const std::unordered_set<const IrValue*> instructions(block.Instructions().begin(), block.Instructions().end());
    const auto tiny = Cone(compare, instructions);
    for (const auto* value : block.Instructions()) {
        if (value->Opcode() != IrOpcode::SelectF32 || !Cone(value->Argument(0), instructions).contains(compare)) continue;
        const auto product = Cone(value->Argument(2), instructions);
        std::size_t size = 0u;
        for (const auto* instruction : tiny) {
            if (!product.contains(instruction)) ++size;
        }
        return size;
    }
    Require(false, name + " selects no flushed product on its tiny product check");
    return 0u;
}

const IrBlock& Translate(IrProgram& program, const std::string& name, const std::vector<std::uint32_t>& words, RdnaOpcode opcode, std::uint32_t floatMode) {
    const RdnaInstruction instruction = DecodeRdnaVectorOp(std::span<const std::uint32_t>(words), 0u);
    Require(instruction.op == opcode, name + " decodes to another opcode");
    Require(instruction.wordCount == words.size(), name + " has the wrong length");
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    TranslationContext context(program, block, 256);
    context.SetFloatMode(ShaderFloatMode{floatMode, true, false, false});
    context.TranslateInstruction(instruction);
    return block;
}

void Check(const std::string& name, const std::vector<std::uint32_t>& words, RdnaOpcode opcode, Tininess expected, std::uint32_t floatMode = 0xc0u) {
    IrProgram program;
    const auto& block = Translate(program, name, words, opcode, floatMode);
    std::uint32_t multiplyHigh = 0u;
    std::uint32_t tinyCompares = 0u;
    const IrValue* tinyCompare = nullptr;
    for (const auto* value : block.Instructions()) {
        if (value->Opcode() == IrOpcode::UMulHi) ++multiplyHigh;
        if (value->Opcode() == IrOpcode::ULessThan32 && value->Argument(1)->HasImmediate() && value->Argument(1)->ImmediateU32() == 0x207fffffu) {
            ++tinyCompares;
            tinyCompare = value;
        }
    }
    const std::uint32_t expectedCompares = expected == Tininess::None ? 0u : 1u;
    Require(tinyCompares == expectedCompares, name + " emits " + std::to_string(tinyCompares) + " tiny product checks, expected " + std::to_string(expectedCompares));
    Require(multiplyHigh == 0u, name + " emits " + std::to_string(multiplyHigh) + " significand products, expected none");
    if (tinyCompare == nullptr) return;
    const std::size_t size = TinyCheckSize(name, block, tinyCompare);
    Require(size == TinyCheckInstructions, name + " checks the product for tininess with " + std::to_string(size) + " instructions the product does not need, expected " + std::to_string(TinyCheckInstructions));
}

void CheckFoldedSource(const std::string& name, const std::vector<std::uint32_t>& words, RdnaOpcode opcode, IrOpcode product, std::uint32_t index, std::uint32_t expected) {
    IrProgram program;
    const auto& block = Translate(program, name, words, opcode, 0xc0u);
    for (const auto* value : block.Instructions()) {
        if (value->Opcode() != product) continue;
        const IrValue* source = value->Argument(index);
        Require(source->Opcode() == IrOpcode::BitCastF32U32 && source->Argument(0)->HasImmediate(), name + " source " + std::to_string(index) + " is not a constant");
        Require(source->Argument(0)->ImmediateU32() == expected, name + " source " + std::to_string(index) + " is folded to the wrong constant");
        return;
    }
    Require(false, name + " emits no product");
}

}

int main() {
    try {
        Check("v_mul_f32 v5, v6, v7", {0x100a0f06u}, RdnaOpcode::VMulF32, Tininess::Checked);
        Check("v_mul_f32 v5, 2.0, v7", {0x100a0ef4u}, RdnaOpcode::VMulF32, Tininess::None);
        Check("v_mul_f32 v5, -4.0, v7", {0x100a0ef7u}, RdnaOpcode::VMulF32, Tininess::None);
        Check("v_mul_f32 v5, 1.0, v7", {0x100a0ef2u}, RdnaOpcode::VMulF32, Tininess::None);
        Check("v_mul_f32 v5, 0x7f800000, v7", {0x100a0effu, 0x7f800000u}, RdnaOpcode::VMulF32, Tininess::None);
        Check("v_mul_f32 v5, 0x00400000, v7", {0x100a0effu, 0x00400000u}, RdnaOpcode::VMulF32, Tininess::None);
        Check("v_mul_f32 v5, 1, v7", {0x100a0e81u}, RdnaOpcode::VMulF32, Tininess::None);
        Check("v_mul_f32 v5, 0.5, v7", {0x100a0ef0u}, RdnaOpcode::VMulF32, Tininess::Checked);
        Check("v_mul_f32 v5, 0x3e800000, v7", {0x100a0effu, 0x3e800000u}, RdnaOpcode::VMulF32, Tininess::Checked);
        Check("v_mul_f32 v5, 0x3f7fffff, v7", {0x100a0effu, 0x3f7fffffu}, RdnaOpcode::VMulF32, Tininess::Checked);
        Check("v_mul_f32 v5, 0.15915494, v7", {0x100a0ef8u}, RdnaOpcode::VMulF32, Tininess::Checked);
        Check("v_mul_legacy_f32 v5, 4.0, v7", {0x0e0a0ef6u}, RdnaOpcode::VMulLegacyF32, Tininess::None);
        Check("v_fmamk_f32 v5, v6, 0x40400000, v7", {0x580a0f06u, 0x40400000u}, RdnaOpcode::VMadmkF32, Tininess::None);
        Check("v_fmaak_f32 v5, v6, v7, 0x40400000", {0x5a0a0f06u, 0x40400000u}, RdnaOpcode::VMadakF32, Tininess::Checked);
        Check("v_fma_f32 v5, v6, -|2.0|, v7", {0xd54b0205u, 0x441de906u}, RdnaOpcode::VFmaF32, Tininess::None);
        Check("v_fma_f32 v5, v6, -|0.5|, v7", {0xd54b0205u, 0x441de106u}, RdnaOpcode::VFmaF32, Tininess::Checked);
        Check("v_mul_f32 v5, 2.0, v7 with f32 denormals kept", {0x100a0ef4u}, RdnaOpcode::VMulF32, Tininess::None, 0xf0u);
        Check("v_mul_f32 v5, v6, v7 with f32 denormals kept", {0x100a0f06u}, RdnaOpcode::VMulF32, Tininess::None, 0xf0u);
        CheckFoldedSource("v_fma_f32 v5, v6, -|2.0|, v7", {0xd54b0205u, 0x441de906u}, RdnaOpcode::VFmaF32, IrOpcode::FPFma32, 1u, 0xc0000000u);
        CheckFoldedSource("v_mul_f32 v5, 0x00400000, v7", {0x100a0effu, 0x00400000u}, RdnaOpcode::VMulF32, IrOpcode::FPMul32, 0u, 0x00000000u);
        CheckFoldedSource("v_mul_f32 v5, 0x80400000, v7", {0x100a0effu, 0x80400000u}, RdnaOpcode::VMulF32, IrOpcode::FPMul32, 0u, 0x80000000u);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
