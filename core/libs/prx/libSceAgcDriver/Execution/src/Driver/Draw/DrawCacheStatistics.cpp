#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

bool Driver::traceDrawCache() {
    static const bool trace = std::getenv("APS5_TRACE_DRAW_CACHE") != nullptr;
    return trace;
}

void Driver::traceInsert(std::unordered_set<std::uint64_t>& set, std::uint64_t key) {
    if (set.size() >= (1u << 16u)) set.clear();
    set.insert(key);
}

void Driver::maybeReportDrawCache(bool profile) {
    auto& counters = drawEntryCounters;
    if (!profile && !traceDrawCache()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - counters.lastReport < std::chrono::seconds(10)) return;
    reportDrawCache(counters, profile);
    counters.lastReport = now;
}

std::string Driver::relocationClassLines(const DrawEntryCounters& counters) {
    std::uint64_t stages = 0;
    double totalUs = 0;
    for (std::size_t c = 0; c < counters.relocStages.size(); ++c) {
        stages += counters.relocStages[c];
        totalUs += counters.relocCompileUs[c];
    }
    std::string text;
    char line[512];
    std::snprintf(line, sizeof(line), "[draw-relocate-classes] 10 s: %llu stages compiled for changed user words, %.1f ms compile in total", static_cast<unsigned long long>(stages), totalUs / 1000.0);
    text += line;
    for (std::size_t c = 0; c < counters.relocStages.size(); ++c) {
        const auto n = counters.relocStages[c];
        std::snprintf(line, sizeof(line), "; %s: %llu stages (%.0f%%), %.1f ms saved if served (%.0f us each)", RelocClassName(static_cast<RelocClass>(c)), static_cast<unsigned long long>(n), stages != 0 ? 100.0 * static_cast<double>(n) / static_cast<double>(stages) : 0.0, counters.relocCompileUs[c] / 1000.0, n != 0 ? counters.relocCompileUs[c] / static_cast<double>(n) : 0.0);
        text += line;
    }
    std::snprintf(line, sizeof(line), "; b patches %llu data words; reasons:", static_cast<unsigned long long>(counters.relocDataWords));
    text += line;
    for (std::size_t r = 1; r < counters.relocReasons.size(); ++r) {
        std::snprintf(line, sizeof(line), " %s %llu%s", RelocReasonName(static_cast<RelocReason>(r)), static_cast<unsigned long long>(counters.relocReasons[r]), r + 1 < counters.relocReasons.size() ? "," : "");
        text += line;
    }
    text += "\n";
    return text;
}

void Driver::reportDrawCache(DrawEntryCounters& counters, bool profile) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string text;
    if (traceDrawCache()) {
        char line[1536];
        std::uint64_t misses = 0;
        for (const auto value : counters.misses) misses += value;
        std::snprintf(line, sizeof(line), "[draw-cache] 10 s: %llu lookups, %llu hits (%.1f%%), %llu stage misses, %llu without an entry, %llu data-only hits (stages: %llu reused, %llu of them with refreshed data words, %llu compiled, %llu of them with changed user words; %llu verified); no entry: new registers %llu, user words/pointers only %llu, evicted %llu, seen but never inserted %llu; stage misses: front differing %llu, fragment differing %llu, other differing %llu, layout %llu, gate %llu, stage count %llu; differing captures: same runs %llu (%.1f words changed each), other runs %llu; %zu entries, %llu evicted\n", count(counters.lookups), count(counters.hits), counters.lookups != 0 ? 100.0 * static_cast<double>(counters.hits) / static_cast<double>(counters.lookups) : 0.0, count(misses), count(counters.absent), count(counters.dataHits), count(counters.dataStagesReused), count(counters.dataStagesPatched), count(counters.dataStagesCompiled), count(counters.dataCompiledUserWords), count(counters.dataVerified), count(counters.absentNewRegisters), count(counters.absentUserWords), count(counters.absentEvicted), count(counters.absentNeverInserted), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.differingSameRuns), counters.differingSameRuns != 0 ? static_cast<double>(counters.differingWords) / static_cast<double>(counters.differingSameRuns) : 0.0, count(counters.differingRunsChanged), drawCache.size(), count(counters.evictions));
        text += line;
        text += relocationClassLines(counters);
    }
    if (profile) text += profileDrawCacheLines(counters);
    AgcDriver::ProfilePrint_nid_no_patch("%s", text.c_str());
    counters = DrawEntryCounters{};
}

DrawEntryCounters Driver::DrawCacheCounters() {
    std::lock_guard cacheLock(drawCacheMutex);
    return drawEntryCounters;
}

std::string Driver::profileDrawCacheLines(const DrawEntryCounters& counters) {
    std::string text;
    const auto append = [&](const char* format, auto... values) {
        char line[2048];
        std::snprintf(line, sizeof(line), format, values...);
        text += line;
    };
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto validated = counters.lookups > counters.absent ? counters.lookups - counters.absent : 0;
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string ranks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char item[32];
        std::snprintf(item, sizeof(item), "%s%llu", rank == 0 ? "" : " / ", count(counters.variantHitsByRank[rank]));
        ranks += item;
    }
    append("[draw-cache] %llu lookups (10 s): %llu no entry, %llu validated in %.1f us each (key, lookup and every stage): %llu hits (every stage equal; stage hits by variant rank 1..k %s), misses by reason: front stage differing %llu, fragment differing %llu, other stage differing %llu, layout (push offset) %llu, gate %llu, stage count %llu; %llu stage validations (%llu equal, %.2f variants compared each); %llu inserts (%llu variants inserted, %llu evicted beyond k, %llu already present, %llu unstable), %zu entries (%llu variants, ~%.1f MiB), %llu evictions in total, %llu LRU moves; verify: %llu hits captured again, %llu stages differed\n", count(counters.lookups), count(counters.absent), count(validated), validated != 0 ? counters.validateUs / static_cast<double>(validated) : 0.0, count(counters.hits), ranks.c_str(), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.stageValidations), count(counters.stageEqual), counters.stageValidations != 0 ? static_cast<double>(counters.variantsCompared) / static_cast<double>(counters.stageValidations) : 0.0, count(counters.inserts), count(counters.variantsInserted), count(counters.variantsEvicted), count(counters.present), count(counters.unstable), drawCache.size(), count(drawCacheVariants), static_cast<double>(drawCacheVariantBytes) / (1024.0 * 1024.0), count(drawCacheEvictions), count(counters.touches), count(counters.verifyHits), count(counters.verifyMismatches));
    append("[draw-cache] register key (10 s): %llu lookups, %llu hits, key build %.1f us each; decode skipped %llu, partial (state/pixel/programs from the entry) %llu; facade log != table %llu; verify: %llu decodes compared, %llu differed\n", count(counters.registerKeyLookups), count(counters.registerKeyHits), counters.registerKeyLookups != 0 ? counters.keyUs / static_cast<double>(counters.registerKeyLookups) : 0.0, count(counters.decodeSkipped), count(counters.decodePartial), count(counters.facadeMismatches), count(counters.verifyDecodes), count(counters.verifyDecodeMismatches));
    append("[draw-cache] stage data hits (10 s): %llu stages (%llu words refreshed), verified %llu; stage inserts with data positions %llu (%.1f positions each)\n", count(counters.stageDataHits), count(counters.stageDataWords), count(counters.stageDataVerified), count(counters.stageDataInserts), counters.stageDataInserts != 0 ? static_cast<double>(counters.stageDataPositions) / static_cast<double>(counters.stageDataInserts) : 0.0);
    return text;
}

}
