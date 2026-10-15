#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawTiming.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/SpeculativeCapture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include <cstdio>
#include <string>

namespace AgcDriver::DriverDetail {

PendingDrawPhases& Driver::pendingDrawPhases() {
    static thread_local PendingDrawPhases pending;
    return pending;
}

void Driver::addDrawPhases(const std::array<double, DrawDriverPhaseCount>& ms, bool drawn, std::uint64_t captures) {
    std::lock_guard lock(drawPhasesMutex);
    auto& totals = drawPhaseTotals;
    for (std::size_t i = 0; i < DrawDriverPhaseCount; ++i) totals.ms[i] += ms[i];
    ++totals.packets;
    if (drawn) ++totals.drawn;
    totals.captures += captures;
    const auto now = std::chrono::steady_clock::now();
    if (now - totals.lastReport < std::chrono::seconds(10)) return;
    totals.lastReport = now;
    std::string report;
    double total = 0;
    for (std::size_t i = 0; i < DrawDriverPhaseCount; ++i) {
        char text[64];
        std::snprintf(text, sizeof(text), " %s %.1f", DrawDriverPhaseNames[i], totals.ms[i] * 1000 / static_cast<double>(totals.packets));
        report += text;
        total += totals.ms[i];
    }
    AgcDriver::ProfilePrint_nid_no_patch( "[draw] driver phases (10 s, %llu draw packets, %llu drawn, %llu captures), us per packet:%s, total %.1f (%.1f ms; Graphics::Draw %.1f ms, skipped %.1f ms)\n", static_cast<unsigned long long>(totals.packets), static_cast<unsigned long long>(totals.drawn), static_cast<unsigned long long>(totals.captures), report.c_str(), total * 1000 / static_cast<double>(totals.packets), total, totals.ms[DrawRowGraphics], totals.ms[DrawRowSkipped]);
    totals.ms = {};
    totals.packets = totals.drawn = totals.captures = 0;
}

// The [speccap] line (APS5_SPECULATIVE_CAPTURE), every 10 s from the first speculating capture on: what the capture
// stopped waiting for, what the draws did instead, and in verify mode what the exact captures found.
void Driver::reportSpeculation() {
    static std::atomic<std::int64_t> due{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto expected = due.load(std::memory_order_relaxed);
    if (now < expected || !due.compare_exchange_strong(expected, now + 10000)) return;
    if (expected == 0) return;
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto captures = Speculation::Take();
    const auto fills = Graphics::TakeSrtFillCounts();
    std::string cpuPaths;
    std::uint64_t cpuWords = fills.notRecorded;
    for (std::size_t path = 0; path < fills.words.size(); ++path) {
        if (path == static_cast<std::size_t>(Graphics::IndirectDrawPath::Gpu) || fills.words[path] == 0) continue;
        char text[96];
        std::snprintf(text, sizeof(text), "%s%s %llu", cpuPaths.empty() ? "" : ", ", Graphics::IndirectDrawPathName(static_cast<Graphics::IndirectDrawPath>(path)), count(fills.words[path]));
        cpuPaths += text;
        cpuWords += fills.words[path];
    }
    const auto& reads = captures.volatileReads;
    const auto hook = [&](ShaderMemory::ReadKind kind) { return std::make_pair(count(captures.hookReads[static_cast<std::size_t>(kind)]), count(captures.hookWaited[static_cast<std::size_t>(kind)])); };
    const auto pageWhole = hook(ShaderMemory::ReadKind::PageWhole), wordNone = hook(ShaderMemory::ReadKind::WordNone), wordSync = hook(ShaderMemory::ReadKind::WordSync), notSpeculating = hook(ShaderMemory::ReadKind::VolatileNotSpeculating), forbiddenRead = hook(ShaderMemory::ReadKind::VolatileForbidden);
    const auto refusal = [&](RefusalReason reason) { return count(captures.refusals[static_cast<std::size_t>(reason)]); };
    AgcDriver::ProfilePrint_nid_no_patch("[speccap] why draw-capture reads went through the flush hook (10 s; reads, of which waited for GPU work): whole page %llu/%llu, word on a page with a pending writer but none over the word %llu/%llu, word the query answered sync %llu/%llu, volatile word of a capture that does not speculate %llu/%llu, volatile word read exactly (consumed by the walk) %llu/%llu; queries answered (per word, speculation on): no writer %llu, foreign writer %llu, larger range %llu, evidence short %llu, writer changed %llu, pending image %llu, unit shadow %llu, label %llu (sync), sampled %llu (sync); refusals: no pure slots %llu, plan unsafe %llu, consumed by the walk %llu, not a leaf %llu, fills do not fit %llu; %llu captures kept their pure leaves speculated and re-read only the other words\n", pageWhole.first, pageWhole.second, wordNone.first, wordNone.second, wordSync.first, wordSync.second, notSpeculating.first, notSpeculating.second, forbiddenRead.first, forbiddenRead.second, count(reads[0]), count(reads[1]), count(reads[2]), count(reads[3]), count(reads[4]), count(reads[5]), count(reads[6]), count(reads[7]), count(reads[8]), refusal(RefusalReason::NoPureSlots), refusal(RefusalReason::PlanUnsafe), refusal(RefusalReason::ConsumedByWalk), refusal(RefusalReason::NotALeaf), refusal(RefusalReason::FillsDontFit), count(captures.partial));
    AgcDriver::ProfilePrint_nid_no_patch("[speccap] (10 s, %s) speculative stage captures %llu: %llu left %llu flattened-SRT words to the draw, %llu repeated exactly (a speculated word was not only a pure SRT leaf), %llu of refused programs; capture word reads that skipped the flush hook %llu (queries answered volatile: no writer %llu, foreign writer %llu, larger range %llu, evidence short %llu, writer changed %llu); evidence from finished writers: %llu unchanged, %llu changed; draws with fills %llu: words copied on the GPU %llu, read on the CPU %llu (%s%snot a lean recorded draw %llu), %llu words of CPU-side indirect draws read exactly; verify: %llu stages checked, %llu mismatches, filled words that differed from the exact read %llu, that did not %llu\n", SpeculativeCaptureMode() == SpeculativeMode::Verify ? "verify" : "on", count(captures.captures), count(captures.accepted), count(captures.fillWords), count(captures.repeated), count(captures.refusedStages), count(captures.speculatedWords), count(reads[static_cast<std::size_t>(VolatileReason::NoWriter)]), count(reads[static_cast<std::size_t>(VolatileReason::Foreign)]), count(reads[static_cast<std::size_t>(VolatileReason::LargeRange)]), count(reads[static_cast<std::size_t>(VolatileReason::EvidenceShort)]), count(reads[static_cast<std::size_t>(VolatileReason::WriterChanged)]), count(captures.observedUnchanged), count(captures.observedChanged), count(fills.draws), count(fills.words[static_cast<std::size_t>(Graphics::IndirectDrawPath::Gpu)]), count(cpuWords), cpuPaths.c_str(), cpuPaths.empty() ? "" : "; ", count(fills.notRecorded), count(captures.indirectCpu), count(captures.verified), count(captures.verifyMismatches), count(captures.verifyStale), count(captures.verifyFresh));
}

void DrawPhaseTiming::Phase(DrawDriverPhase which) {
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    phaseMs[which] += std::chrono::duration<double, std::milli>(now - phaseLap).count();
    phaseLap = now;
}

}
