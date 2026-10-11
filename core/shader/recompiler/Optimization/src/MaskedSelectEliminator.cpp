#include "Optimization/MaskedSelectEliminator.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include "RdnaDecoder/RdnaInstruction.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
namespace {

constexpr std::size_t MinimumWalkLimit = 4096;
constexpr std::size_t WalkLimitPerInstruction = 2;
constexpr std::uint32_t DepthLimit = 8;

bool isSelect(IrOpcode opcode) {
    return opcode == IrOpcode::SelectU1 || opcode == IrOpcode::SelectU32 || opcode == IrOpcode::SelectF32;
}

bool isLaneLocal(IrOpcode opcode) {
    if (IrOpcodeHasSideEffects(opcode)) return false;
    switch (opcode) {
        case IrOpcode::Identity:
        case IrOpcode::IAdd32:
        case IrOpcode::ISub32:
        case IrOpcode::IMul32:
        case IrOpcode::SelectU1:
        case IrOpcode::SelectU32:
        case IrOpcode::SelectF32:
        case IrOpcode::BitCastU16F16:
        case IrOpcode::BitCastF16U16:
        case IrOpcode::BitCastU32F32:
        case IrOpcode::BitCastF32U32:
        case IrOpcode::ConvertU16U32:
        case IrOpcode::ConvertU32U16:
        case IrOpcode::ConvertU8U32:
        case IrOpcode::ConvertU32U8:
        case IrOpcode::ConvertF32F16:
        case IrOpcode::ConvertF16F32:
        case IrOpcode::ConvertS32F32:
        case IrOpcode::ConvertU32F32:
        case IrOpcode::ConvertF32S32:
        case IrOpcode::ConvertF32U32:
        case IrOpcode::ConvertF32F64:
        case IrOpcode::ConvertF64F32:
        case IrOpcode::ConvertF64S32:
        case IrOpcode::ConvertF64U32:
        case IrOpcode::ConvertS32F64:
        case IrOpcode::ConvertU32F64:
        case IrOpcode::CompositeConstructU64:
        case IrOpcode::CompositeConstructU32x2:
        case IrOpcode::CompositeConstructU32x3:
        case IrOpcode::CompositeConstructF32x2:
        case IrOpcode::CompositeConstructU32x4:
        case IrOpcode::CompositeExtractU64:
        case IrOpcode::CompositeExtractU32x2:
        case IrOpcode::CompositeExtractU32x3:
        case IrOpcode::CompositeExtractU32x4:
        case IrOpcode::PackHalf2x16:
        case IrOpcode::PackFloat2x16Rtz:
        case IrOpcode::BitFieldInsert:
        case IrOpcode::BitFieldUExtract:
        case IrOpcode::BitFieldSExtract:
        case IrOpcode::IAdd64:
        case IrOpcode::IAddCarry32:
        case IrOpcode::ISub64:
        case IrOpcode::IMul64:
        case IrOpcode::SMulHi:
        case IrOpcode::UMulHi:
        case IrOpcode::IAbs32:
        case IrOpcode::ShiftLeftLogical32:
        case IrOpcode::ShiftLeftLogical64:
        case IrOpcode::ShiftRightLogical32:
        case IrOpcode::ShiftRightLogical64:
        case IrOpcode::ShiftRightArithmetic32:
        case IrOpcode::ShiftRightArithmetic64:
        case IrOpcode::BitwiseAnd32:
        case IrOpcode::BitwiseAnd64:
        case IrOpcode::BitwiseOr32:
        case IrOpcode::BitwiseXor32:
        case IrOpcode::BitwiseNot32:
        case IrOpcode::BitReverse32:
        case IrOpcode::BitCount32:
        case IrOpcode::BitCount64:
        case IrOpcode::FindUMsb32:
        case IrOpcode::FindUMsb64:
        case IrOpcode::FindILsb32:
        case IrOpcode::SMin32:
        case IrOpcode::UMin32:
        case IrOpcode::SMax32:
        case IrOpcode::UMax32:
        case IrOpcode::SMinTri32:
        case IrOpcode::UMinTri32:
        case IrOpcode::SMaxTri32:
        case IrOpcode::UMaxTri32:
        case IrOpcode::SMedTri32:
        case IrOpcode::UMedTri32:
        case IrOpcode::SLessThan32:
        case IrOpcode::SLessThan64:
        case IrOpcode::ULessThan32:
        case IrOpcode::ULessThan64:
        case IrOpcode::IEqual32:
        case IrOpcode::IEqual64:
        case IrOpcode::SLessThanEqual32:
        case IrOpcode::ULessThanEqual32:
        case IrOpcode::SGreaterThan32:
        case IrOpcode::UGreaterThan32:
        case IrOpcode::UGreaterThan64:
        case IrOpcode::INotEqual32:
        case IrOpcode::INotEqual64:
        case IrOpcode::SGreaterThanEqual32:
        case IrOpcode::UGreaterThanEqual32:
        case IrOpcode::LogicalOr:
        case IrOpcode::LogicalAnd:
        case IrOpcode::LogicalXor:
        case IrOpcode::LogicalNot:
        case IrOpcode::FPOrdEqual32:
        case IrOpcode::FPUnordEqual32:
        case IrOpcode::FPOrdNotEqual32:
        case IrOpcode::FPUnordNotEqual32:
        case IrOpcode::FPOrdLessThan32:
        case IrOpcode::FPUnordLessThan32:
        case IrOpcode::FPOrdGreaterThan32:
        case IrOpcode::FPUnordGreaterThan32:
        case IrOpcode::FPOrdLessThanEqual32:
        case IrOpcode::FPUnordLessThanEqual32:
        case IrOpcode::FPOrdGreaterThanEqual32:
        case IrOpcode::FPUnordGreaterThanEqual32:
        case IrOpcode::FPIsNan32:
        case IrOpcode::FPCmpClass32:
        case IrOpcode::FPAbs32:
        case IrOpcode::FPNeg32:
        case IrOpcode::FPSaturate32:
        case IrOpcode::FPAdd32:
        case IrOpcode::FPSub32:
        case IrOpcode::FPFma32:
        case IrOpcode::FPMad32:
        case IrOpcode::FPMul32:
        case IrOpcode::FPMin32:
        case IrOpcode::FPMax32:
        case IrOpcode::FPMinTri32:
        case IrOpcode::FPMaxTri32:
        case IrOpcode::FPMedTri32:
        case IrOpcode::FPRecip32:
        case IrOpcode::FPRecipIFlag32:
        case IrOpcode::FPRecipSqrt32:
        case IrOpcode::FPSqrt:
        case IrOpcode::FPSin:
        case IrOpcode::FPCos:
        case IrOpcode::FPExp2:
        case IrOpcode::FPLog2:
        case IrOpcode::FPRoundEven32:
        case IrOpcode::FPFloor32:
        case IrOpcode::FPCeil32:
        case IrOpcode::FPTrunc32:
        case IrOpcode::FPFract32:
        case IrOpcode::FPAdd64:
        case IrOpcode::FPMul64:
        case IrOpcode::FPFma64:
        case IrOpcode::FPMin64:
        case IrOpcode::FPMax64:
        case IrOpcode::FPSaturate64:
        case IrOpcode::FPLdexp64:
        case IrOpcode::FPRoundEven64:
        case IrOpcode::FPFloor64:
        case IrOpcode::FPCeil64:
        case IrOpcode::FPTrunc64:
        case IrOpcode::FPFract64:
        case IrOpcode::FPFrexpMant64:
        case IrOpcode::FPFrexpExp64:
        case IrOpcode::MakeImageAddress:
        case IrOpcode::ImageQueryDimensions:
            return true;
        default:
            return false;
    }
}

bool readsOnlyWhereActive(IrOpcode opcode) {
    switch (opcode) {
        case IrOpcode::LoadBufferU8:
        case IrOpcode::LoadBufferU16:
        case IrOpcode::LoadBufferU32:
        case IrOpcode::LoadBufferU32x2:
        case IrOpcode::LoadBufferU32x3:
        case IrOpcode::LoadBufferU32x4:
        case IrOpcode::StoreBufferU8:
        case IrOpcode::StoreBufferU16:
        case IrOpcode::StoreBufferU32:
        case IrOpcode::StoreBufferU32x2:
        case IrOpcode::StoreBufferU32x3:
        case IrOpcode::StoreBufferU32x4:
        case IrOpcode::LoadSharedU8:
        case IrOpcode::LoadSharedU16:
        case IrOpcode::LoadSharedU32:
        case IrOpcode::LoadSharedU32x2:
        case IrOpcode::LoadSharedU32x3:
        case IrOpcode::LoadSharedU32x4:
        case IrOpcode::WriteSharedU8:
        case IrOpcode::WriteSharedU16:
        case IrOpcode::WriteSharedU32:
        case IrOpcode::WriteSharedU32x2:
        case IrOpcode::WriteSharedU32x3:
        case IrOpcode::WriteSharedU32x4:
        case IrOpcode::SetAttribute:
        case IrOpcode::ImageRead:
        case IrOpcode::ImageWrite:
            return true;
        default:
            return false;
    }
}

bool isExplicitLodSample(const IrProgram& program, const IrValue& value) {
    if (value.Opcode() != IrOpcode::ImageSampleRaw && value.Opcode() != IrOpcode::ImageGatherRaw) return false;
    if (program.Resources().stage != IrShaderStage::Pixel) return true;
    const auto index = value.Flags<MemoryFlags>().index;
    if (index >= program.Resources().memoryInfo.size()) return false;
    return (program.Resources().memoryInfo[index].imageSampleFlags & (RdnaImageSampleFlagDerivative | RdnaImageSampleFlagLod | RdnaImageSampleFlagLevelZero)) != 0u;
}

bool isImmediate(const IrValue* value, std::uint32_t immediate) {
    return value->HasImmediate() && value->Type() == IrType::U32 && value->ImmediateU32() == immediate;
}

const IrValue* otherArgument(const IrValue* value, std::uint32_t immediate) {
    if (value->ArgumentCount() != 2u) return nullptr;
    const IrValue* left = value->Argument(0)->Resolve();
    const IrValue* right = value->Argument(1)->Resolve();
    if (isImmediate(right, immediate)) return left;
    if (isImmediate(left, immediate)) return right;
    return nullptr;
}

struct ThreadBit {
    const IrValue* low = nullptr;
    const IrValue* high = nullptr;
};

bool isLane(const IrValue* value) {
    return value->Resolve()->Opcode() == IrOpcode::LaneId;
}

std::optional<ThreadBit> threadBit(const IrValue* value, std::uint32_t waveSize) {
    value = value->Resolve();
    if (value->Opcode() != IrOpcode::INotEqual32) return std::nullopt;
    const IrValue* bit = otherArgument(value, 0u);
    if (bit == nullptr || bit->Opcode() != IrOpcode::BitwiseAnd32) return std::nullopt;
    const IrValue* shifted = otherArgument(bit, 1u);
    if (shifted == nullptr || shifted->Opcode() != IrOpcode::ShiftRightLogical32) return std::nullopt;
    const IrValue* amount = shifted->Argument(1)->Resolve();
    if (amount->Opcode() != IrOpcode::BitwiseAnd32) return std::nullopt;
    const IrValue* lane = otherArgument(amount, 31u);
    if (lane == nullptr || !isLane(lane)) return std::nullopt;
    const IrValue* word = shifted->Argument(0)->Resolve();
    if (waveSize == 32u || word->HasImmediate()) return ThreadBit{word, word};
    if (word->Opcode() != IrOpcode::SelectU32) return std::nullopt;
    const IrValue* half = word->Argument(0)->Resolve();
    if (half->Opcode() != IrOpcode::ULessThan32 || !isLane(half->Argument(0)) || !isImmediate(half->Argument(1)->Resolve(), 32u)) return std::nullopt;
    return ThreadBit{word->Argument(1)->Resolve(), word->Argument(2)->Resolve()};
}

bool alwaysTrue(const IrValue* value, std::uint32_t waveSize, std::uint32_t depth = 0) {
    value = value->Resolve();
    if (value->HasImmediate()) return value->Type() == IrType::Bool && value->ImmediateBool();
    if (depth > DepthLimit) return false;
    if (value->Opcode() == IrOpcode::LogicalAnd) return alwaysTrue(value->Argument(0), waveSize, depth + 1u) && alwaysTrue(value->Argument(1), waveSize, depth + 1u);
    const auto bit = threadBit(value, waveSize);
    return bit && isImmediate(bit->low, 0xffffffffu) && isImmediate(bit->high, 0xffffffffu);
}

bool readsQuad(const IrProgram& program, const IrValue& value) {
    return value.Opcode() == IrOpcode::ImageQueryLod || ((value.Opcode() == IrOpcode::ImageSampleRaw || value.Opcode() == IrOpcode::ImageGatherRaw) && !isExplicitLodSample(program, value));
}

bool readsOtherLanes(IrOpcode opcode) {
    switch (opcode) {
        case IrOpcode::ReadFirstLane:
        case IrOpcode::ReadLane:
        case IrOpcode::WriteLane:
        case IrOpcode::Ballot:
        case IrOpcode::DppMoveU32:
        case IrOpcode::DppUpdateU32:
        case IrOpcode::SwizzleU32:
        case IrOpcode::PermuteU32:
        case IrOpcode::BpermuteU32:
        case IrOpcode::Permlane16U32:
            return true;
        default:
            return false;
    }
}

bool isBuiltin(const IrValue* value, StageInputKind kind) {
    value = value->Resolve();
    return value->Opcode() == IrOpcode::GetBuiltin && value->ArgumentCount() != 0u && isImmediate(value->Argument(0)->Resolve(), static_cast<std::uint32_t>(kind));
}

bool isWqmWord(const IrValue* word) {
    word = word->Resolve();
    return word->Opcode() == IrOpcode::CompositeExtractU64 && word->Argument(0)->Resolve()->Opcode() == IrOpcode::WqmU64;
}

std::string name(const IrValue* value) {
    value = value->Resolve();
    if (isBuiltin(value, StageInputKind::PackedAncillary)) return ValueReference(value) + " (the ancillary word)";
    if (isBuiltin(value, StageInputKind::HelperInvocation)) return ValueReference(value) + " (HelperInvocation)";
    if (value->Opcode() == IrOpcode::GetBuiltin && value->ArgumentCount() != 0u) return ValueReference(value) + " (kind " + ValueReference(value->Argument(0)) + ")";
    return ValueReference(value);
}

std::string describeWord(const IrValue* word) {
    word = word->Resolve();
    const std::string text = name(word);
    if (word->HasImmediate()) return text;
    if (word->Opcode() == IrOpcode::CompositeExtractU32x4 && word->Argument(0)->Resolve()->Opcode() == IrOpcode::Ballot) return text + " = ballot of " + name(word->Argument(0)->Resolve()->Argument(0));
    if (isWqmWord(word)) return text + " = s_wqm word";
    const IrOpcode opcode = word->Opcode();
    if ((opcode == IrOpcode::BitwiseAnd32 || opcode == IrOpcode::BitwiseOr32 || opcode == IrOpcode::BitwiseXor32) && word->ArgumentCount() == 2u) {
        return text + "(" + name(word->Argument(0)) + ", " + name(word->Argument(1)) + ")";
    }
    return text;
}

std::string describeCondition(const IrValue* condition, std::uint32_t waveSize) {
    condition = condition->Resolve();
    const std::string text = name(condition);
    if (condition->HasImmediate()) return text;
    if (condition->Opcode() == IrOpcode::IEqual32) {
        const IrValue* helper = otherArgument(condition, 0u);
        if (helper != nullptr && isBuiltin(helper, StageInputKind::HelperInvocation)) return text + " = not a helper lane";
    }
    if (const auto bit = threadBit(condition, waveSize)) {
        return text + " = lane bit of " + describeWord(bit->low) + (bit->high == bit->low ? std::string() : " / " + describeWord(bit->high));
    }
    if (condition->Opcode() == IrOpcode::INotEqual32) {
        const IrValue* masked = otherArgument(condition, 0u);
        const IrValue* shifted = masked != nullptr && masked->Opcode() == IrOpcode::BitwiseAnd32 ? otherArgument(masked, 1u) : nullptr;
        if (shifted != nullptr && shifted->Opcode() == IrOpcode::ShiftRightLogical32) {
            const IrValue* amount = shifted->Argument(1)->Resolve();
            const std::string bit = amount->HasImmediate() && amount->Type() == IrType::U32 ? std::to_string(amount->ImmediateU32()) : name(amount);
            return text + " = bit " + bit + " of " + name(shifted->Argument(0));
        }
    }
    if (condition->ArgumentCount() == 2u && (condition->Opcode() == IrOpcode::LogicalAnd || condition->Opcode() == IrOpcode::LogicalOr)) {
        return text + "(" + name(condition->Argument(0)) + ", " + name(condition->Argument(1)) + ")";
    }
    return text;
}

std::string describeValue(const IrValue* value, std::uint32_t waveSize) {
    return value->Resolve()->Type() == IrType::Bool ? describeCondition(value, waveSize) : describeWord(value);
}

enum class Region : std::uint8_t { Unreached, Once, Loop };

Region regionOf(const std::vector<Region>& regions, const IrBlock* block) {
    return block != nullptr && block->Id() < regions.size() ? regions[block->Id()] : Region::Unreached;
}

bool followable(const std::vector<Region>& regions, const IrBlock* block, bool acrossLoops) {
    const Region region = regionOf(regions, block);
    return region == Region::Once || (acrossLoops && region == Region::Loop);
}

struct ImplicationKey {
    const IrValue* first = nullptr;
    const IrValue* second = nullptr;

