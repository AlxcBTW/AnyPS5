// Differential check of MaskedSelectEliminator: random wave programs (exec saved, narrowed, killed,
// revived and restored around diamonds and loops) run on a small lane interpreter before and after
// the pass. A select may only go if every observable effect stays the same: stores and loads gated by
// their exec, cross-lane reads (readfirstlane, readlane, ballot), branch decisions and plain uses.
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/MaskedSelectEliminator.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ShaderRecompiler;

namespace {

using Word4 = std::array<std::uint32_t, 4>;

class Random {
public:
    explicit Random(std::uint64_t seed) : engine(seed) {}
    std::uint32_t Below(std::uint32_t count) { return std::uniform_int_distribution<std::uint32_t>(0u, count - 1u)(engine); }
    bool Chance(std::uint32_t percent) { return Below(100u) < percent; }

private:
    std::mt19937_64 engine;
};

constexpr std::size_t Vgprs = 4;
constexpr std::size_t Slots = 3;
constexpr std::size_t Exec = Vgprs;
constexpr std::size_t Lo = Vgprs + 1u;
constexpr std::size_t Hi = Vgprs + 2u;
constexpr std::size_t FirstSlot = Vgprs + 3u;
constexpr std::size_t Fields = FirstSlot + 3u * Slots;
using Env = std::array<IrValue*, Fields>;

bool boolField(std::size_t field) {
    return field == Exec || (field >= FirstSlot && (field - FirstSlot) % 3u == 0u);
}

class Generator {
public:
    Generator(IrProgram& program, std::uint64_t seed, std::uint32_t wave) : program(program), random(seed), wave(wave) {}

    void Build() {
        program.SetWaveSize(wave);
        block = &newBlock();
        program.SetEntryBlock(*block);
        Env env{};
        for (std::size_t index = 0; index < Vgprs; ++index) {
            env[index] = &emit(IrOpcode::IAdd32, IrType::U32, {&emit(IrOpcode::IMul32, IrType::U32, {&lane(), &constant(1u + random.Below(97u))}), &constant(random.Below(1000u))});
        }
        IrValue& initial = random.Chance(25) ? constantBool(true) : emit(IrOpcode::ULessThan32, IrType::Bool, {&lane(), &constant(1u + random.Below(wave))});
        execFromBool(env, initial);
        for (std::size_t slot = 0; slot < Slots; ++slot) save(env, slot);
        budget = 20 + static_cast<int>(random.Below(100u));
        sequence(env, 0);
        for (std::size_t index = 0; index < Vgprs; ++index) {
            (void)emit(IrOpcode::WriteSharedU32, IrType::Void, {&lane(), env[index], env[Exec]});
        }
    }

private:
    IrBlock& newBlock() {
        auto& created = program.CreateBlock();
        program.BlockOrder().push_back(&created);
        return created;
    }

    IrValue& constant(std::uint32_t value) {
        auto& created = program.CreateValue(IrOpcode::Void, IrType::U32);
        created.SetImmediateU32(value);
        return created;
    }

    IrValue& constantBool(bool value) {
        auto& created = program.CreateValue(IrOpcode::Void, IrType::Bool);
        created.SetImmediateBool(value);
        return created;
    }

    IrValue& emit(IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments) {
        auto& created = program.CreateValue(opcode, type, 0u);
        for (auto* argument : arguments) created.AddArgument(argument);
        block->AppendInstruction(&created);
        return created;
    }

    IrValue& phi(IrType type) {
        auto& created = program.CreateValue(IrOpcode::Phi, type);
        block->AppendInstruction(&created);
        return created;
    }

    IrValue& lane() {
        return emit(IrOpcode::LaneId, IrType::U32, {});
    }

    // The translator's lane bit of an exec word pair (TranslationContext::threadBit).
    IrValue& threadBit(IrValue& low, IrValue& high) {
        auto& laneId = lane();
        IrValue* word = &low;
        if (wave == 64u) word = &emit(IrOpcode::SelectU32, IrType::U32, {&emit(IrOpcode::ULessThan32, IrType::Bool, {&laneId, &constant(32u)}), &low, &high});
        auto& shift = emit(IrOpcode::BitwiseAnd32, IrType::U32, {&laneId, &constant(31u)});
        auto& shifted = emit(IrOpcode::ShiftRightLogical32, IrType::U32, {word, &shift});
        auto& bit = emit(IrOpcode::BitwiseAnd32, IrType::U32, {&shifted, &constant(1u)});
        return emit(IrOpcode::INotEqual32, IrType::Bool, {&bit, &constant(0u)});
    }

