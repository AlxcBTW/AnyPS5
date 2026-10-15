#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/SpeculativeCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace AgcDriver::DriverDetail {

namespace {

std::atomic<std::uint64_t> speculativeCaptures{0}, filledCaptures{0}, filledWords{0}, repeatedCaptures{0}, refusedCaptures{0};

// Whether the stage binding of FlattenedSrt holds every filled word (the walk pure slots are
// indices into it).
bool FillsFit(const ShaderRecompiler::RecompileResult& compiled, std::span<const Graphics::SrtFill> fills) {
    const ShaderRecompiler::DescriptorBinding* flat = nullptr;
    for (const auto& binding : compiled.bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
        if (flat != nullptr) return false;
        flat = &binding;
    }
    return flat != nullptr && std::all_of(fills.begin(), fills.end(), [&](const Graphics::SrtFill& fill) { return fill.word < flat->guestDescriptor.size(); });
}

// APS5_SPECULATIVE_CAPTURE=verify: the speculative materialization against the exact one. They
// must agree on everything but the filled words, which the draw overwrites; stale and fresh count
// the filled words whose exact value differed from the speculated one or not.
const char* DiffersOutsideFills(const ShaderRecompiler::RecompileResult& speculative, const ShaderRecompiler::RecompileResult& exact, std::span<const Graphics::SrtFill> fills, std::uint64_t& stale, std::uint64_t& fresh) {
    if (speculative.variantId != exact.variantId) return "variant";
    if (speculative.pushConstants != exact.pushConstants) return "push constants";
    if (speculative.bindings.size() != exact.bindings.size()) return "bindings";
    for (std::size_t b = 0; b < speculative.bindings.size(); ++b) {
        const auto& left = speculative.bindings[b];
        const auto& right = exact.bindings[b];
        if (left.kind != right.kind || left.role != right.role || left.binding != right.binding || left.count != right.count || left.guestDescriptor.size() != right.guestDescriptor.size()) return "bindings";
        for (std::size_t word = 0; word < left.guestDescriptor.size(); ++word) {
            if (left.guestDescriptor[word] == right.guestDescriptor[word]) continue;
            const bool filled = left.role == ShaderRecompiler::DescriptorRole::FlattenedSrt && std::any_of(fills.begin(), fills.end(), [&](const Graphics::SrtFill& fill) { return fill.word == word; });
            if (!filled) return left.role == ShaderRecompiler::DescriptorRole::FlattenedSrt ? "flattened SRT outside the fills" : "descriptor words";
        }
    }
    for (const auto& fill : fills) {
        for (std::size_t b = 0; b < speculative.bindings.size(); ++b) {
            if (speculative.bindings[b].role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
            ++(speculative.bindings[b].guestDescriptor[fill.word] != exact.bindings[b].guestDescriptor[fill.word] ? stale : fresh);
        }
    }
    return nullptr;
}


}

Driver::SrtFillCaptureCounts Driver::TakeSrtFillCaptureCounts() {
    return {speculativeCaptures.exchange(0, std::memory_order_relaxed), filledCaptures.exchange(0, std::memory_order_relaxed), filledWords.exchange(0, std::memory_order_relaxed), repeatedCaptures.exchange(0, std::memory_order_relaxed), refusedCaptures.exchange(0, std::memory_order_relaxed)};
}

ShaderRecompiler::RecompileResult Driver::materializeDrawStage(std::size_t i, std::uint32_t pushOffset, const QueueState& queue, const Submission& submission, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, std::vector<ShaderRecompiler::MemoryRegion>& memory, const std::vector<ShaderRecompiler::LinkedProgram>& linked, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, ShaderMemory& shaderMemory, std::vector<StageCapture>& stageCaptures, std::vector<bool>& recompiled, bool drawHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool profile, std::uint64_t dumpTarget, std::uint64_t dumpSlot1, std::uint64_t& captures, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, std::string& rejected) {
    PerformanceTimer timing("Shader.DrawStage");
    using Stage = ShaderRecompiler::ShaderStage;
    phaseTiming.Phase(DrawRowVectors);
    const auto& program = programs[i];
    const auto waveSize = program.binary.stage == Stage::Fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
    ShaderRecompiler::RecompileRequest request{
        program.binary,
        {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? std::optional(pixel) : std::nullopt, vertexInfos[i], memory, RegisteredFloatMode(*program.snapshot)},
        localDevice->Target(),
        {0, 0, pushOffset, (graphics.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : Graphics::PipelinePushSlotBytes) - pushOffset % Graphics::PipelinePushSlotBytes},
        ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation, {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}}
    };
    const auto waitedBefore = traceCapSync() || profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
    const auto invocation = InvocationFor(*program.snapshot, program.codeOffset, request);
    timing.Mark("prepared_invocation");
    auto& stageCapture = stageCaptures[i];
    stageCapture.forgetSerial = GuestMemory::ForgetSerial();
    stageCapture.pushOffset = pushOffset;
    // A speculating draw (Driver::draw) reads a word that recorded GPU work still writes without
    // waiting for it. The capture stands when every such word is only the leaf of pure flat-SRT slots
    // (SpeculatedLeafSlots: the walk consumed none of them); the draw then fills those FlattenedSrt
    // words from guest memory on the GPU (Graphics::Draw), after the writer, and the stale bytes in
    // the snapshot never reach a shader. If only some words fail, a second attempt reads just those
    // exactly; a capture where none could stay speculated is repeated whole, and its program then
    // skips speculation for a while.
    stageCapture.srtFills.clear();
    std::vector<std::uint64_t> speculated;
    std::vector<std::uint32_t> speculatedValues;
    const bool wasSpeculating = shaderMemory.Speculating();
    bool speculate = wasSpeculating;
    if (speculate && Speculation::Refused(program.binary.codeAddress)) {
        ++refusedCaptures;
        speculate = false;
    }
    const auto captureStage = [&](bool speculating) {
        shaderMemory.Speculate(speculating);
        shaderMemory.AllowSpeculation();
        stageCapture.srtFills.clear();
        speculated.clear();
        speculatedValues.clear();
        const auto exact = [&] {
            shaderMemory.Speculate(false);
            shaderMemory.AllowSpeculation();
            const SampledReadScope sampling(evidenceReads);
            return shaderMemory.Capture(invocation);
        };
        if (!speculating) return exact();
        resolveSpeculations();
        ++speculativeCaptures;
        reportSpeculation();
        // A first attempt speculates every word the query calls volatile. If some of them were
        // consumed by the walk (or are no pure leaf), a second attempt reads just those exactly and
        // keeps the others speculated; only a capture where nothing could stay speculated is
        // repeated whole, and its program then skips speculation for a while.
        std::vector<std::uint64_t> forbidden;
        for (int attempt = 0; attempt < 2; ++attempt) {
            shaderMemory.ForbidSpeculation(forbidden);
            std::shared_ptr<const ShaderRecompiler::ResourceCapture> speculative;
            bool threw = false;
            try {
                const SampledReadScope sampling(evidenceReads);
                speculative = shaderMemory.Capture(invocation);
            } catch (...) {
                speculated = shaderMemory.TakeSpeculated(&speculatedValues);
                if (speculated.empty()) {
                    shaderMemory.Speculate(wasSpeculating);
                    throw;
                }
                threw = true;
            }
            if (!threw) {
                speculated = shaderMemory.TakeSpeculated(&speculatedValues);
                if (speculated.empty()) return speculative;
            }
            std::optional<RefusalReason> structural;
            std::vector<std::uint64_t> failing;
            bool consumed = false;
            if (threw) {
                failing = speculated;
            } else {
                const auto& plan = *speculative->plan;
                if (plan.requiresSpecializationMemory || std::any_of(plan.cleanFlatSlots.begin(), plan.cleanFlatSlots.end(), [](std::uint8_t clean) { return clean != 0u; })) structural = RefusalReason::PlanUnsafe;
                else if (speculative->readTrace.leaves.empty()) structural = RefusalReason::NoPureSlots;
                else failing = SpeculationFailures(speculated, speculative->readTrace.leaves, speculative->readTrace.otherReads, consumed);
            }
            if (!threw && !structural && failing.empty()) {
                const auto slots = SpeculatedLeafSlots(speculated, speculative->readTrace.leaves, speculative->readTrace.otherReads);
                for (const auto& [slot, address] : *slots) stageCapture.srtFills.push_back({slot, address});
                if (attempt != 0) Speculation::Count(&SpeculationCounts::partial);
                ++filledCaptures;
                filledWords += stageCapture.srtFills.size();
                return speculative;
            }
            std::vector<std::uint64_t> distinct = speculated;
            std::sort(distinct.begin(), distinct.end());
            distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
            if (attempt == 0 && !threw && !structural && failing.size() < distinct.size()) {
                forbidden = std::move(failing);
                continue;
            }
            const auto reason = structural ? *structural : consumed || threw ? RefusalReason::ConsumedByWalk : RefusalReason::NotALeaf;
            static std::atomic<std::uint64_t> reports{0};
            if (reports.fetch_add(1) < 32 && speculative != nullptr) std::fprintf(stderr, "[speccap] refused program 0x%llx stage %zu (reason %u: 0=no pure slots 1=plan unsafe 2=consumed by the walk 3=not a leaf): %zu words speculated, first 0x%llx, %zu leaves, %zu walk reads\n", static_cast<unsigned long long>(program.binary.codeAddress), i, static_cast<unsigned>(reason), speculated.size(), static_cast<unsigned long long>(speculated.front()), speculative->readTrace.leaves.size(), speculative->readTrace.otherReads.size());
            ++repeatedCaptures;
            Speculation::Refuse(program.binary.codeAddress, reason);
            break;
        }
        stageCapture.srtFills.clear();
        speculated.clear();
        speculatedValues.clear();
        return exact();
    };
    auto capture = captureStage(speculate);

