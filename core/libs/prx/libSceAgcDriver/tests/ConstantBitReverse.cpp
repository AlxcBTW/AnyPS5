#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/ConstantFolder.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>

using namespace ShaderRecompiler;

namespace {

bool Check(std::uint32_t input, std::uint32_t expected) {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& constant = builder.Constant(input);
    auto& reverse = builder.Emit(IrOpcode::BitReverse32, IrType::U32, {&constant});
    auto& reference = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&reverse});
    ConstantFolder{}.Fold(program);
    const auto* actual = reference.Argument(0)->Resolve();
    if (actual->HasImmediate() && actual->Type() == IrType::U32 && actual->ImmediateU32() == expected) return true;
    std::fprintf(stderr, "bit reverse of 0x%08x did not fold to 0x%08x\n", input, expected);
    return false;
}

}

int main() {
    int failures = 0;
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 8> patterns{{
        {0u, 0u}, {0xffffffffu, 0xffffffffu}, {0x55555555u, 0xaaaaaaaau},
        {0x00ff00ffu, 0xff00ff00u}, {0x01234567u, 0xe6a2c480u},
        {0x89abcdefu, 0xf7b3d591u}, {0xdeadbeefu, 0xf77db57bu}, {0x0000000fu, 0xf0000000u},
    }};
    for (const auto& [input, expected] : patterns) failures += !Check(input, expected);
    for (std::uint32_t bit = 0u; bit < 32u; ++bit) failures += !Check(1u << bit, 1u << (31u - bit));
    IrProgram program;
    auto& block = program.CreateBlock();
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& input = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& reverse = builder.Emit(IrOpcode::BitReverse32, IrType::U32, {&input});
    auto& reference = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&reverse});
    ConstantFolder{}.Fold(program);
    if (reference.Argument(0)->Resolve() != &reverse || reverse.Argument(0)->Resolve() != &input) {
        std::fprintf(stderr, "runtime bit reverse was changed\n");
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
