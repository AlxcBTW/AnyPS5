#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "Optimization/ResourceProgram.hpp"

namespace AgcDriver::DriverDetail {

void Driver::classifyDiffering(std::uint64_t program, std::uint64_t key, const DispatchVariant& old, const DispatchVariant& fresh, const ShaderRecompiler::ResourceCapture* capture, EntryCounters& counters) {
    ++counters.differingClassified;
    ++counters.differingByProgram[program];
    auto& ring = priorValueSets[key];
    if (std::any_of(ring.begin(), ring.end(), [&](const ValueSet& set) { return set.first == fresh.runs && set.second == fresh.words; })) ++counters.differingMatchedPrior;
    ring.emplace_front(old.runs, old.words);
    while (ring.size() > 3) ring.pop_back();
    if (priorValueSets.size() > dispatchCacheEntries()) priorValueSets.clear();
    if (old.runs != fresh.runs || old.words.size() != fresh.words.size()) {
        ++counters.differingRunsChanged;
        ++counters.differingWalk;
        return;
    }
    const auto baseWord = [&](const std::vector<ShaderRecompiler::DescriptorValue>& values, std::uint32_t value) {
        return std::any_of(values.begin(), values.end(), [&](const ShaderRecompiler::DescriptorValue& descriptor) { return descriptor.dwordCount >= 2 && (descriptor.dwords[0] == value || descriptor.dwords[1] == value); });
    };
    std::size_t words = 0, addressWords = 0, dataWords = 0;
    for (std::size_t i = 0; i < fresh.words.size(); ++i) {
        if (old.words[i] == fresh.words[i]) continue;
        ++words;
        counters.differingPositions.insert(i);
        counters.differingFirstPosition = std::min(counters.differingFirstPosition, i);
        counters.differingLastPosition = std::max(counters.differingLastPosition, i);
        const auto value = fresh.words[i];
        if (capture != nullptr && (baseWord(capture->snapshot.buffers, value) || baseWord(capture->snapshot.images, value))) ++addressWords;
        else if (capture != nullptr && std::find(capture->snapshot.flattenedSrt.begin(), capture->snapshot.flattenedSrt.end(), value) != capture->snapshot.flattenedSrt.end()) ++dataWords;
    }
    counters.differingWords += words;
    ++counters.differingWordBuckets[words <= 1 ? 0 : words <= 4 ? 1 : words <= 16 ? 2 : 3];
    if (words == 0) ++counters.differingWalk;
    else if (addressWords == words) ++counters.differingAddress;
    else if (dataWords == words) ++counters.differingData;
    else if (addressWords + dataWords == 0) ++counters.differingWalk;
    else ++counters.differingMixed;
}

void Driver::reportDispatchCache(EntryCounters& counters) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto validated = counters.lookups - counters.absent;
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] %llu lookups (10 s): %llu no entry, %llu validated by value in %.1f us each: %llu equal, %llu differing, %llu page not mapped, %llu queued label, %llu image being stored, %llu publish generation moved, %llu pending serial moved, %llu forget serial moved (%llu pending images stored first, %llu pending runs synced first, %llu equal entries inserted before a forget; gate retried %llu: %llu equal, %llu moved again); runs per entry %.1f validated / %.1f inserted; %llu misses found the entry replaced meanwhile; %llu inserts (%llu captures not kept: unstable), %llu evictions in total, %zu entries, %llu LRU moves\n", count(counters.lookups), count(counters.absent), count(validated), validated != 0 ? counters.validateUs / static_cast<double>(validated) : 0.0, count(counters.equal), count(counters.differing), count(counters.inaccessible), count(counters.queuedLabel), count(counters.flushingImage), count(counters.publishMoved), count(counters.pendingMoved), count(counters.forgetMoved), count(counters.imagesFlushed), count(counters.runsSynced), count(counters.forgetSinceInsert), count(counters.retriesEqual + counters.retriesMoved), count(counters.retriesEqual), count(counters.retriesMoved), validated != 0 ? static_cast<double>(counters.runsValidated) / static_cast<double>(validated) : 0.0, counters.inserts != 0 ? static_cast<double>(counters.runsInserted) / static_cast<double>(counters.inserts) : 0.0, count(counters.replaced), count(counters.inserts), count(counters.unstable), count(dispatchCacheEvictions), dispatchCache.size(), count(counters.touches));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> programs(counters.differingByProgram.begin(), counters.differingByProgram.end());
    std::sort(programs.begin(), programs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string top;
    for (std::size_t i = 0; i < programs.size() && i < 8; ++i) {
        char text[48];
        std::snprintf(text, sizeof(text), " 0x%llx x%llu", static_cast<unsigned long long>(programs[i].first), count(programs[i].second));
        top += text;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] differing by class (10 s, %llu classified): address-only %llu, data-only %llu, walk %llu, mixed %llu (runs changed %llu); matched one of the last 3 value sets %llu; differing words %llu in total (misses with 1 / 2-4 / 5-16 / >16 words: %llu / %llu / %llu / %llu), %zu distinct positions (first %zu, last %zu); top programs by differing:%s\n", count(counters.differingClassified), count(counters.differingAddress), count(counters.differingData), count(counters.differingWalk), count(counters.differingMixed), count(counters.differingRunsChanged), count(counters.differingMatchedPrior), count(counters.differingWords), count(counters.differingWordBuckets[0]), count(counters.differingWordBuckets[1]), count(counters.differingWordBuckets[2]), count(counters.differingWordBuckets[3]), counters.differingPositions.size(), counters.differingPositions.empty() ? std::size_t{0} : counters.differingFirstPosition, counters.differingLastPosition, top.c_str());
    std::string ranks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char text[32];
        std::snprintf(text, sizeof(text), "%s%llu", rank == 0 ? "" : " / ", count(counters.variantHitsByRank[rank]));
        ranks += text;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] variants (10 s, k = %zu): hits by rank 1..k %s; %.2f variants compared per validation; %llu inserted into an entry, %llu evicted beyond k; %.2f variants per entry (%llu over %zu entries, ~%.1f MiB)\n", dispatchVariants(), ranks.c_str(), validated != 0 ? static_cast<double>(counters.variantsCompared) / static_cast<double>(validated) : 0.0, count(counters.variantsInserted), count(counters.variantsEvicted), dispatchCache.empty() ? 0.0 : static_cast<double>(dispatchCacheVariants) / static_cast<double>(dispatchCache.size()), count(dispatchCacheVariants), dispatchCache.size(), static_cast<double>(dispatchCacheVariantBytes) / (1024.0 * 1024.0));
    std::string dataRanks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char text[32];
        std::snprintf(text, sizeof(text), "%s%llu", rank == 0 ? "" : " / ", count(counters.dataHitsByRank[rank]));
        dataRanks += text;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-cache] data hits (10 s): %llu (%llu words refreshed; by rank 1..k %s; %llu data variants missed on pending runs), verified %llu; inserts with data positions %llu of %llu (%.1f positions each), leaves skipped: unmapped %llu, mismatched %llu, aliased %llu\n", count(counters.dataHits), count(counters.dataWordsRefreshed), dataRanks.c_str(), count(dataPendingMisses.exchange(0, std::memory_order_relaxed)), count(counters.dataVerified), count(counters.dataInserts), count(counters.inserts), counters.dataInserts != 0 ? static_cast<double>(counters.dataPositionsInserted) / static_cast<double>(counters.dataInserts) : 0.0, count(counters.dataLeavesUnmapped), count(counters.dataLeavesMismatched), count(counters.dataLeavesAliased));
    std::string words;
    for (std::size_t i = 0; i < counters.missWords.size(); ++i) {
        if (counters.missWords[i] == 0) continue;
        char text[32];
        std::snprintf(text, sizeof(text), " u%zu x%llu", i, count(counters.missWords[i]));
        words += text;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-keymiss] no entry (10 s): %llu (%llu under a content key): first sight %llu, same user words as the identity's previous lookup %llu, only pointer words changed %llu, only V# base bits changed %llu, pointer words and V# base bits %llu, other user words %llu; changed user words:%s\n", count(counters.absent), count(counters.missContentKey), count(counters.missFirstSight), count(counters.missSameUser), count(counters.missPointers), count(counters.missAddresses), count(counters.missPointersAddresses), count(counters.missValues), words.c_str());
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-content] %s (10 s): %llu lookups under a content key (%zu shapes, %llu learned), %llu relocated hits (%llu with moved user words, %llu with moved walked V# bases), refused %llu by user words, %llu by V# fields; inserts: %llu profiles (%.2f pointer words, %.2f relocatable V#s each), %llu with nothing to relocate, exact V#s by reason (bda/null/no source/sources/pointer/walked/derived/flattened/copied) %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu; %llu hits verified\n",contentKeys() ? "on" : "off (APS5_NO_DISPATCH_CONTENT_KEY)", count(counters.contentLookups), dispatchShapes.size(), count(counters.shapesLearned), count(counters.relocatedHits), count(counters.relocatedPointers), count(counters.relocatedAddresses), count(counters.refusedUser), count(counters.refusedFields), count(counters.profilesBuilt), counters.profilesBuilt + counters.profilesEmpty != 0 ? static_cast<double>(counters.profilePointerWords) / static_cast<double>(counters.profilesBuilt + counters.profilesEmpty) : 0.0, counters.profilesBuilt + counters.profilesEmpty != 0 ? static_cast<double>(counters.profileBuffers) / static_cast<double>(counters.profilesBuilt + counters.profilesEmpty) : 0.0, count(counters.profilesEmpty), count(counters.fieldRefusals[0]), count(counters.fieldRefusals[1]), count(counters.fieldRefusals[2]), count(counters.fieldRefusals[3]), count(counters.fieldRefusals[4]), count(counters.fieldRefusals[5]), count(counters.fieldRefusals[6]), count(counters.fieldRefusals[7]), count(counters.fieldRefusals[8]), count(counters.dataVerified));
    counters = EntryCounters{};
}

}