    bool operator==(const ImplicationKey&) const = default;
};

struct ImplicationKeyHash {
    std::size_t operator()(const ImplicationKey& key) const {
        return std::hash<const IrValue*>{}(key.first) * 31u ^ std::hash<const IrValue*>{}(key.second);
    }
};

class Implications {
public:
    Implications(const std::vector<Region>& regions, std::uint32_t waveSize, std::size_t& work) : regions(regions), waveSize(waveSize), work(work) {}

    void Reset(const IrValue* writeMask, bool acrossLoops) {
        mask = writeMask;
        loops = acrossLoops;
        always = alwaysTrue(writeMask, waveSize);
        results.clear();
        proven.clear();
        assumptionReads = 0;
        causes = 0;
        leaf = nullptr;
    }

    enum Cause : std::uint8_t { CauseBudget = 1u, CauseDepth = 2u, CauseWqm = 4u, CauseLoop = 8u };

    struct Explanation {
        bool implied = false;
        std::uint8_t causes = 0;
        const IrValue* leaf = nullptr;
    };

    Explanation Explain(const IrValue* condition) {
        results.clear();
        proven.clear();
        assumptionReads = 0;
        causes = 0;
        leaf = nullptr;
        const bool implied = Implies(condition);
        return Explanation{implied, causes, leaf};
    }

