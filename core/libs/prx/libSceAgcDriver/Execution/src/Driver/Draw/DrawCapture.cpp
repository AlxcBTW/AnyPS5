#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

namespace {

// exp (APS5_EXP_RING_POINTERS): set while verifyRingHit rebuilds a stage, so that rebuild records
// nothing.
thread_local bool ringVerifying = false;

// The user words of `program` whose registers the draw's key left out.
std::vector<std::uint32_t> RingLeftOut(const DrawProgram& program) {
    std::vector<std::uint32_t> words;
    const auto& ring = Ring::ThisDraw();
    if (!ring.keyed || ring.mask == nullptr) return words;
    for (const auto reg : ring.mask->registers) {
        if (const auto word = Ring::RegisterUserWord(program.binary.stage, program.firstUserSgpr, program.userDataBase, program.userData.size(), reg)) words.push_back(*word);
    }
    std::sort(words.begin(), words.end());
    return words;
}

}

bool SameRingLayout(const std::shared_ptr<const Ring::Layout>& a, const std::shared_ptr<const Ring::Layout>& b) {
    if (a == nullptr || b == nullptr) return a == b;
    if (a->words.size() != b->words.size()) return false;
    for (std::size_t w = 0; w < a->words.size(); ++w) {
        if (a->words[w].userWord != b->words[w].userWord || a->words[w].value != b->words[w].value || a->words[w].relative != b->words[w].relative) return false;
    }
    return a->runWord == b->runWord;
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
    const auto capture = [&] {
        const SampledReadScope sampling(evidenceReads);
        return shaderMemory.Capture(invocation);
    }();

    stageCapture.regions = shaderMemory.TakeRecentRegions();
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
    stageCapture.capture = nullptr;
    stageCapture.ring.reset();
    if (Ring::Draws() && !ringVerifying && !drawParameters.indirect && Ring::ThisDraw().keyed) {
        // exp (APS5_EXP_RING_POINTERS): the trace for the data positions, and the stage recorded
        // relative to its ring words while its program set still learns its mask or leaves one of
        // the stage's words out.
        stageCapture.capture = capture;
        const auto& mask = Ring::ThisDraw().mask;
        auto candidates = CacheableResult(*stageCapture.compiled) ? Ring::Candidates(capture->plan) : std::vector<std::uint32_t>{};
        const auto excluded = [&](std::uint32_t word) {
            if (const auto& vertex = vertexInfos[i]; vertex && (vertex->resourcesNum != 0 || vertex->fetchEmbedded)) {
                if (word == vertex->fetchAttribReg || word == vertex->fetchAttribReg + 1u || word == vertex->fetchBufferReg || word == vertex->fetchBufferReg + 1u) return true;
            }
            for (const auto sgpr : {stageCapture.compiled->vertexOffsetSgpr, stageCapture.compiled->instanceOffsetSgpr}) {
                if (sgpr >= 0 && static_cast<std::uint32_t>(sgpr) >= program.firstUserSgpr && word == static_cast<std::uint32_t>(sgpr) - program.firstUserSgpr) return true;
            }
            return !Ring::UserWordRegister(program.binary.stage, program.firstUserSgpr, program.userDataBase, word).has_value();
        };
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), excluded), candidates.end());
        if (mask != nullptr && mask->frozen) {
            const auto leftOut = RingLeftOut(program);
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](std::uint32_t word) { return !std::binary_search(leftOut.begin(), leftOut.end(), word); }), candidates.end());
        }
        if (!candidates.empty()) {
            std::vector<ShaderRecompiler::MemoryRegion> initial;
            for (const auto& other : programs) {
                for (const auto& region : other.memory) {
                    if (!region.bytes.empty()) initial.push_back(region);
                }
            }
            auto failure = Ring::RecordFailure::None;
            stageCapture.ring = Ring::RecordStage(candidates, program.userData, program.binary.codeAddress, stageCapture.regions, initial, *stageCapture.compiled, [&](const ShaderRecompiler::SrtRuntime& runtime) { return invocation.Capture(runtime); }, [&](const ShaderRecompiler::ResourceCapture& moved, std::span<const std::uint32_t>) { return invocation.Materialize(moved); }, &failure);
            auto& totals = Ring::Totals();
            if (stageCapture.ring) {
                totals.recorded.fetch_add(1, std::memory_order_relaxed);
                totals.recordWords.fetch_add(stageCapture.ring->words.size(), std::memory_order_relaxed);
            } else {
                totals.failures[static_cast<std::size_t>(failure)].fetch_add(1, std::memory_order_relaxed);
            }
        }
        timing.Mark("ring_record");
    }
    ShaderRecompiler::RecompileResult result = *stageCapture.compiled;
    phaseTiming.Phase(DrawRowRecompile);
    return result;
}