    for (;;) {

    stageCapture.regions = shaderMemory.TakeRecentRegions();
    stageCapture.userDataUse = capture->userDataUse;
    recompiled[i] = true;
    memory = shaderMemory.Regions();

    if (drawHit) {
        for (std::size_t j = 0; j < programs.size(); ++j) {
            if (matched[j] != nullptr && !recompiled[j]) memory.insert(memory.end(), matchedRegions[j].begin(), matchedRegions[j].end());
        }
    }
    request.context.memory = memory;
    if (traceCapSync()) traceCapture("draw-capture", program.binary.codeAddress, submission.queue, memory, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
    if (profile) {
        ++captures;
        phaseTiming.Phase(DrawRowCapture);

        const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBefore, phaseMs[DrawRowCapture]);
        phaseMs[DrawRowCapture] -= waited;
        phaseMs[DrawRowCaptureHookWaits] += waited;
    }
    if (dumpTarget != 0) {

        const auto slot0 = (static_cast<std::uint64_t>(readRegister(queue.context, 0x390)) << 40u) | (static_cast<std::uint64_t>(readRegister(queue.context, 0x318)) << 8u);
        if ((graphics.hasColorTarget && graphics.color.address == dumpTarget) || slot0 == dumpTarget) static_cast<void>(dumpRequest(program.binary.codeAddress, request));
    }
    if (dumpSlot1 != 0) {
        const auto value = [&](std::uint32_t offset) -> std::uint64_t { const auto it = queue.context.find(offset); return it == queue.context.end() ? 0u : it->second; };
        const auto slot1 = (value(0x391) << 40u) | (value(0x327) << 8u);
        if (slot1 == dumpSlot1) {
            static_cast<void>(dumpRequest(program.binary.codeAddress, request));
            if (std::FILE* file = std::fopen("draw_slot1.regs", "w")) {
                for (const auto& [offset, value] : queue.context) std::fprintf(file, "context %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.userConfig) std::fprintf(file, "uconfig %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.shader) std::fprintf(file, "shader %x %08x\n", offset, value);
                std::fclose(file);
            }
        }
    }

    phaseTiming.Phase(DrawRowCapture);

    timing.Mark("capture_resources");
    stageCapture.compiled = invocation.Materialize(*capture);
    // The binding the fills target must hold them, or the stage is captured exactly.
    if (stageCapture.srtFills.empty() || FillsFit(*stageCapture.compiled, stageCapture.srtFills)) break;
    ++repeatedCaptures;
    Speculation::Refuse(program.binary.codeAddress, RefusalReason::FillsDontFit);
    capture = captureStage(false);
    }
    shaderMemory.Speculate(wasSpeculating);
    if (!stageCapture.srtFills.empty()) {
        Speculation::Remember(speculated, speculatedValues);
        if (SpeculativeCaptureMode() == SpeculativeMode::Verify) {
            // The exact capture (it waits for the writers) on a copy of the memory, so the draw's
            // own capture state stays as the speculative one left it.
            ShaderMemory exactMemory(shaderMemory);
            exactMemory.Speculate(false);
            std::shared_ptr<const ShaderRecompiler::ResourceCapture> exactCapture;
            {
                const SampledReadScope sampling(evidenceReads);
                exactCapture = exactMemory.Capture(invocation);
            }
            const auto exact = invocation.Materialize(*exactCapture);
            std::uint64_t stale = 0, fresh = 0;
            const char* what = DiffersOutsideFills(*stageCapture.compiled, *exact, stageCapture.srtFills, stale, fresh);
            Speculation::Count(&SpeculationCounts::verified);
            Speculation::Count(&SpeculationCounts::verifyStale, stale);
            Speculation::Count(&SpeculationCounts::verifyFresh, fresh);
            if (what != nullptr) {
                Speculation::Count(&SpeculationCounts::verifyMismatches);
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[speccap] VERIFY MISMATCH: stage %zu (program 0x%llx) differs from its exact capture in its %s (%zu fill words)\n", i, static_cast<unsigned long long>(program.binary.codeAddress), what, stageCapture.srtFills.size());
                if (SpeculativeCaptureAbort()) std::abort();
            }
        }
    }
    timing.Mark("materialize");
    ShaderRecompiler::RecompileResult result = *stageCapture.compiled;
    phaseTiming.Phase(DrawRowRecompile);
    return result;
}