    bool Implies(const IrValue* condition, std::uint32_t depth = 0) {
        condition = condition->Resolve();
        if (condition == mask || always) return true;
        if (condition->HasImmediate()) {
            const bool vacuous = condition->Type() == IrType::Bool && !condition->ImmediateBool();
            if (!vacuous) stopAt(condition);
            return vacuous;
        }
        const ImplicationKey key{condition, nullptr};
        if (const auto known = lookup(key)) return *known;
        if (exhausted(depth)) return false;
        --work;
        const Attempt attempt = assume(key);
        bool result = false;
        if (condition->Opcode() == IrOpcode::LogicalAnd) {
            result = Implies(condition->Argument(0), depth + 1u) || Implies(condition->Argument(1), depth + 1u);
        } else if (condition->IsPhi()) {
            result = merges(*condition);
            for (std::size_t index = 0; result && index < condition->ArgumentCount(); ++index) {
                result = Implies(condition->Argument(index), depth + 1u);
            }
            if (!result && regionOf(regions, condition->Parent()) == Region::Loop) causes |= CauseLoop;
        } else if (const auto bit = threadBit(condition, waveSize)) {
            result = wordsImply(bit->low, bit->high, depth + 1u);
        } else {
            stopAt(condition);
        }
        return settle(key, attempt, result);
    }

private:
    static constexpr std::uint32_t ImplicationDepthLimit = 64;

