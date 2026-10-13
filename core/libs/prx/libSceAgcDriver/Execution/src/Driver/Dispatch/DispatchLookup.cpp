#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <cstdlib>
#include <ranges>

namespace AgcDriver::DriverDetail {

namespace {

// A relocated variant's runs at the live pointer words (runBases names the user word a run is
// relative to; -1 or a missing entry keeps the run where it was captured).
void rebaseRuns(const DispatchVariant& variant, std::span<const std::uint32_t> userData, std::vector<std::pair<std::uint64_t, std::uint64_t>>& rebased) {
    const auto& profile = *variant.relocation;
    rebased.assign(variant.runs.begin(), variant.runs.end());
    for (std::size_t run = 0; run < rebased.size() && run < variant.runBases.size(); ++run) {
        const auto base = variant.runBases[run];
        if (base < 0 || static_cast<std::size_t>(base) >= userData.size() || static_cast<std::size_t>(base) >= profile.userData.size()) continue;
        const auto delta = AlignedPointer(userData[base]) - AlignedPointer(profile.userData[base]);
        rebased[run].first += delta;
        rebased[run].second += delta;
    }
}

void appendRegionsAt(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, std::vector<ShaderRecompiler::MemoryRegion>& regions) {
    regions.reserve(regions.size() + runs.size());
    std::size_t offset = 0;
    for (const auto& [begin, end] : runs) {
        const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
        regions.push_back({begin, std::as_bytes(words.subspan(offset, count))});
        offset += count;
    }
}

}

bool Driver::contentKeys() {
    static const bool enabled = std::getenv("APS5_NO_DISPATCH_CONTENT_KEY") == nullptr && !stampValidate();
    return enabled;
}

std::uint64_t Driver::verifyContentHits() {
    static const std::uint64_t every = [] {
        const char* text = std::getenv("APS5_VERIFY_CONTENT_HITS");
        return text != nullptr ? std::max<std::uint64_t>(std::strtoull(text, nullptr, 10), 1) : 0;
    }();
    return every;
}

std::uint64_t Driver::dispatchContentKey(std::uint64_t identity, std::span<const std::uint32_t> userData, bool& contentKey) {
    contentKey = false;
    std::shared_ptr<const DispatchShape> shape;
    {
        std::lock_guard lock(dispatchCacheMutex);
        if (const auto found = dispatchShapes.find(identity); found != dispatchShapes.end()) shape = found->second;
    }
    if (shape == nullptr) return 0;
    contentKey = true;
    return DispatchContentKey(identity, *shape, userData, [](std::uint64_t address, std::span<std::uint32_t> out) { return GuestMemory::CopyMapped(address, std::as_writable_bytes(out)) == GuestMemory::Compare::Equal; });
}

void Driver::noteKeyMiss(std::uint64_t identity, std::span<const std::uint32_t> userData, bool absent, bool contentKey, EntryCounters& counters) {
    if (keyMissMemory.size() > 65536) keyMissMemory.clear();
    const auto [it, first] = keyMissMemory.try_emplace(identity);
    auto& memory = it->second;
    if (absent) {
        if (contentKey) ++counters.missContentKey;
        if (first) {
            ++counters.missFirstSight;
        } else if (std::ranges::equal(memory.userData, userData)) {
            ++counters.missSameUser;
        } else {
            bool pointers = false, addresses = false, values = memory.userData.size() != userData.size();
            for (std::size_t i = 0; i < userData.size() && i < memory.userData.size(); ++i) {
                const auto changed = memory.userData[i] ^ userData[i];
                if (changed == 0) continue;
                if (i < counters.missWords.size()) ++counters.missWords[i];
                const auto mask = i < memory.userMasks.size() ? memory.userMasks[i] : 0u;
                if (i < 64 && (memory.pointerWords >> i & 1u) != 0) pointers = true;
                else if ((changed & ~mask) == 0) addresses = true;
                else values = true;
            }
            ++(values ? counters.missValues : pointers && addresses ? counters.missPointersAddresses : pointers ? counters.missPointers : counters.missAddresses);
        }
    }
    memory.userData.assign(userData.begin(), userData.end());
}

void Driver::lookupDispatch(std::uint64_t address, const Submission& submission, std::uint64_t key, std::uint64_t identity, std::span<const std::uint32_t> userData, bool contentKey, bool noDispatchCache, bool traceCache, bool profile, std::span<const ShaderRecompiler::MemoryRegion> memory, DispatchPhaseTiming& phaseTiming, std::array<double, DriverPhaseCount>& phaseMs, std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, std::shared_ptr<DispatchVariant>& keepVariant, std::vector<ShaderRecompiler::MemoryRegion>& captured, std::vector<std::uint32_t>& liveWords, bool& dataHit, bool& relocatedHit, bool& cached, bool& validated, std::shared_ptr<DispatchEntry>& missedEntry, bool& missedDiffering) {
    if (!noDispatchCache) {

        static const bool validateUnlocked = std::getenv("APS5_NO_UNLOCKED_VALIDATE") == nullptr;
        std::unique_lock cacheLock(dispatchCacheMutex);
        ++entryCounters.lookups;
        if (contentKey) ++entryCounters.contentLookups;
        const auto found = dispatchCache.find(key);
        std::shared_ptr<DispatchEntry> entry = found != dispatchCache.end() ? found->second : nullptr;
        if (entry == nullptr) ++entryCounters.absent;
        if (profile) noteKeyMiss(identity, userData, entry == nullptr, contentKey, entryCounters);
        if (validateUnlocked) cacheLock.unlock();
        phaseTiming.Phase(PhaseLookup);
        if (entry != nullptr) {
            validated = true;
            const auto& variants = entry->variants;

            std::shared_ptr<DispatchVariant> variant;
            std::size_t rank = 0;
            const auto generation = variants.front()->generation.load(std::memory_order_acquire);
            bool current = false;
            std::uint64_t restamped = 0;

            auto outcome = EntryOutcome::Differing;
            std::uint64_t imagesFlushed = 0, runsSynced = 0, retriesEqual = 0, retriesMoved = 0, compared = 0;
            std::vector<ShaderRecompiler::MemoryRegion> regions;

            std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
            // A relocated variant: its runs at the live pointer words, and those of the hit.
            std::vector<std::pair<std::uint64_t, std::uint64_t>> rebased, hitRuns;
            std::uint64_t refusedUser = 0, refusedFields = 0;
            bool movedPointers = false, movedAddresses = false;
            const auto waitedBeforeValidate = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            if (!stampValidate()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);

                std::optional<SampledReadScope> sampling;

                for (std::size_t i = 0; i < variants.size(); ++i) {
                    regions.clear();
                    const auto& candidate = *variants[i];
                    const bool relocatable = candidate.relocation != nullptr && contentKeys();
                    std::span<const std::uint32_t> positions;
                    std::span<const std::pair<std::uint64_t, std::uint64_t>> runs;
                    if (relocatable) {
                        // Every user word the profile does not relocate must be equal; a refusal
                        // is a differing variant without reading memory.
                        if (!UserWordsFit(*candidate.relocation, userData)) {
                            ++refusedUser;
                            ++compared;
                            if (i == 0) outcome = EntryOutcome::Differing;
                            continue;
                        }
                        rebaseRuns(candidate, userData, rebased);
                        appendRegionsAt(rebased, candidate.words, regions);
                        positions = candidate.relocation->maskPositions;
                        runs = rebased;
                    } else {
                        appendEntryRegions(candidate, regions);
                    }
                    ++compared;
                    auto result = validateVariant(address, submission.queue, candidate, regions, imagesFlushed, runsSynced, sampling, &liveData, positions, runs);

                    if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) {
                        result = validateVariant(address, submission.queue, candidate, regions, imagesFlushed, runsSynced, sampling, &liveData, positions, runs);
                        ++(result == EntryOutcome::Equal || result == EntryOutcome::EqualData ? retriesEqual : retriesMoved);
                    }
                    const bool equal = result == EntryOutcome::Equal || result == EntryOutcome::EqualData;
                    if (equal && relocatable && !FieldsFit(*candidate.relocation, candidate.words, liveData)) {
                        ++refusedFields;
                        result = EntryOutcome::Differing;
                    }
                    if (i == 0) outcome = result;
                    if (result == EntryOutcome::Equal || result == EntryOutcome::EqualData) {
                        outcome = result;
                        variant = variants[i];
                        rank = i;
                        dataHit = std::any_of(liveData.begin(), liveData.end(), [&](const auto& live) { return std::binary_search(candidate.dataPositions.begin(), candidate.dataPositions.end(), live.first); });
                        if (relocatable) {
                            movedPointers = !std::ranges::equal(userData, candidate.relocation->userData);
                            movedAddresses = std::any_of(liveData.begin(), liveData.end(), [&](const auto& live) { return std::binary_search(candidate.relocation->maskPositions.begin(), candidate.relocation->maskPositions.end(), live.first) && !std::binary_search(candidate.dataPositions.begin(), candidate.dataPositions.end(), live.first); });
                            relocatedHit = movedPointers || movedAddresses;
                            if (relocatedHit) hitRuns = rebased;
                        }
                        break;
                    }
                }
                current = variant != nullptr;
            } else {
                variant = variants.front();
                compared = 1;
                current = true;
                for (const auto& [begin, bytes] : variant->spans) {
                    GuestMemory::CollectWrites(begin, bytes);
                    if (!GuestMemory::UnchangedSince(begin, bytes, generation)) {
                        current = false;
                        break;
                    }
                }
                if (!current) {

                    std::uint64_t collected = 0;
                    for (const auto& [begin, bytes] : variant->spans) collected = std::max(collected, GuestMemory::CollectWrites(begin, bytes));
                    bool same = collected != 0;
                    if (same) {
                        const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);
                        PendingView pending;
                        pending.Load();
                        same = validateCaptured(address, submission.queue, variant->captured, *variant->compiled, false, pending);
                    }
                    if (same) {
                        restamped = collected;
                        current = true;
                    }
                }
                if (current) outcome = EntryOutcome::Equal;
            }
            phaseTiming.Phase(PhaseValidate);
            if (profile) {
                const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBeforeValidate, phaseMs[PhaseValidate]);
                phaseMs[PhaseValidate] -= waited;
                phaseMs[PhaseValidateWait] += waited;
            }
            if (!cacheLock.owns_lock()) cacheLock.lock();

            const auto again = dispatchCache.find(key);
            const bool untouched = again != dispatchCache.end() && again->second == entry && variants.front()->generation.load(std::memory_order_acquire) == generation;
            auto& counters = entryCounters;
            counters.validateUs += phaseMs[PhaseValidate] * 1000;
            counters.imagesFlushed += imagesFlushed;
            counters.runsSynced += runsSynced;
            for (std::size_t i = 0; i < compared && i < variants.size(); ++i) counters.runsValidated += variants[i]->runs.size();
            counters.retriesEqual += retriesEqual;
            counters.retriesMoved += retriesMoved;
            counters.variantsCompared += compared;
            counters.refusedUser += refusedUser;
            counters.refusedFields += refusedFields;
            switch (outcome) {
                case EntryOutcome::Equal: ++counters.equal; break;
                case EntryOutcome::EqualData: ++counters.equal; break;
                case EntryOutcome::Differing: ++counters.differing; break;
                case EntryOutcome::Inaccessible: ++counters.inaccessible; break;
                case EntryOutcome::QueuedLabel: ++counters.queuedLabel; break;
                case EntryOutcome::FlushingImage: ++counters.flushingImage; break;
                case EntryOutcome::PublishMoved: ++counters.publishMoved; break;
                case EntryOutcome::PendingMoved: ++counters.pendingMoved; break;
                case EntryOutcome::ForgetMoved: ++counters.forgetMoved; break;
            }
            if (current) {
                ++counters.variantHitsByRank[rank];
                compiledResult = variant->compiled;
                if (dataHit || relocatedHit) {

                    liveWords = variant->words;
                    for (const auto& [position, value] : liveData) liveWords[position] = value;
                    regions.clear();
                    if (relocatedHit) appendRegionsAt(hitRuns, liveWords, regions);
                    else appendEntryRegions(*variant, regions, &liveWords);
                    auto patched = std::make_shared<ShaderRecompiler::RecompileResult>(*variant->compiled);
                    if (dataHit) {
                        auto& descriptor = patched->bindings[variant->flatBinding].guestDescriptor;
                        for (std::size_t k = 0; k < variant->dataPositions.size(); ++k) {
                            if (variant->dataSlots[k] < descriptor.size()) descriptor[variant->dataSlots[k]] = liveWords[variant->dataPositions[k]];
                        }
                        ++counters.dataHits;
                        counters.dataWordsRefreshed += liveData.size();
                        ++counters.dataHitsByRank[rank];
                    }
                    if (relocatedHit) {
                        ApplyRelocation(*patched, *variant->relocation, liveWords, userData);
                        ++counters.relocatedHits;
                        if (movedPointers) ++counters.relocatedPointers;
                        if (movedAddresses) ++counters.relocatedAddresses;
                    }
                    compiledResult = std::move(patched);
                }
                if (stampValidate()) {
                    captured = variant->captured;
                } else {

                    captured.reserve(memory.size() + regions.size());
                    captured.assign(memory.begin(), memory.end());
                    captured.insert(captured.end(), regions.begin(), regions.end());
                    if (variant->forgetSerial != GuestMemory::ForgetSerial()) ++counters.forgetSinceInsert;
                }
                keepVariant = variant;
                cached = true;
                ++dispatchCacheHits;
                if (untouched) {
                    if (restamped != 0) variant->generation.store(restamped, std::memory_order_release);

                    if (rank != 0) {
                        auto rotated = std::make_shared<DispatchEntry>();
                        rotated->variants.reserve(variants.size());
                        rotated->variants.push_back(variant);
                        for (std::size_t i = 0; i < variants.size(); ++i) {
                            if (i != rank) rotated->variants.push_back(variants[i]);
                        }
                        rotated->touched = entry->touched;
                        rotated->order = entry->order;
                        again->second = std::move(rotated);
                    }

                    if (dispatchCacheHits - again->second->touched > dispatchCacheEntries() / 8) {
                        dispatchOrder.splice(dispatchOrder.begin(), dispatchOrder, again->second->order);
                        again->second->touched = dispatchCacheHits;
                        ++counters.touches;
                    }
                }
            } else {
                if (traceCache) std::fprintf(stderr, "[dispatch-cache] 0x%llx captured memory changed\n", static_cast<unsigned long long>(address));

                missedEntry = entry;
                missedDiffering = outcome == EntryOutcome::Differing;
                if (!untouched) ++counters.replaced;
            }
            if (profile && std::chrono::steady_clock::now() - counters.lastReport > std::chrono::seconds(10)) {
                counters.lastReport = std::chrono::steady_clock::now();
                reportDispatchCache(counters);
            }
            phaseTiming.Phase(PhaseRelock);
        }
    }
}

}