    void execFromBool(Env& env, IrValue& value) {
        auto& ballot = emit(IrOpcode::Ballot, IrType::U32x4, {&value});
        env[Exec] = &value;
        env[Lo] = &emit(IrOpcode::CompositeExtractU32x4, IrType::U32, {&ballot, &constant(0u)});
        env[Hi] = wave == 64u ? &emit(IrOpcode::CompositeExtractU32x4, IrType::U32, {&ballot, &constant(1u)}) : &constant(0u);
    }

    void execFromWords(Env& env, IrValue& low, IrValue& high) {
        env[Lo] = &low;
        env[Hi] = &high;
        env[Exec] = &threadBit(low, high);
    }

    void save(Env& env, std::size_t slot) {
        env[FirstSlot + 3u * slot] = env[Exec];
        env[FirstSlot + 3u * slot + 1u] = env[Lo];
        env[FirstSlot + 3u * slot + 2u] = env[Hi];
    }

    IrValue& vgpr(Env& env) {
        return *env[random.Below(Vgprs)];
    }

    IrValue& operand(Env& env) {
        return random.Chance(70) ? vgpr(env) : constant(random.Below(64u));
    }

    IrValue& predicate(Env& env) {
        switch (random.Below(3u)) {
            case 0: return emit(IrOpcode::ULessThan32, IrType::Bool, {&vgpr(env), &operand(env)});
            case 1: return emit(IrOpcode::INotEqual32, IrType::Bool, {&emit(IrOpcode::BitwiseAnd32, IrType::U32, {&vgpr(env), &constant(1u << random.Below(4u))}), &constant(0u)});
            default: return emit(IrOpcode::ULessThan32, IrType::Bool, {&lane(), &constant(random.Below(wave + 1u))});
        }
    }

    void write(Env& env, std::size_t target, IrValue& value) {
        env[target] = &emit(IrOpcode::SelectU32, IrType::U32, {env[Exec], &value, env[target]});
    }

    IrValue& uniformCondition(Env& env) {
        switch (random.Below(3u)) {
            case 0: return emit(IrOpcode::INotEqual32, IrType::Bool, {&emit(IrOpcode::BitwiseOr32, IrType::U32, {env[Lo], env[Hi]}), &constant(0u)});
            case 1: {
                auto& first = emit(IrOpcode::ReadFirstLane, IrType::U32, {&vgpr(env), env[Exec]});
                return emit(IrOpcode::INotEqual32, IrType::Bool, {&emit(IrOpcode::BitwiseAnd32, IrType::U32, {&first, &constant(1u)}), &constant(0u)});
            }
            default: return emit(IrOpcode::ULessThan32, IrType::Bool, {&constant(random.Below(4u)), &constant(random.Below(4u))});
        }
    }