    bool exhausted(std::uint32_t depth) {
        if (depth > ImplicationDepthLimit) {
            causes |= CauseDepth;
            return true;
        }
        if (work == 0) {
            causes |= CauseBudget;
            return true;
        }
        return false;
    }

    void stopAt(const IrValue* value) {
        if (leaf == nullptr) leaf = value;
    }

    enum class Proof : std::uint8_t { Assumed, Implied, NotImplied };

    struct Attempt {
        std::size_t proven = 0;
        std::size_t assumptionReads = 0;
    };

    std::optional<bool> lookup(const ImplicationKey& key) {
        const auto known = results.find(key);
        if (known == results.end()) return std::nullopt;
        if (known->second == Proof::Assumed) ++assumptionReads;
        return known->second != Proof::NotImplied;
    }

    Attempt assume(const ImplicationKey& key) {
        results.emplace(key, Proof::Assumed);
        return Attempt{proven.size(), assumptionReads};
    }

    bool settle(const ImplicationKey& key, const Attempt& attempt, bool implied) {
        if (!implied && assumptionReads != attempt.assumptionReads) {
            for (std::size_t index = attempt.proven; index < proven.size(); ++index) results.erase(proven[index]);
            proven.resize(attempt.proven);
        }
        results[key] = implied ? Proof::Implied : Proof::NotImplied;
        if (implied) proven.push_back(key);
        return implied;
    }