void Driver::cacheDrawStages(bool useDrawEntries, bool drawHit, const Pm4::DrawParameters& drawParameters, const std::optional<Graphics::IndirectDrawPath>& indirectCpu, const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& stageCaptures, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool verifyHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::uint64_t drawKey, bool registerKey, const std::shared_ptr<const DrawDecode>& decode, DrawPhaseTiming& phaseTiming) {
    if (useDrawEntries && !drawHit && !(drawParameters.indirect && indirectCpu) && (verifyHit || admitDrawKey(drawKey))) {
        phaseTiming.Phase(DrawRowVectors);
        std::uint64_t unstable = 0, mismatches = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& stageCapture = stageCaptures[i];
            if (stageCapture.compiled == nullptr || !CacheableResult(*stageCapture.compiled)) continue;
            auto variant = std::make_shared<DispatchVariant>();
            variant->compiled = stageCapture.compiled;
            variant->shader = programs[i].snapshot;
            variant->forgetSerial = stageCapture.forgetSerial;
            variant->pushOffset = stageCapture.pushOffset;
            if (vertexInfos[i]) variant->vertexInfo = std::make_shared<const ShaderRecompiler::ShaderVertexStageInfo>(*vertexInfos[i]);
            variant->srtFills = stageCapture.srtFills;

            std::vector<ShaderRecompiler::MemoryRegion> regions(stageCapture.regions.begin(), stageCapture.regions.end());
            for (const auto& read : decodeReads[i]) regions.push_back({read.address, std::as_bytes(std::span(read.bytes))});
            std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
            tagVariantRuns(*variant, regions, stageCapture.userDataUse, decodeReads[i].empty() && (!vertexInfos[i] || vertexInfos[i]->resourcesNum == 0), programs[i].hiddenUserWords);
            const auto sameContent = [](const DispatchVariant& a, const DispatchVariant& b) {
                const auto flatten = [](const DispatchVariant& source) {
                    std::vector<std::pair<std::uint64_t, std::uint32_t>> flat;
                    std::size_t word = 0;
                    for (const auto& [begin, end] : source.runs) {
                        for (auto address = begin; address < end && word < source.words.size(); address += sizeof(std::uint32_t)) flat.emplace_back(address, source.words[word++]);
                    }
                    std::sort(flat.begin(), flat.end());
                    return flat;
                };
                return (a.runs == b.runs && a.words == b.words) || flatten(a) == flatten(b);
            };
            const auto sameForwardedWords = [](const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b, std::uint32_t hidden) {
                if (a.pushConstants.size() != b.pushConstants.size() || a.shaderDataUserWords != b.shaderDataUserWords) return false;
                for (std::size_t slot = 0; slot < a.shaderDataUserWords.size() && (slot + 1) * sizeof(std::uint32_t) <= a.pushConstants.size(); ++slot) {
                    if (a.shaderDataUserWords[slot] >= hidden && std::memcmp(a.pushConstants.data() + slot * sizeof(std::uint32_t), b.pushConstants.data() + slot * sizeof(std::uint32_t), sizeof(std::uint32_t)) != 0) return false;
                }
                return true;
            };
            if (verifyHit && matched[i] != nullptr && (!sameContent(*matched[i], *variant) || !sameForwardedWords(*matched[i]->compiled, *variant->compiled, programs[i].hiddenUserWords))) {
                ++mismatches;
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit captured differently: %zu runs / %zu words matched, %zu / %zu fresh\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), matched[i]->runs.size(), matched[i]->words.size(), variant->runs.size(), variant->words.size());
            }
            if (insertCompare()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
                if (!captureStable(stageCapture.regions)) {
                    ++unstable;
                    continue;
                }
            }
            fresh[i] = std::move(variant);
        }
        insertDrawEntry(drawKey, fresh, registerKey ? decode : nullptr);
        if (unstable != 0 || mismatches != 0) {
            std::lock_guard cacheLock(drawCacheMutex);
            drawEntryCounters.unstable += unstable;
            drawEntryCounters.verifyMismatches += mismatches;
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

}
