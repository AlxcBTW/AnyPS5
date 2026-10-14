#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace AgcDriver::DriverDetail {

namespace {

// Programs whose speculative capture failed once (a speculated word was consumed by the walk, or
// read where no pure flat-SRT leaf reads it): they capture without speculation from then on. A
// stale entry (the address reused by other code) only costs that code its speculation.
std::mutex refusedMutex;
std::unordered_set<std::uint64_t>& refusedPrograms() {
    static std::unordered_set<std::uint64_t> programs;
    return programs;
}

bool speculationRefused(std::uint64_t program) {
    std::lock_guard lock(refusedMutex);
    return refusedPrograms().contains(program);
}

void refuseSpeculation(std::uint64_t program) {
    std::lock_guard lock(refusedMutex);
    refusedPrograms().insert(program);
}

std::atomic<std::uint64_t> speculativeCaptures{0}, filledCaptures{0}, filledWords{0}, repeatedCaptures{0}, refusedCaptures{0};

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
    const auto captureOnce = [&] {
        const SampledReadScope sampling(evidenceReads);
        return shaderMemory.Capture(invocation);
    };
    // A speculating draw (Driver::draw: a GPU-side mesh indirect draw, APS5_NO_GPU_DRAW_INDIRECT=1
    // off) reads a word that recorded GPU work still writes without waiting for it. The capture
    // stands when every such word is only the leaf of pure flat-SRT slots (the walk consumed none
    // of them: SpeculatedLeafSlots); the draw then fills those FlattenedSrt words from guest memory
    // on the GPU (Graphics::Draw), after the writer, and the stale bytes in the snapshot never
    // reach a shader. Otherwise the capture is repeated without speculation, which waits as before,
    // and the program never speculates again.
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
    stageCapture.srtFills.clear();
    const bool speculating = shaderMemory.Speculating();
    if (speculating && speculationRefused(program.binary.codeAddress)) {
        ++refusedCaptures;
        shaderMemory.Speculate(false);
    }
    if (shaderMemory.Speculating()) {
        ++speculativeCaptures;
        try {
            capture = captureOnce();
        } catch (...) {
            if (shaderMemory.TakeSpeculated().empty()) throw;
            capture = nullptr;
        }
        if (capture != nullptr) {
            const auto speculated = shaderMemory.TakeSpeculated();
            if (!speculated.empty()) {
                const auto slots = SpeculatedLeafSlots(speculated, capture->readTrace.leaves, capture->readTrace.otherReads);
                if (slots.has_value()) {
                    for (const auto& [slot, address] : *slots) stageCapture.srtFills.push_back({slot, address});
                    ++filledCaptures;
                    filledWords += slots->size();
                } else {
                    capture = nullptr;
                }
            }
        }
        if (capture == nullptr) {
            ++repeatedCaptures;
            refuseSpeculation(program.binary.codeAddress);
            shaderMemory.Speculate(false);
            capture = captureOnce();
        }
    } else {
        capture = captureOnce();
    }
    shaderMemory.Speculate(speculating);

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