    void statement(Env& env, int depth) {
        const std::uint32_t choice = random.Below(100u);
        const std::size_t target = random.Below(Vgprs);
        const std::size_t slot = random.Below(Slots);
        IrValue& slotExec = *env[FirstSlot + 3u * slot];
        IrValue& slotLo = *env[FirstSlot + 3u * slot + 1u];
        IrValue& slotHi = *env[FirstSlot + 3u * slot + 2u];
        if (choice < 26) {
            static constexpr std::array<IrOpcode, 4> operations{IrOpcode::IAdd32, IrOpcode::IMul32, IrOpcode::BitwiseXor32, IrOpcode::ISub32};
            write(env, target, emit(operations[random.Below(4u)], IrType::U32, {&vgpr(env), &operand(env)}));
        } else if (choice < 30) {
            write(env, target, emit(IrOpcode::SelectU32, IrType::U32, {&predicate(env), &vgpr(env), &operand(env)}));
        } else if (choice < 37) {
            execFromBool(env, emit(IrOpcode::LogicalAnd, IrType::Bool, {env[Exec], &predicate(env)}));
        } else if (choice < 40) {
            execFromBool(env, emit(IrOpcode::LogicalAnd, IrType::Bool, {env[Exec], &emit(IrOpcode::LogicalNot, IrType::Bool, {&predicate(env)})}));
        } else if (choice < 45) {
            save(env, slot);
        } else if (choice < 49) {
            execFromWords(env, slotLo, slotHi);
        } else if (choice < 51) {
            env[Exec] = &slotExec;
            env[Lo] = &slotLo;
            env[Hi] = &slotHi;
        } else if (choice < 54) {
            execFromWords(env, emit(IrOpcode::BitwiseAnd32, IrType::U32, {env[Lo], &slotLo}), emit(IrOpcode::BitwiseAnd32, IrType::U32, {env[Hi], &slotHi}));
        } else if (choice < 56) {
            execFromWords(env, emit(IrOpcode::BitwiseOr32, IrType::U32, {env[Lo], &slotLo}), emit(IrOpcode::BitwiseOr32, IrType::U32, {env[Hi], &slotHi}));
        } else if (choice < 58) {
            execFromWords(env, emit(IrOpcode::BitwiseAnd32, IrType::U32, {&slotLo, &emit(IrOpcode::BitwiseNot32, IrType::U32, {env[Lo]})}),
                          emit(IrOpcode::BitwiseAnd32, IrType::U32, {&slotHi, &emit(IrOpcode::BitwiseNot32, IrType::U32, {env[Hi]})}));
        } else if (choice < 60) {
            execFromBool(env, emit(IrOpcode::LogicalAnd, IrType::Bool, {env[Exec], &slotExec}));
        } else if (choice < 68) {
            IrValue& active = random.Chance(80) ? *env[Exec] : slotExec;
            (void)emit(IrOpcode::WriteSharedU32, IrType::Void, {&emit(IrOpcode::BitwiseAnd32, IrType::U32, {&operand(env), &constant(63u)}), &vgpr(env), &active});
        } else if (choice < 72) {
            IrValue& loaded = emit(IrOpcode::LoadSharedU32, IrType::U32, {&emit(IrOpcode::BitwiseAnd32, IrType::U32, {&vgpr(env), &constant(63u)}), env[Exec]});
            write(env, target, loaded);
        } else if (choice < 75) {
            auto& address = emit(IrOpcode::MakeImageAddress, IrType::ImageAddress, {&vgpr(env), &operand(env)});
            auto& read = emit(IrOpcode::ImageRead, IrType::U32x4, {&emit(IrOpcode::GetImageResource, IrType::ImageResource, {}), &address, env[Exec]});
            write(env, target, emit(IrOpcode::CompositeExtractU32x4, IrType::U32, {&read, &constant(random.Below(4u))}));
        } else if (choice < 78) {
            auto& compare = emit(IrOpcode::LogicalAnd, IrType::Bool, {env[Exec], &predicate(env)});
            auto& ballot = emit(IrOpcode::Ballot, IrType::U32x4, {&compare});
            (void)emit(IrOpcode::ReferenceU32, IrType::Void, {&emit(IrOpcode::CompositeExtractU32x4, IrType::U32, {&ballot, &constant(wave == 64u ? random.Below(2u) : 0u)})});
        } else if (choice < 84) {
            // v_readfirstlane / v_readlane into an SGPR, then either a scalar use or a v_mov back under exec.
            IrValue& read = choice < 81 ? emit(IrOpcode::ReadFirstLane, IrType::U32, {&vgpr(env), env[Exec]}) : emit(IrOpcode::ReadLane, IrType::U32, {&vgpr(env), &constant(random.Below(wave))});
            if (random.Chance(50)) (void)emit(IrOpcode::ReferenceU32, IrType::Void, {&read});
            else write(env, target, read);
        } else if (choice < 85) {
            (void)emit(IrOpcode::ReferenceU32, IrType::Void, {&vgpr(env)});
        } else if (choice < 91 && depth < 3) {
            ifRegion(env, depth);
        } else if (choice < 95 && depth < 3) {
            diamond(env, depth);
        } else if (depth < 3) {
            loop(env, depth);
        }
    }