    bool merges(const IrValue& phi) {
        if (!followable(regions, phi.Parent(), loops)) return notFollowable(phi.Parent());
        if (phi.ArgumentCount() != phi.PhiBlockCount()) return false;
        for (std::size_t index = 0; index < phi.ArgumentCount(); ++index) {
            const IrValue* incoming = phi.Argument(index)->Resolve();
            if (!incoming->HasImmediate() && !followable(regions, incoming->Parent(), loops)) return notFollowable(incoming->Parent());
        }
        return true;
    }

    bool notFollowable(const IrBlock* block) {
        if (regionOf(regions, block) == Region::Loop) causes |= CauseLoop;
        return false;
    }

    static const IrValue* incomingFrom(const IrValue& phi, const IrBlock* predecessor) {
        for (std::size_t index = 0; index < phi.PhiBlockCount(); ++index) {
            if (phi.PhiBlock(index) == predecessor) return phi.Argument(index)->Resolve();
        }
        return nullptr;
    }

    bool wordsImply(const IrValue* low, const IrValue* high, std::uint32_t depth) {
        if (isImmediate(low, 0u) && (waveSize == 32u || isImmediate(high, 0u))) return true;
        const ImplicationKey key{low, high};
        if (const auto known = lookup(key)) return *known;
        if (exhausted(depth)) return false;
        --work;
        const Attempt attempt = assume(key);
        bool result = false;
        bool followed = false;
        if (low->Opcode() == IrOpcode::CompositeExtractU32x4 && isImmediate(low->Argument(1)->Resolve(), 0u)) {
            const IrValue* ballot = low->Argument(0)->Resolve();
            const bool sameBallot = waveSize == 32u || (high->Opcode() == IrOpcode::CompositeExtractU32x4 && high->Argument(0)->Resolve() == ballot && isImmediate(high->Argument(1)->Resolve(), 1u));
            followed = ballot->Opcode() == IrOpcode::Ballot && sameBallot;
            result = followed && Implies(ballot->Argument(0), depth + 1u);
        }
        if (!result && low->Opcode() == IrOpcode::BitwiseAnd32) {
            if (waveSize == 32u) {
                followed = true;
                result = wordsImply(low->Argument(0)->Resolve(), high, depth + 1u) || wordsImply(low->Argument(1)->Resolve(), high, depth + 1u);
            } else if (high->Opcode() == IrOpcode::BitwiseAnd32) {
                followed = true;
                for (std::size_t lowIndex = 0; !result && lowIndex < 2u; ++lowIndex) {
                    for (std::size_t highIndex = 0; !result && highIndex < 2u; ++highIndex) {
                        result = wordsImply(low->Argument(lowIndex)->Resolve(), high->Argument(highIndex)->Resolve(), depth + 1u);
                    }
                }
            }
        }
        if (!result && low->IsPhi() && merges(*low) && (waveSize == 32u || (high->IsPhi() && high->Parent() == low->Parent() && merges(*high)))) {
            followed = true;
            result = true;
            for (std::size_t index = 0; result && index < low->PhiBlockCount(); ++index) {
                const IrValue* highIncoming = waveSize == 32u ? high : incomingFrom(*high, low->PhiBlock(index));
                result = highIncoming != nullptr && wordsImply(low->Argument(index)->Resolve(), highIncoming, depth + 1u);
            }
            if (!result && regionOf(regions, low->Parent()) == Region::Loop) causes |= CauseLoop;
        }
        if (!result && !followed) {
            if (isWqmWord(low) || isWqmWord(high)) causes |= CauseWqm;
            stopAt(isImmediate(low, 0u) ? high : low);
        }
        return settle(key, attempt, result);
    }