void Driver::cacheDrawStages(bool useDrawEntries, bool drawHit, const Pm4::DrawParameters& drawParameters, const std::optional<Graphics::IndirectDrawPath>& indirectCpu, const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& stageCaptures, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool verifyHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::uint64_t drawKey, bool registerKey, const std::shared_ptr<const DrawDecode>& decode, DrawPhaseTiming& phaseTiming) {
    if (useDrawEntries && !drawHit && !(drawParameters.indirect && indirectCpu)) {
        phaseTiming.Phase(DrawRowVectors);
        std::uint64_t unstable = 0, mismatches = 0;
        // exp (APS5_EXP_RING_POINTERS): a key that left registers out needs every one of them read
        // by a stage that gets a variant (else an entry would hit whatever their value).
        const bool ringInsert = Ring::Draws() && Ring::ThisDraw().keyed && !drawParameters.indirect;
        if (ringInsert && Ring::ThisDraw().mask != nullptr) {
            for (const auto reg : Ring::ThisDraw().mask->registers) {
                bool covered = false;
                for (std::size_t i = 0; i < programs.size() && !covered; ++i) {
                    covered = stageCaptures[i].compiled != nullptr && Ring::RegisterUserWord(programs[i].binary.stage, programs[i].firstUserSgpr, programs[i].userDataBase, programs[i].userData.size(), reg).has_value();
                }
                if (!covered) {
                    Ring::Totals().orphans.fetch_add(1, std::memory_order_relaxed);
                    phaseTiming.Phase(DrawRowKeyLookupValidate);
                    return;
                }
            }
        }
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& stageCapture = stageCaptures[i];
            if (stageCapture.compiled == nullptr || !CacheableResult(*stageCapture.compiled)) continue;
            auto variant = std::make_shared<DispatchVariant>();
            variant->compiled = stageCapture.compiled;
            variant->shader = programs[i].snapshot;
            variant->forgetSerial = stageCapture.forgetSerial;
            variant->pushOffset = stageCapture.pushOffset;
            if (vertexInfos[i]) variant->vertexInfo = std::make_shared<const ShaderRecompiler::ShaderVertexStageInfo>(*vertexInfos[i]);

            if (ringInsert) {
                // exp (APS5_EXP_RING_POINTERS): the runs read through a left-out word relative to it,
                // the left-out words without a record exact, and the pure flat slots as data.
                std::vector<ShaderRecompiler::MemoryRegion> extra;
                for (const auto& read : decodeReads[i]) extra.push_back({read.address, std::as_bytes(std::span(read.bytes))});
                const auto leftOut = RingLeftOut(programs[i]);
                Ring::BuildRuns(stageCapture.ring ? &*stageCapture.ring : nullptr, leftOut, programs[i].userData, stageCapture.regions, extra, variant->runs, variant->words, variant->ring);
                if (variant->ring != nullptr) {
                    for (const auto& word : variant->ring->words) {
                        if (!word.relative) Ring::Totals().exactWords.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (dataHits() && stageCapture.capture != nullptr && !stageCapture.capture->readTrace.leaves.empty()) {
                    const auto& bindings = stageCapture.compiled->bindings;
                    for (std::size_t b = 0; b < bindings.size(); ++b) {
                        if (bindings[b].role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
                        variant->flatBinding = static_cast<std::uint32_t>(b);
                        Ring::DataPositions(variant->runs, variant->words, variant->ring.get(), stageCapture.capture->readTrace, bindings[b].guestDescriptor, variant->dataPositions, variant->dataSlots);
                        break;
                    }
                }
            } else {
                std::vector<ShaderRecompiler::MemoryRegion> regions(stageCapture.regions.begin(), stageCapture.regions.end());
                for (const auto& read : decodeReads[i]) regions.push_back({read.address, std::as_bytes(std::span(read.bytes))});
                std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
                for (const auto& region : regions) {
                    variant->runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
                    const auto count = region.bytes.size() / sizeof(std::uint32_t);
                    const auto offset = variant->words.size();
                    variant->words.resize(offset + count);
                    std::memcpy(variant->words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
                }
            }
            if (verifyHit && matched[i] != nullptr && (matched[i]->runs != variant->runs || matched[i]->words != variant->words)) {
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

void Driver::refreshRingWords(const QueueState& queue, std::vector<DrawProgram>& programs) {
    const auto& ring = Ring::ThisDraw();
    if (!ring.keyed || ring.mask == nullptr || ring.mask->registers.empty()) return;
    for (auto& program : programs) {
        for (const auto reg : ring.mask->registers) {
            if (const auto word = Ring::RegisterUserWord(program.binary.stage, program.firstUserSgpr, program.userDataBase, program.userData.size(), reg)) program.userData[*word] = readUserData(queue.shader, reg);
        }
    }
}

void Driver::learnRingMask(const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<StageCapture>& stageCaptures) {
    const auto& ring = Ring::ThisDraw();
    if (!ring.keyed || (ring.mask != nullptr && ring.mask->frozen)) return;
    static_cast<void>(roles);
    // A register goes out of the key when every program that reads it holds it relative.
    const auto holds = [&](std::size_t p, std::uint32_t word) {
        if (p >= stageCaptures.size() || !stageCaptures[p].ring) return false;
        const auto& words = stageCaptures[p].ring->words;
        return std::any_of(words.begin(), words.end(), [&](const Ring::Word& held) { return held.userWord == word && held.relative; });
    };
    std::vector<std::uint32_t> candidates;
    for (std::size_t p = 0; p < programs.size() && p < stageCaptures.size(); ++p) {
        if (!stageCaptures[p].ring) continue;
        for (const auto& word : stageCaptures[p].ring->words) {
            if (const auto reg = Ring::UserWordRegister(programs[p].binary.stage, programs[p].firstUserSgpr, programs[p].userDataBase, word.userWord)) candidates.push_back(*reg);
        }
    }
    std::vector<std::uint32_t> registers;
    for (const auto reg : candidates) {
        bool droppable = true;
        for (std::size_t p = 0; p < programs.size() && droppable; ++p) {
            const auto word = Ring::RegisterUserWord(programs[p].binary.stage, programs[p].firstUserSgpr, programs[p].userDataBase, programs[p].userData.size(), reg);
            if (word && !holds(p, *word)) droppable = false;
        }
        if (droppable) registers.push_back(reg);
    }
    Ring::LearnMask(ring.set, std::move(registers), ring.pins);
}

void Driver::verifyRingHit(std::size_t i, std::uint32_t pushOffset, const QueueState& queue, const Submission& submission, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<ShaderRecompiler::LinkedProgram>& linked, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, const ShaderRecompiler::RecompileResult& patched) {
    const auto fail = [&](const char* what, std::size_t where) {
        std::fprintf(stderr, "[exp-ring] APS5_EXP_RING_VERIFY: %s disagrees (stage %zu, program 0x%llx, at %zu)\n", what, i, static_cast<unsigned long long>(programs[i].binary.codeAddress), where);
        std::fflush(stderr);
        std::abort();
    };
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    for (const auto& program : programs) {
        for (const auto& region : program.memory) {
            if (!region.bytes.empty()) memory.push_back(region);
        }
    }
    ShaderMemory shaderMemory(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
    std::vector<StageCapture> captures(programs.size());
    std::vector<bool> recompiled(programs.size(), false);
    std::uint64_t count = 0;
    std::array<double, DrawDriverPhaseCount> ms{};
    auto lap = std::chrono::steady_clock::now();
    DrawPhaseTiming timing{false, ms, lap};
    std::string rejected;
    ringVerifying = true;
    const auto fresh = materializeDrawStage(i, pushOffset, queue, submission, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, shaderMemory, captures, recompiled, false, matched, matchedRegions, false, 0, 0, count, timing, ms, rejected);
    ringVerifying = false;
    if (!rejected.empty()) fail("the rebuild (rejected)", 0);
    if (fresh.variantId != patched.variantId) fail("the variant", 0);
    if (fresh.pushConstants != patched.pushConstants) fail("the push constants", 0);
    if (fresh.bindings.size() != patched.bindings.size()) fail("the binding count", 0);
    for (std::size_t b = 0; b < fresh.bindings.size(); ++b) {
        const auto& x = fresh.bindings[b];
        const auto& y = patched.bindings[b];
        if (x.kind != y.kind || x.role != y.role || x.binding != y.binding || x.count != y.count) fail("a binding's shape", b);
        if (x.guestDescriptor != y.guestDescriptor) fail("a binding's words", b);
    }
    // Every dword the rebuild read is among the hit's validated regions, with the same bytes.
    std::size_t index = 0;
    for (const auto& region : captures[i].regions) {
        for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= region.bytes.size(); offset += sizeof(std::uint32_t), ++index) {
            const auto address = region.guestAddress + offset;
            bool found = false;
            for (const auto& known : matchedRegions[i]) {
                if (address < known.guestAddress || address + sizeof(std::uint32_t) > known.guestAddress + known.bytes.size()) continue;
                if (std::memcmp(known.bytes.data() + (address - known.guestAddress), region.bytes.data() + offset, sizeof(std::uint32_t)) != 0) fail("a read dword's value", index);
                found = true;
                break;
            }
            if (!found) fail("the read set (a dword the hit did not validate)", index);
        }
    }
    Ring::Totals().verified.fetch_add(1, std::memory_order_relaxed);
}

}
