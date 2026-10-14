#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawTiming.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
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
    // The GPU SRT fill of GPU-side mesh indirect draws (APS5_NO_GPU_DRAW_INDIRECT=1 turns it off;
    // the hook reads then show what it would take over).
    const auto captures = TakeSrtFillCaptureCounts();
    const auto reads = ShaderMemory::TakeSpeculationCounts();
    const auto fills = Graphics::TakeSrtFillCounts();
    std::string cpuPaths;
    std::uint64_t cpuWords = fills.notRecorded;
    for (std::size_t path = 0; path < fills.words.size(); ++path) {
        if (path == static_cast<std::size_t>(Graphics::IndirectDrawPath::Gpu) || fills.words[path] == 0) continue;
        char text[96];
        std::snprintf(text, sizeof(text), "%s%s %llu", cpuPaths.empty() ? "" : ", ", Graphics::IndirectDrawPathName(static_cast<Graphics::IndirectDrawPath>(path)), static_cast<unsigned long long>(fills.words[path]));
        cpuPaths += text;
        cpuWords += fills.words[path];
    }
    AgcDriver::ProfilePrint_nid_no_patch("[srtfill] (10 s) speculative stage captures %llu: %llu left %llu flattened-SRT words to the draw, %llu repeated without speculation (a speculated word was not only a pure SRT leaf), %llu of refused programs; words speculated %llu; hook reads of pending words in indirect draw captures %llu; draws with fills %llu: words copied on the GPU %llu, read on the CPU %llu (not a lean recorded draw %llu%s%s)
", static_cast<unsigned long long>(captures.speculative), static_cast<unsigned long long>(captures.filled), static_cast<unsigned long long>(captures.words), static_cast<unsigned long long>(captures.repeated), static_cast<unsigned long long>(captures.refused), static_cast<unsigned long long>(reads.speculated), static_cast<unsigned long long>(reads.syncReads), static_cast<unsigned long long>(fills.draws), static_cast<unsigned long long>(fills.words[static_cast<std::size_t>(Graphics::IndirectDrawPath::Gpu)]), static_cast<unsigned long long>(cpuWords), static_cast<unsigned long long>(fills.notRecorded), cpuPaths.empty() ? "" : ", ", cpuPaths.c_str());
}

void DrawPhaseTiming::Phase(DrawDriverPhase which) {
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    phaseMs[which] += std::chrono::duration<double, std::milli>(now - phaseLap).count();
    phaseLap = now;
}

}