    void sequence(Env& env, int depth) {
        const int count = 1 + static_cast<int>(random.Below(depth == 0 ? 14u : 7u));
        for (int index = 0; index < count && budget > 0; ++index, --budget) statement(env, depth);
    }

    void merge(Env& env, IrBlock& leftEnd, const Env& left, IrBlock& rightEnd, const Env& right) {
        IrBlock& joined = newBlock();
        leftEnd.AddBranch(&joined);
        rightEnd.AddBranch(&joined);
        block = &joined;
        for (std::size_t field = 0; field < Fields; ++field) {
            if (left[field] == right[field]) {
                env[field] = left[field];
                continue;
            }
            auto& merged = phi(boolField(field) ? IrType::Bool : IrType::U32);
            merged.AddPhiOperand(&leftEnd, left[field]);
            merged.AddPhiOperand(&rightEnd, right[field]);
            env[field] = &merged;
        }
    }

    void branch(IrValue& condition, IrBlock& taken, IrBlock& other) {
        (void)emit(IrOpcode::Reference, IrType::Void, {&condition});
        block->AddBranch(&taken);
        block->AddBranch(&other);
    }

    void diamond(Env& env, int depth) {
        IrBlock& thenBlock = newBlock();
        IrBlock& elseBlock = newBlock();
        branch(uniformCondition(env), thenBlock, elseBlock);
        Env left = env;
        block = &thenBlock;
        sequence(left, depth + 1);
        IrBlock& leftEnd = *block;
        Env right = env;
        block = &elseBlock;
        if (random.Chance(60)) sequence(right, depth + 1);
        IrBlock& rightEnd = *block;
        merge(env, leftEnd, left, rightEnd, right);
    }

    // s_and_saveexec, s_cbranch_execz over the body, an optional else under s_andn2, then the restore.
    void ifRegion(Env& env, int depth) {
        const std::size_t slot = random.Below(Slots);
        save(env, slot);
        execFromBool(env, emit(IrOpcode::LogicalAnd, IrType::Bool, {env[Exec], &predicate(env)}));
        IrBlock& body = newBlock();
        IrBlock& skip = newBlock();
        branch(emit(IrOpcode::INotEqual32, IrType::Bool, {&emit(IrOpcode::BitwiseOr32, IrType::U32, {env[Lo], env[Hi]}), &constant(0u)}), body, skip);
        Env inside = env;
        block = &body;
        sequence(inside, depth + 1);
        IrBlock& bodyEnd = *block;
        Env outside = env;
        block = &skip;
        IrBlock& skipEnd = *block;
        merge(env, bodyEnd, inside, skipEnd, outside);
        IrValue& savedLo = *env[FirstSlot + 3u * slot + 1u];
        IrValue& savedHi = *env[FirstSlot + 3u * slot + 2u];
        if (random.Chance(30)) {
            execFromWords(env, emit(IrOpcode::BitwiseAnd32, IrType::U32, {&savedLo, &emit(IrOpcode::BitwiseNot32, IrType::U32, {env[Lo]})}),
                          emit(IrOpcode::BitwiseAnd32, IrType::U32, {&savedHi, &emit(IrOpcode::BitwiseNot32, IrType::U32, {env[Hi]})}));
            sequence(env, depth + 1);
        }
        if (random.Chance(85)) execFromWords(env, *env[FirstSlot + 3u * slot + 1u], *env[FirstSlot + 3u * slot + 2u]);
    }

    void loop(Env& env, int depth) {
        IrBlock& preheader = *block;
        IrBlock& header = newBlock();
        preheader.AddBranch(&header);
        block = &header;
        Env carried{};
        for (std::size_t field = 0; field < Fields; ++field) {
            carried[field] = &phi(boolField(field) ? IrType::Bool : IrType::U32);
            carried[field]->AddPhiOperand(&preheader, env[field]);
        }
        IrValue& counter = phi(IrType::U32);
        counter.AddPhiOperand(&preheader, &constant(0u));
        Env body = carried;
        sequence(body, depth + 1);
        IrBlock& latch = *block;
        auto& next = emit(IrOpcode::IAdd32, IrType::U32, {&counter, &constant(1u)});
        IrValue* again = &emit(IrOpcode::ULessThan32, IrType::Bool, {&next, &constant(1u + random.Below(4u))});
        if (random.Chance(40)) {
            auto& any = emit(IrOpcode::INotEqual32, IrType::Bool, {&emit(IrOpcode::BitwiseOr32, IrType::U32, {body[Lo], body[Hi]}), &constant(0u)});
            again = &emit(IrOpcode::LogicalAnd, IrType::Bool, {again, &any});
        }
        IrBlock& exit = newBlock();
        branch(*again, header, exit);
        for (std::size_t field = 0; field < Fields; ++field) carried[field]->AddPhiOperand(&latch, body[field]);
        counter.AddPhiOperand(&latch, &next);
        block = &exit;
        env = body;
    }

