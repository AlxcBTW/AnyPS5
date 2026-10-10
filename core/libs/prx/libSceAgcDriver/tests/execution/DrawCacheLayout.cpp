#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "Recompiler.hpp"
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DispatchVariant;
using AgcDriver::DriverDetail::DrawEntry;
using AgcDriver::DriverDetail::DrawMiss;
using AgcDriver::DriverDetail::DrawProgram;
using AgcDriver::DriverDetail::Driver;
using AgcDriver::DriverDetail::EntryOutcome;
using AgcDriver::Graphics::PipelinePushSlotBytes;
using Role = ShaderRecompiler::ProgramRole;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t VertexPushBytes = 40;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::shared_ptr<DispatchVariant> Variant(std::uint32_t pushOffset, std::size_t pushBytes) {
    auto compiled = std::make_shared<ShaderRecompiler::RecompileResult>();
    compiled->pushConstants.resize(pushBytes);
    auto variant = std::make_shared<DispatchVariant>();
    variant->compiled = std::move(compiled);
    variant->pushOffset = pushOffset;
    return variant;
}

struct Draw {
    std::vector<DrawProgram> programs;
    std::vector<Role> roles{Role::Main, Role::Fragment};

    Draw() : programs(2) {
        programs[0].binary.stage = ShaderStage::Vertex;
        programs[1].binary.stage = ShaderStage::Fragment;
    }
};

struct Match {
    std::optional<DrawMiss> miss;
    std::vector<std::shared_ptr<DispatchVariant>> matched;
    std::vector<std::size_t> ranks;
    std::vector<std::pair<std::size_t, const DispatchVariant*>> validated;
};

Match Run(const DrawEntry& entry, bool fixedSlots, EntryOutcome fragmentOutcome = EntryOutcome::Equal) {
    const Draw draw;
    Match match;
    match.matched.resize(draw.programs.size());
    match.ranks.resize(draw.programs.size());
    std::uint64_t validations = 0, equal = 0;
    match.miss = Driver::MatchDrawEntry(entry, draw.programs, draw.roles, fixedSlots, [&](std::size_t stage, const DispatchVariant& variant) {
        match.validated.emplace_back(stage, &variant);
        return stage == 1 ? fragmentOutcome : EntryOutcome::Equal;
    }, match.matched, match.ranks, validations, equal);
    return match;
}

void Fill(DrawEntry& entry, std::vector<std::shared_ptr<DispatchVariant>> vertex, std::vector<std::shared_ptr<DispatchVariant>> fragment) {
    entry.stages = {std::move(vertex), std::move(fragment)};
}

void FixedSlotFragmentHits() {
    const auto vertex = Variant(0, VertexPushBytes);
    const auto packed = Variant(VertexPushBytes, 16);
    const auto slotted = Variant(PipelinePushSlotBytes, 16);
    DrawEntry entry;
    Fill(entry, {vertex}, {packed, slotted});
    const auto match = Run(entry, true);
    Require(!match.miss, "a draw cached with pipeline libraries missed its own fragment variant at the fixed slot");
    Require(match.matched[0] == vertex && match.matched[1] == slotted && match.ranks[1] == 1, "the fixed-slot lookup picked the wrong fragment variant");
    Require(match.validated.size() == 2 && match.validated[1].second == slotted.get(), "the fixed-slot lookup validated a fragment variant laid out after the vertex push constants");
}

void PackedFragmentHits() {
    const auto vertex = Variant(0, VertexPushBytes);
    const auto packed = Variant(VertexPushBytes, 16);
    const auto slotted = Variant(PipelinePushSlotBytes, 16);
    DrawEntry entry;
    Fill(entry, {vertex}, {slotted, packed});
    const auto match = Run(entry, false);
    Require(!match.miss && match.matched[1] == packed && match.ranks[1] == 1, "without pipeline libraries the lookup did not pick the fragment variant right after the vertex push constants");
    Require(match.validated.size() == 2 && match.validated[1].second == packed.get(), "without pipeline libraries the lookup validated a fragment variant at the fixed slot");
}

void MisplacedFragmentIsLayoutMiss() {
    DrawEntry entry;
    Fill(entry, {Variant(0, VertexPushBytes)}, {Variant(VertexPushBytes, 16)});
    const auto match = Run(entry, true);
    Require(match.miss == DrawMiss::Layout && match.matched[1] == nullptr, "a fragment variant outside the fixed slot was not a layout miss");
}

void FragmentOutcomes() {
    DrawEntry entry;
    Fill(entry, {Variant(0, VertexPushBytes)}, {Variant(PipelinePushSlotBytes, 16)});
    Require(Run(entry, true, EntryOutcome::Differing).miss == DrawMiss::FragmentDiffering, "a differing fragment variant was not a fragment miss");
    Require(Run(entry, true, EntryOutcome::PublishMoved).miss == DrawMiss::Gate, "a fragment variant refused by a gate was not a gate miss");
    DrawEntry single;
    single.stages = {{Variant(0, VertexPushBytes)}};
    Require(Run(single, true).miss == DrawMiss::Stages, "an entry with another stage count was not a stage-count miss");
}

}

int main() {
    try {
        FixedSlotFragmentHits();
        PackedFragmentHits();
        MisplacedFragmentIsLayoutMiss();
        FragmentOutcomes();
        std::cout << "draw cache layout tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