    const std::vector<Region>& regions;
    std::uint32_t waveSize;
    std::size_t& work;
    const IrValue* mask = nullptr;
    bool loops = false;
    bool always = false;
    std::unordered_map<ImplicationKey, Proof, ImplicationKeyHash> results;
    std::vector<ImplicationKey> proven;
    std::size_t assumptionReads = 0;
    std::uint8_t causes = 0;
    const IrValue* leaf = nullptr;
};

std::vector<Region> blockRegions(const IrProgram& program) {
    const auto& order = program.BlockOrder();
    if (order.empty()) return {};
    std::unordered_map<const IrBlock*, int> state;
    std::unordered_set<const IrBlock*> looping;
    std::vector<std::pair<const IrBlock*, std::size_t>> stack;
    state[order.front()] = 1;
    stack.emplace_back(order.front(), 0u);
    while (!stack.empty()) {
        auto& [block, next] = stack.back();
        if (next == block->Successors().size()) {
            state[block] = 2;
            stack.pop_back();
            continue;
        }
        const IrBlock* source = block;
        const IrBlock* successor = block->Successors()[next++];
        int& successorState = state[successor];
        if (successorState == 0) {
            successorState = 1;
            stack.emplace_back(successor, 0u);
        } else if (successorState == 1) {
            std::unordered_set<const IrBlock*> body{successor};
            std::vector<const IrBlock*> pending{source};
            while (!pending.empty()) {
                const IrBlock* member = pending.back();
                pending.pop_back();
                if (!body.insert(member).second) continue;
                for (const IrBlock* predecessor : member->Predecessors()) pending.push_back(predecessor);
            }
            looping.insert(body.begin(), body.end());
        }
    }
    std::vector<Region> result;
    for (const auto& [block, visited] : state) {
        if (visited != 2) continue;
        if (block->Id() >= result.size()) result.resize(block->Id() + 1u, Region::Unreached);
        result[block->Id()] = looping.contains(block) ? Region::Loop : Region::Once;
    }
    return result;
}

std::size_t walkLimit(const IrProgram& program) {
    std::size_t instructions = 0;
    for (const IrBlock* block : program.BlockOrder()) instructions += block->Instructions().size();
    return std::max(MinimumWalkLimit, WalkLimitPerInstruction * instructions);
}

using Reason = MaskedSelectRefusalReason;

constexpr std::size_t TraceRetryFactor = 64;

struct WalkStop {
    Reason reason = Reason::ObservableUse;
    const IrValue* use = nullptr;
    const IrValue* exec = nullptr;
    const IrValue* leaf = nullptr;
    const IrValue* through = nullptr;
    std::size_t arm = 0;
    bool throughExec = false;
    bool asExec = false;
    bool reproved = false;
    std::size_t steps = 0;
};

struct PathStep {
    const IrValue* select = nullptr;
    std::size_t arm = 0;
};

class MaskedLaneWalk {
public:
    MaskedLaneWalk(const IrProgram& program, const std::vector<Region>& regions, bool explain) : program(program), regions(regions), limit(walkLimit(program)), explain(explain), implications(regions, program.WaveSize(), work) {}

    bool UnobservedWhereMasked(const IrValue& select, const IrValue* mask) {
        const bool acrossLoops = mask->HasImmediate() || regionOf(regions, mask->Parent()) == Region::Once;
        work = limit;
        implications.Reset(mask, acrossLoops);
        startVisits();
        pending.clear();
        pending.push_back(&select);
        static_cast<void>(firstVisit(select));
        if (explain) pathOf(select) = PathStep{};
        while (!pending.empty()) {
            const IrValue* value = pending.back();
            pending.pop_back();
            for (const IrUse& use : value->OperandUses()) {
                const IrValue* user = use.user;
                if (!followable(regions, user->Parent(), acrossLoops)) return stop(regionOf(regions, user->Parent()) == Region::Loop ? Reason::LoopMask : Reason::Unreached, *value, *user);
                if (isSelect(user->Opcode()) && use.operand == 1u && implications.Implies(user->Argument(0))) continue;
                if (user->Opcode() == IrOpcode::LogicalAnd && use.operand < 2u && implications.Implies(user->Argument(1u - use.operand))) continue;
                const bool guarded = readsOnlyWhereActive(user->Opcode()) && use.operand + 1u < user->ArgumentCount();
                if (guarded && implications.Implies(user->Argument(user->ArgumentCount() - 1u))) continue;
                if (!user->IsPhi() && !isLaneLocal(user->Opcode()) && !isExplicitLodSample(program, *user)) return stopAtUse(*value, *user, use.operand, guarded);
                if (firstVisit(*user)) {
                    if (work == 0) return stop(Reason::VisitLimit, *value, *user);
                    --work;
                    if (explain) {
                        const PathStep step = isSelect(user->Opcode()) && use.operand != 0u ? PathStep{user, use.operand} : pathOf(*value);
                        pathOf(*user) = step;
                    }
                    pending.push_back(user);
                }
            }
        }
        return true;
    }

    std::pair<Reason, std::string> Describe(const IrValue& select, const IrValue* mask) {
        const WalkStop stopped = last;
        std::string detail = describeStop(stopped);
        if (stopped.reason == Reason::VisitLimit && !retrying) {
            retrying = true;
            const std::size_t budget = limit;
            limit = budget * TraceRetryFactor;
            const bool proven = UnobservedWhereMasked(select, mask);
            const std::size_t used = limit - work;
            if (proven) {
                detail += "; with " + std::to_string(limit) + " steps the walk proves the select in " + std::to_string(used);
            } else if (last.reason == Reason::VisitLimit) {
                detail += "; still unproven with " + std::to_string(limit) + " steps";
            } else {
                detail += "; with " + std::to_string(limit) + " steps it stops instead, " + std::string(MaskedSelectRefusalCode(last.reason)) + ": " + describeStop(last);
            }
            limit = budget;
            retrying = false;
        }
        return {stopped.reason, detail};
    }

private:
    bool stop(Reason reason, const IrValue& value, const IrValue& user) {
        if (!explain) return false;
        const PathStep path = pathOf(value);
        last = WalkStop{};
        last.reason = work == 0 ? Reason::VisitLimit : reason;
        last.use = &user;
        last.through = path.select;
        last.arm = path.arm;
        last.steps = limit - work;
        return false;
    }

    bool stopAtUse(const IrValue& value, const IrValue& user, std::size_t operand, bool guarded) {
        if (!explain) return false;
        static_cast<void>(stop(Reason::ObservableUse, value, user));
        if (last.reason == Reason::VisitLimit) return false;
        if (guarded) {
            explainExec(user.Argument(user.ArgumentCount() - 1u), false);
        } else if (readsQuad(program, user)) {
            last.reason = Reason::Derivative;
        } else if (readsOtherLanes(user.Opcode())) {
            last.reason = Reason::CrossLane;
        } else if (readsOnlyWhereActive(user.Opcode()) && operand + 1u == user.ArgumentCount()) {
            last.asExec = true;
        } else if (last.through != nullptr && last.arm == 1u) {
            explainExec(last.through->Argument(0), true);
        }
        return false;
    }