    IrProgram& program;
    Random random;
    std::uint32_t wave;
    IrBlock* block = nullptr;
    int budget = 0;
};

struct Event {
    std::uint32_t instruction;
    std::uint32_t lane;
    std::uint32_t value;
    bool operator==(const Event&) const = default;
};

class Interpreter {
public:
    explicit Interpreter(std::uint32_t wave) : wave(wave) {}

    bool Run(IrProgram& program, std::vector<Event>& trace, std::string& error) {
        IrBlock* current = &program.EntryBlock();
        IrBlock* previous = nullptr;
        for (std::uint32_t steps = 0; steps < 4096u; ++steps) {
            std::vector<std::pair<const IrValue*, std::vector<Word4>>> incoming;
            for (IrValue* inst : current->Instructions()) {
                if (!inst->IsPhi()) continue;
                const IrValue* source = nullptr;
                for (std::size_t index = 0; index < inst->PhiBlockCount(); ++index) {
                    if (inst->PhiBlock(index) == previous) source = inst->Argument(index);
                }
                if (source == nullptr) {
                    error = "phi without an incoming value for its predecessor";
                    return false;
                }
                incoming.emplace_back(inst, lanes(source));
            }
            for (auto& [value, data] : incoming) values[value] = std::move(data);
            for (IrValue* inst : current->Instructions()) {
                if (!inst->IsPhi()) execute(*inst, trace);
            }
            const auto& successors = current->Successors();
            if (successors.empty()) return true;
            previous = current;
            if (successors.size() == 1u) {
                current = successors.front();
                continue;
            }
            const IrValue* last = current->Instructions().empty() ? nullptr : current->Instructions().back();
            if (last == nullptr || last->Opcode() != IrOpcode::Reference) {
                error = "conditional block does not end in its branch condition";
                return false;
            }
            const auto condition = lanes(last->Argument(0));
            for (std::uint32_t laneIndex = 1; laneIndex < wave; ++laneIndex) {
                if ((condition[laneIndex][0] != 0u) != (condition[0][0] != 0u)) {
                    error = "divergent branch condition";
                    return false;
                }
            }
            current = condition[0][0] != 0u ? successors[0] : successors[1];
        }
        error = "step limit";
        return false;
    }

private:
    std::vector<Word4> lanes(const IrValue* value) {
        value = value->Resolve();
        if (value->HasImmediate()) return std::vector<Word4>(wave, Word4{value->ImmediateU32(), 0u, 0u, 0u});
        const auto found = values.find(value);
        if (found == values.end()) throw std::runtime_error(std::string("read of an undefined ") + std::string(IrOpcodeName(value->Opcode())));
        return found->second;
    }

