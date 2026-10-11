#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_MASKEDSELECTELIMINATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_MASKEDSELECTELIMINATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ShaderRecompiler {

enum class MaskedSelectRefusalReason : std::uint8_t {
    VisitLimit,
    LoopSelect,
    LoopMask,
    LoopExec,
    WholeQuadMode,
    Derivative,
    CrossLane,
    ExecReactivated,
    ObservableUse,
    ImplicationDepth,
    Unreached,
};

[[nodiscard]] std::string_view MaskedSelectRefusalCode(MaskedSelectRefusalReason reason);

struct MaskedSelectRefusal {
    std::uint32_t select = 0;
    MaskedSelectRefusalReason reason = MaskedSelectRefusalReason::ObservableUse;
    std::string line;
};

struct MaskedSelectDrop {
    std::uint32_t select = 0;
    std::string line;
};

struct MaskedSelectEliminationStats {
    std::uint32_t removedSelects = 0;
    std::vector<MaskedSelectDrop> drops;
    std::vector<MaskedSelectRefusal> refusals;
};

[[nodiscard]] bool AncillaryTraceEnabled();
[[nodiscard]] bool ReadsPackedAncillary(const IrProgram& program);
[[nodiscard]] std::string AncillarySummaryLine(const IrProgram& program, const MaskedSelectEliminationStats& stats, bool composed);

class MaskedSelectEliminator {
public:
    [[nodiscard]] MaskedSelectEliminationStats Eliminate(IrProgram& program) const;
    [[nodiscard]] MaskedSelectEliminationStats Eliminate(IrProgram& program, bool explain) const;
};

}

#endif