    void explainExec(const IrValue* exec, bool ofThrough) {
        last.exec = exec->Resolve();
        last.throughExec = ofThrough;
        work = limit;
        const auto proof = implications.Explain(exec);
        last.leaf = proof.leaf;
        last.reproved = proof.implied;
        if (proof.implied) {
            last.reason = Reason::ImplicationDepth;
        } else if ((proof.causes & Implications::CauseBudget) != 0u) {
            last.reason = Reason::VisitLimit;
        } else if ((proof.causes & Implications::CauseDepth) != 0u) {
            last.reason = Reason::ImplicationDepth;
        } else if ((proof.causes & Implications::CauseWqm) != 0u) {
            last.reason = Reason::WholeQuadMode;
        } else if ((proof.causes & Implications::CauseLoop) != 0u) {
            last.reason = Reason::LoopExec;
        } else {
            last.reason = Reason::ExecReactivated;
        }
    }

    std::string describeStop(const WalkStop& stopped) const {
        const std::uint32_t waveSize = program.WaveSize();
        const std::string use = stopped.use != nullptr ? name(stopped.use) : std::string("?");
        std::string text;
        if (stopped.exec != nullptr) {
            text = stopped.throughExec ? use + " reads the value after " + name(stopped.through) + " wrote it under " + describeCondition(stopped.exec, waveSize) : use + " runs under " + describeCondition(stopped.exec, waveSize);
            switch (stopped.reason) {
                case Reason::WholeQuadMode: text += ", an exec s_wqm made, so helper lanes are active"; break;
                case Reason::LoopExec: text += ", carried around a loop, and the loop rule could not prove it implies the mask"; break;
                case Reason::ImplicationDepth: text += stopped.reproved ? ", which a fresh proof shows implies the mask, but the walk reused a result cut short by the depth limit" : ", whose proof goes deeper than the limit"; break;
                case Reason::VisitLimit: text += ", whose proof used all " + std::to_string(limit) + " steps"; break;
                default: text += ", which does not imply the mask"; break;
            }
            if (stopped.leaf != nullptr) text += "; the proof stopped at " + describeValue(stopped.leaf, waveSize);
        } else {
            switch (stopped.reason) {
                case Reason::VisitLimit: text = "the walk used " + std::to_string(stopped.steps) + " of " + std::to_string(limit) + " steps, at " + use; break;
                case Reason::LoopMask: text = use + " is inside a loop, and the mask is computed inside a loop"; break;
                case Reason::Unreached: text = use + " is in a block the entry does not reach"; break;
                case Reason::Derivative: text = use + " takes its LOD from the quad"; break;
                case Reason::CrossLane: text = use + " reads other lanes"; break;
                default: text = use + (stopped.asExec ? " uses the value as its exec" : " is neither lane-local nor guarded by an exec"); break;
            }
        }
        if (stopped.through != nullptr && !stopped.throughExec) {
            text += "; reached through " + name(stopped.through) + (stopped.arm == 1u ? " (active arm, mask " : " (inactive arm, mask ") + describeCondition(stopped.through->Argument(0), waveSize) + ")";
        }
        return text;
    }

    PathStep& pathOf(const IrValue& value) {
        if (value.Id() >= paths.size()) paths.resize(value.Id() + 1u);
        return paths[value.Id()];
    }

    void startVisits() {
        if (++epoch != 0u) return;
        std::fill(visits.begin(), visits.end(), 0u);
        epoch = 1u;
    }

    bool firstVisit(const IrValue& value) {
        const std::uint32_t id = value.Id();
        if (id >= visits.size()) visits.resize(id + 1u, 0u);
        if (visits[id] == epoch) return false;
        visits[id] = epoch;
        return true;
    }

    const IrProgram& program;
    const std::vector<Region>& regions;
    std::size_t limit;
    std::size_t work = 0;
    bool explain;
    bool retrying = false;
    Implications implications;
    std::vector<const IrValue*> pending;
    std::vector<std::uint32_t> visits;
    std::uint32_t epoch = 0;
    std::vector<PathStep> paths;
    WalkStop last;
};

std::vector<bool> ancillaryCarriers(const IrProgram& program) {
    std::vector<bool> carries;
    std::vector<const IrValue*> pending;
    const auto mark = [&](const IrValue& value) {
        if (value.Id() >= carries.size()) carries.resize(value.Id() + 1u, false);
        if (carries[value.Id()]) return;
        carries[value.Id()] = true;
        pending.push_back(&value);
    };
    for (const IrBlock* block : program.BlockOrder()) {
        for (const IrValue* inst : block->Instructions()) {
            if (isBuiltin(inst, StageInputKind::PackedAncillary)) mark(*inst);
        }
    }
    while (!pending.empty()) {
        const IrValue* value = pending.back();
        pending.pop_back();
        for (const IrUse& use : value->OperandUses()) {
            const IrOpcode opcode = use.user->Opcode();
            if (opcode == IrOpcode::Identity || use.user->IsPhi() || (isSelect(opcode) && use.operand == 1u)) mark(*use.user);
        }
    }
    return carries;
}

bool keepsAncillary(const std::vector<bool>& carries, const IrValue& select) {
    const IrValue* inactive = select.Argument(2)->Resolve();
    return inactive->Id() < carries.size() && carries[inactive->Id()];
}

std::string tracePrefix(const IrProgram& program) {
    char shader[32];
    std::snprintf(shader, sizeof(shader), "0x%llx", static_cast<unsigned long long>(program.Resources().shaderHash));
    return std::string("[ancillary] shader ") + shader;
}

std::string refusalLine(const IrProgram& program, const IrValue& select, Reason reason, const std::string& detail) {
    return tracePrefix(program) + " select #" + std::to_string(select.Id()) + " kept, " + std::string(MaskedSelectRefusalCode(reason)) + ": " + detail + "; mask " + describeCondition(select.Argument(0), program.WaveSize());
}

std::string dropLine(const IrProgram& program, const IrValue& select, bool everyLane) {
    const char* why = everyLane ? "the mask holds in every lane" : "every use reads it only where the mask holds";
    return tracePrefix(program) + " select #" + std::to_string(select.Id()) + " dropped: " + why + "; mask " + describeCondition(select.Argument(0), program.WaveSize());
}

}