    void execute(const IrValue& inst, std::vector<Event>& trace) {
        std::vector<std::vector<Word4>> arguments;
        for (std::size_t index = 0; index < inst.ArgumentCount(); ++index) arguments.push_back(lanes(inst.Argument(index)));
        std::vector<Word4> result(wave, Word4{});
        const auto arg = [&](std::size_t index, std::uint32_t laneIndex) { return arguments[index][laneIndex][0]; };
        const std::uint32_t id = inst.Id();
        switch (inst.Opcode()) {
            case IrOpcode::LaneId:
                for (std::uint32_t l = 0; l < wave; ++l) result[l][0] = l;
                break;
            case IrOpcode::IAdd32:
            case IrOpcode::ISub32:
            case IrOpcode::IMul32:
            case IrOpcode::BitwiseAnd32:
            case IrOpcode::BitwiseOr32:
            case IrOpcode::BitwiseXor32:
            case IrOpcode::ShiftRightLogical32:
            case IrOpcode::ULessThan32:
            case IrOpcode::INotEqual32:
            case IrOpcode::IEqual32:
            case IrOpcode::LogicalAnd:
            case IrOpcode::LogicalOr:
                for (std::uint32_t l = 0; l < wave; ++l) {
                    const std::uint32_t a = arg(0, l);
                    const std::uint32_t b = arg(1, l);
                    std::uint32_t r = 0;
                    switch (inst.Opcode()) {
                        case IrOpcode::IAdd32: r = a + b; break;
                        case IrOpcode::ISub32: r = a - b; break;
                        case IrOpcode::IMul32: r = a * b; break;
                        case IrOpcode::BitwiseAnd32: r = a & b; break;
                        case IrOpcode::BitwiseOr32: r = a | b; break;
                        case IrOpcode::BitwiseXor32: r = a ^ b; break;
                        case IrOpcode::ShiftRightLogical32: r = a >> (b & 31u); break;
                        case IrOpcode::ULessThan32: r = a < b ? 1u : 0u; break;
                        case IrOpcode::INotEqual32: r = a != b ? 1u : 0u; break;
                        case IrOpcode::IEqual32: r = a == b ? 1u : 0u; break;
                        case IrOpcode::LogicalAnd: r = (a != 0u && b != 0u) ? 1u : 0u; break;
                        case IrOpcode::LogicalOr: r = (a != 0u || b != 0u) ? 1u : 0u; break;
                        default: break;
                    }
                    result[l][0] = r;
                }
                break;
            case IrOpcode::BitwiseNot32:
                for (std::uint32_t l = 0; l < wave; ++l) result[l][0] = ~arg(0, l);
                break;
            case IrOpcode::LogicalNot:
                for (std::uint32_t l = 0; l < wave; ++l) result[l][0] = arg(0, l) != 0u ? 0u : 1u;
                break;
            case IrOpcode::SelectU1:
            case IrOpcode::SelectU32:
                for (std::uint32_t l = 0; l < wave; ++l) result[l] = arguments[arg(0, l) != 0u ? 1 : 2][l];
                break;
            case IrOpcode::Ballot: {
                Word4 bits{};
                for (std::uint32_t l = 0; l < wave; ++l) {
                    if (arg(0, l) != 0u) bits[l / 32u] |= 1u << (l % 32u);
                }
                for (std::uint32_t l = 0; l < wave; ++l) result[l] = bits;
                break;
            }
            case IrOpcode::CompositeExtractU32x4:
                for (std::uint32_t l = 0; l < wave; ++l) result[l][0] = arguments[0][l][arg(1, l) & 3u];
                break;
            case IrOpcode::ReadFirstLane: {
                std::uint32_t value = 0;
                for (std::uint32_t l = 0; l < wave; ++l) {
                    if (arg(1, l) != 0u) {
                        value = arg(0, l);
                        break;
                    }
                }
                for (std::uint32_t l = 0; l < wave; ++l) result[l][0] = value;
                break;
            }
            case IrOpcode::ReadLane: {
                const std::uint32_t value = arg(0, arg(1, 0) % wave);
                for (std::uint32_t l = 0; l < wave; ++l) result[l][0] = value;
                break;
            }
            case IrOpcode::WriteSharedU32:
                for (std::uint32_t l = 0; l < wave; ++l) {
                    if (arg(2, l) == 0u) continue;
                    shared[arg(0, l) & 63u] = arg(1, l);
                    trace.push_back(Event{id, l, arg(1, l)});
                }
                break;
            case IrOpcode::LoadSharedU32:
                for (std::uint32_t l = 0; l < wave; ++l) result[l][0] = arg(1, l) != 0u ? shared[arg(0, l) & 63u] : 0u;
                break;
            case IrOpcode::GetImageResource:
                break;
            case IrOpcode::MakeImageAddress:
                for (std::uint32_t l = 0; l < wave; ++l) result[l] = Word4{arg(0, l), arg(1, l), 0u, 0u};
                break;
            case IrOpcode::ImageRead:
                for (std::uint32_t l = 0; l < wave; ++l) {
                    if (arg(2, l) == 0u) continue;
                    const std::uint32_t x = arguments[1][l][0];
                    const std::uint32_t y = arguments[1][l][1];
                    const std::uint32_t texel = (x * 2654435761u) ^ (y * 40503u + 0x9e3779b9u);
                    result[l] = Word4{texel, texel + 1u, texel ^ 0x55u, texel >> 3u};
                }
                break;
            case IrOpcode::ReferenceU32:
                for (std::uint32_t l = 0; l < wave; ++l) trace.push_back(Event{id, l, arg(0, l)});
                break;
            case IrOpcode::Reference:
                trace.push_back(Event{id, 0u, arg(0, 0)});
                break;
            default:
                throw std::runtime_error(std::string("interpreter has no rule for ") + std::string(IrOpcodeName(inst.Opcode())));
        }
        values[&inst] = std::move(result);
    }

