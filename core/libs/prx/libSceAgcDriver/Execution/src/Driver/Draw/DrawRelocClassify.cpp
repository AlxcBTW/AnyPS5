// Lab diagnostic (APS5_TRACE_DRAW_CACHE): classifies stages that were compiled because the draw's user words changed,
// to tell which reuse design would pay off. See RelocClass in DrawCache.hpp.
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include <algorithm>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

namespace {

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t {3};
}

RelocVerdict Verdict(RelocClass cls, RelocReason reason, std::uint64_t words = 0) {
    return {cls, reason, words};
}

}

const char* RelocClassName(RelocClass cls) {
    switch (cls) {
        case RelocClass::Equal: return "a pointers moved, bytes equal (#4943)";
        case RelocClass::DataOnly: return "b pointers moved, only data words differ";
        case RelocClass::Structural: return "c genuine rebuild";
        default: return "d unclassifiable";
    }
}

const char* RelocReasonName(RelocReason reason) {
    switch (reason) {
        case RelocReason::None: return "ok";
        case RelocReason::NoKeptStage: return "d:no kept stage";
        case RelocReason::ShapeDiffers: return "d:shape differs";
        case RelocReason::NoChange: return "d:no user word changed";
        case RelocReason::HiddenWord: return "c:merged-pointer word moved";
        case RelocReason::NonPointerWord: return "c:non-pointer word changed";
        case RelocReason::PointerAlignment: return "c:pointer null/unaligned";
        case RelocReason::RunStructure: return "c:run layout changed";
        case RelocReason::PointerDelta: return "c:run shift != pointer delta";
        case RelocReason::StructuralWords: return "c:structural words differ";
        default: return "?";
    }
}

// Compares a freshly captured stage with one kept stage of the same draw shape; `now` / `before` are the user words of the fresh draw
// and of the draw that produced the kept entry. A pointer word counts as moved when the kept stage and the fresh one both tag it relocatable (#4943).
RelocVerdict ClassifyStageMove(const DispatchVariant& fresh, const DispatchVariant& kept, const std::vector<std::uint32_t>& now, const std::vector<std::uint32_t>& before, std::uint32_t hidden) {
    if (fresh.compiled == nullptr || kept.compiled == nullptr || kept.pushOffset != fresh.pushOffset) return Verdict(RelocClass::Unclassified, RelocReason::NoKeptStage);
    if (now.size() != before.size() || fresh.runs.size() != kept.runs.size() || fresh.words.size() != kept.words.size() || fresh.compiled->pushConstants.size() != kept.compiled->pushConstants.size()) return Verdict(RelocClass::Unclassified, RelocReason::ShapeDiffers);
    for (std::size_t r = 0; r < fresh.runs.size(); ++r) {
        if (fresh.runs[r].second - fresh.runs[r].first != kept.runs[r].second - kept.runs[r].first) return Verdict(RelocClass::Unclassified, RelocReason::ShapeDiffers);
    }
    const bool tagged = fresh.runBases.size() == fresh.runs.size() && kept.runBases.size() == kept.runs.size();
    const std::uint64_t relocatable = tagged ? fresh.relocatableWords & kept.relocatableWords : 0;
    std::uint64_t changed = 0;
    bool any = false;
    for (std::uint32_t k = 0; k < now.size(); ++k) {
        if (now[k] == before[k]) continue;
        any = true;
        if (k < hidden) return Verdict(RelocClass::Structural, RelocReason::HiddenWord);
        if (k >= 64 || (relocatable >> k & 1u) == 0) return Verdict(RelocClass::Structural, RelocReason::NonPointerWord);
        if (now[k] == 0 || before[k] == 0 || ((now[k] ^ before[k]) & 3u) != 0) return Verdict(RelocClass::Structural, RelocReason::PointerAlignment);
        changed |= std::uint64_t {1} << k;
    }
    if (!any) return Verdict(RelocClass::Unclassified, RelocReason::NoChange);
    std::uint64_t seen = 0;
    for (std::size_t r = 0; r < fresh.runs.size(); ++r) {
        const std::int8_t base = tagged ? fresh.runBases[r] : std::int8_t {-1};
        const std::int8_t keptBase = tagged ? kept.runBases[r] : std::int8_t {-1};
        if (base != keptBase) return Verdict(RelocClass::Structural, RelocReason::RunStructure);
        const auto shift = static_cast<std::int64_t>(fresh.runs[r].first) - static_cast<std::int64_t>(kept.runs[r].first);
        if (base < 0 || (changed >> base & 1u) == 0) {
            if (shift != 0) return Verdict(RelocClass::Structural, RelocReason::RunStructure);
            continue;
        }
        seen |= std::uint64_t {1} << base;
        const auto delta = static_cast<std::int64_t>(AlignedWord(now[base])) - static_cast<std::int64_t>(AlignedWord(before[base]));
        if (shift != delta) return Verdict(RelocClass::Structural, RelocReason::PointerDelta);
    }
    if (seen != changed) return Verdict(RelocClass::Unclassified, RelocReason::ShapeDiffers);
    const auto& positions = !kept.dataPositions.empty() ? kept.dataPositions : fresh.dataPositions;
    std::vector<bool> data(fresh.words.size(), false);
    for (const auto position : positions) {
        if (position < data.size()) data[position] = true;
    }
    std::uint64_t differing = 0, structural = 0;
    for (std::size_t w = 0; w < fresh.words.size(); ++w) {
        if (fresh.words[w] == kept.words[w]) continue;
        ++differing;
        if (!data[w]) ++structural;
    }
    if (differing == 0) return Verdict(RelocClass::Equal, RelocReason::None);
    if (structural == 0) return Verdict(RelocClass::DataOnly, RelocReason::None, differing);
    return Verdict(RelocClass::Structural, RelocReason::StructuralWords, structural);
}