std::string_view MaskedSelectRefusalCode(MaskedSelectRefusalReason reason) {
    switch (reason) {
        case MaskedSelectRefusalReason::VisitLimit: return "visit-limit";
        case MaskedSelectRefusalReason::LoopSelect: return "loop-select";
        case MaskedSelectRefusalReason::LoopMask: return "loop-mask";
        case MaskedSelectRefusalReason::LoopExec: return "loop-exec";
        case MaskedSelectRefusalReason::WholeQuadMode: return "wqm";
        case MaskedSelectRefusalReason::Derivative: return "derivative";
        case MaskedSelectRefusalReason::CrossLane: return "cross-lane";
        case MaskedSelectRefusalReason::ExecReactivated: return "exec-reactivated";
        case MaskedSelectRefusalReason::ObservableUse: return "observable-use";
        case MaskedSelectRefusalReason::ImplicationDepth: return "implication-depth";
        case MaskedSelectRefusalReason::Unreached: return "unreached";
    }
    return "unknown";
}

bool AncillaryTraceEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_ANCILLARY") != nullptr;
    return enabled;
}

bool ReadsPackedAncillary(const IrProgram& program) {
    for (const IrBlock* block : program.BlockOrder()) {
        for (const IrValue* inst : block->Instructions()) {
            if (inst->HasUses() && isBuiltin(inst, StageInputKind::PackedAncillary)) return true;
        }
    }
    return false;
}

std::string AncillarySummaryLine(const IrProgram& program, const MaskedSelectEliminationStats& stats, bool composed) {
    const std::size_t dropped = stats.drops.size();
    return tracePrefix(program) + " summary: " + std::to_string(dropped) + (dropped == 1u ? " select" : " selects") + " dropped, " + std::to_string(stats.refusals.size()) + " kept, whole word " + (composed ? "composed" : "not composed");
}

MaskedSelectEliminationStats MaskedSelectEliminator::Eliminate(IrProgram& program) const {
    auto stats = Eliminate(program, AncillaryTraceEnabled());
    for (const auto& drop : stats.drops) std::fprintf(stderr, "%s\n", drop.line.c_str());
    for (const auto& refusal : stats.refusals) std::fprintf(stderr, "%s\n", refusal.line.c_str());
    return stats;
}

MaskedSelectEliminationStats MaskedSelectEliminator::Eliminate(IrProgram& program, bool explain) const {
    MaskedSelectEliminationStats stats;
    const auto regions = blockRegions(program);
    MaskedLaneWalk walk(program, regions, explain);
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<bool> carries;
        if (explain) {
            carries = ancillaryCarriers(program);
            stats.refusals.clear();
        }
        for (auto blockIt = program.BlockOrder().rbegin(); blockIt != program.BlockOrder().rend(); ++blockIt) {
            const Region region = regionOf(regions, *blockIt);
            const bool single = region == Region::Once;
            auto& instructions = (*blockIt)->Instructions();
            auto it = instructions.end();
            while (it != instructions.begin()) {
                --it;
                IrValue* inst = *it;
                if (!isSelect(inst->Opcode()) || inst->ArgumentCount() != 3u || !inst->HasUses()) continue;
                const IrValue* mask = inst->Argument(0)->Resolve();
                const bool everyLane = alwaysTrue(mask, program.WaveSize());
                if (!everyLane && (!single || !walk.UnobservedWhereMasked(*inst, mask))) {
                    if (explain && keepsAncillary(carries, *inst)) {
                        auto [reason, detail] = single ? walk.Describe(*inst, mask)
                                                       : std::pair{region == Region::Loop ? Reason::LoopSelect : Reason::Unreached, std::string(region == Region::Loop ? "the select is inside a loop" : "the select is in a block the entry does not reach")};
                        stats.refusals.push_back(MaskedSelectRefusal{inst->Id(), reason, refusalLine(program, *inst, reason, detail)});
                    }
                    continue;
                }
                if (explain && keepsAncillary(carries, *inst)) stats.drops.push_back(MaskedSelectDrop{inst->Id(), dropLine(program, *inst, everyLane)});
                inst->ReplaceAllUsesWith(inst->Argument(1));
                inst->Invalidate();
                inst->SetParent(nullptr);
                it = instructions.erase(it);
                ++stats.removedSelects;
                changed = true;
            }
        }
        if (changed) DeadCodeEliminator{}.Eliminate(program);
    }
    return stats;
}

}