    std::uint32_t wave;
    std::unordered_map<const IrValue*, std::vector<Word4>> values;
    std::array<std::uint32_t, 64> shared{};
};

struct Outcome {
    bool ran = false;
    std::vector<Event> trace;
    std::string error;
};

Outcome interpret(IrProgram& program, std::uint32_t wave) {
    Outcome outcome;
    try {
        Interpreter interpreter(wave);
        outcome.ran = interpreter.Run(program, outcome.trace, outcome.error);
    } catch (const std::exception& error) {
        outcome.error = error.what();
    }
    return outcome;
}

}

int main(int argc, char** argv) {
    const std::uint64_t first = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 1u;
    const std::uint64_t count = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 4000u;
    const bool verbose = argc > 3;
    std::uint64_t programs = 0;
    std::uint64_t removed = 0;
    std::uint64_t changed = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t skipped = 0;
    for (std::uint64_t seed = first; seed < first + count; ++seed) {
        for (const std::uint32_t wave : {32u, 64u}) {
            IrProgram original;
            Generator(original, seed, wave).Build();
            IrProgram optimized;
            Generator(optimized, seed, wave).Build();
            std::uint32_t gone = 0;
            try {
                gone = MaskedSelectEliminator{}.Eliminate(optimized).removedSelects;
            } catch (const std::exception& error) {
                std::printf("seed %llu wave %u: the pass threw: %s\n", static_cast<unsigned long long>(seed), wave, error.what());
                ++mismatches;
                continue;
            }
            const Outcome before = interpret(original, wave);
            if (!before.ran) {
                ++skipped;
                if (verbose) std::printf("seed %llu wave %u: generator produced an unusable program: %s\n", static_cast<unsigned long long>(seed), wave, before.error.c_str());
                continue;
            }
            const Outcome after = interpret(optimized, wave);
            ++programs;
            removed += gone;
            if (gone != 0u) ++changed;
            if (after.ran && after.trace == before.trace) continue;
            ++mismatches;
            std::printf("MISMATCH seed %llu wave %u: %u selects removed, %s\n", static_cast<unsigned long long>(seed), wave, gone, after.ran ? "different observable trace" : after.error.c_str());
            if (after.ran) {
                const std::size_t limit = std::min(before.trace.size(), after.trace.size());
                std::size_t index = 0;
                while (index < limit && before.trace[index] == after.trace[index]) ++index;
                if (index < limit) {
                    std::printf("  first difference at event %zu: instruction %%%u lane %u value %u before, instruction %%%u lane %u value %u after\n", index, before.trace[index].instruction, before.trace[index].lane, before.trace[index].value,
                                after.trace[index].instruction, after.trace[index].lane, after.trace[index].value);
                } else {
                    std::printf("  trace lengths %zu before, %zu after\n", before.trace.size(), after.trace.size());
                }
            }
            if (verbose) std::printf("---- before\n%s\n---- after\n%s\n", ProgramToString(original).c_str(), ProgramToString(optimized).c_str());
        }
    }
    std::printf("%llu programs, %llu changed by the pass, %llu selects removed, %llu skipped, %llu mismatches\n", static_cast<unsigned long long>(programs), static_cast<unsigned long long>(changed),
                static_cast<unsigned long long>(removed), static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(mismatches));
    return mismatches == 0u ? 0 : 1;
}