RelocVerdict Driver::classifyCompiledStage(const std::shared_ptr<DrawEntry>& donor, std::size_t i, const DrawProgram& now, const StageCapture& stageCapture, const std::vector<Graphics::DecodeRead>& decodeReads, bool eligible) {
    if (donor == nullptr || donor->decode == nullptr || i >= donor->decode->programs.size() || stageCapture.compiled == nullptr) return Verdict(RelocClass::Unclassified, RelocReason::NoKeptStage);
    std::vector<std::shared_ptr<DispatchVariant>> candidates;
    {
        std::lock_guard cacheLock(drawCacheMutex);
        if (i < donor->stages.size()) candidates = donor->stages[i];
    }
    if (candidates.empty()) return Verdict(RelocClass::Unclassified, RelocReason::NoKeptStage);
    DispatchVariant fresh;
    fresh.compiled = stageCapture.compiled;
    fresh.pushOffset = stageCapture.pushOffset;
    std::vector<ShaderRecompiler::MemoryRegion> regions(stageCapture.regions.begin(), stageCapture.regions.end());
    for (const auto& read : decodeReads) regions.push_back({read.address, std::as_bytes(std::span(read.bytes))});
    std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
    tagVariantRuns(fresh, regions, stageCapture.userDataUse, eligible && decodeReads.empty(), now.hiddenUserWords);
    if (drawStageDataHits() && stageCapture.capture != nullptr && !stageCapture.capture->readTrace.leaves.empty()) recordStageDataPositions(fresh, *stageCapture.capture, decodeReads);
    const auto& before = donor->decode->programs[i].userData;
    RelocVerdict best;
    for (const auto& kept : candidates) {
        if (kept == nullptr) continue;
        const auto verdict = ClassifyStageMove(fresh, *kept, now.userData, before, now.hiddenUserWords);
        const bool better = verdict.cls != RelocClass::Unclassified && (best.cls == RelocClass::Unclassified || verdict.cls < best.cls);
        if (better || (best.cls == RelocClass::Unclassified && best.reason == RelocReason::NoKeptStage)) best = verdict;
    }
    return best;
}

}
