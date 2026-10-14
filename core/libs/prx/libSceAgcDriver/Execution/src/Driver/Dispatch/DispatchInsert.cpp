#include "prx/libc/include/HostCommitMeter.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "ImageTableAbi.hpp"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

bool CacheableResult(const ShaderRecompiler::RecompileResult& compiled) {
    return compiled.poisonedSrtReads == 0u && std::none_of(compiled.imageTablePoison.begin(), compiled.imageTablePoison.end(), [](const ShaderRecompiler::ImageTableEntryPoison& poison) {
        return poison.reason == static_cast<std::uint32_t>(ShaderRecompiler::ImageTableAbi::PoisonReason::Unmapped) || poison.reason == static_cast<std::uint32_t>(ShaderRecompiler::ImageTableAbi::PoisonReason::Inactive);
    });
}

void Driver::insertDispatch(std::uint64_t address, std::uint64_t key, std::uint64_t identity, std::span<const std::uint32_t> userData, bool noDispatchCache, bool profile, const std::shared_ptr<const ShaderSnapshot>& registeredShader, std::uint64_t forgetAtCapture, std::span<const ShaderRecompiler::MemoryRegion> memory, const std::shared_ptr<ShaderMemory>& shaderMemory, const std::vector<ShaderRecompiler::MemoryRegion>& captured, const std::shared_ptr<const ShaderRecompiler::ResourceCapture>& capture, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, const std::shared_ptr<DispatchEntry>& missedEntry, bool missedDiffering, std::shared_ptr<DispatchVariant>& attachVariant, DispatchPhaseTiming& phaseTiming) {
    if (!noDispatchCache) {
        auto fresh = std::make_shared<DispatchVariant>();
        fresh->compiled = compiledResult;
        fresh->shader = registeredShader;
        fresh->forgetSerial = forgetAtCapture;

        std::vector<ShaderRecompiler::MemoryRegion> srtRegions;
        for (const auto& region : captured) {
            const bool registered = std::any_of(memory.begin(), memory.end(), [&](const auto& known) { return region.guestAddress >= known.guestAddress && region.guestAddress < known.guestAddress + known.bytes.size(); });
            if (registered) continue;
            srtRegions.push_back(region);
            fresh->runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            const auto count = region.bytes.size() / sizeof(std::uint32_t);
            const auto offset = fresh->words.size();
            fresh->words.resize(offset + count);
            std::memcpy(fresh->words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        }
        // The user words the walk only read through (candidates for relative runs) and the
        // inline V#s among the user words, for the content key and [dispatch-keymiss].
        const std::uint64_t userWords = userData.size() >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << userData.size()) - 1;
        const std::uint64_t pointerCandidates = capture != nullptr ? capture->userDataUse.asBase & ~capture->userDataUse.asValue & userWords : 0;
        std::shared_ptr<RelocationProfile> relocation;
        if (contentKeys() && capture != nullptr && compiledResult != nullptr) {
            relocation = std::make_shared<RelocationProfile>();
            relocation->userData.assign(userData.begin(), userData.end());
            // Same words, runs split where they stop or start being relative to a user word.
            if (pointerCandidates != 0) relocation->pointerWords = SplitRelativeRuns(srtRegions, capture->userDataUse, pointerCandidates, fresh->runs, fresh->runBases, fresh->words);
            FindBufferFields(fresh->runs, fresh->words, userData, capture->snapshot.buffers, capture->snapshot.flattenedSrt, *compiledResult, *relocation);
            relocation->forwarded = compiledResult->shaderDataUserWords;
        }

        DataWordPositionCounts dataCounts;
        if (dataHits() && !stampValidate() && capture != nullptr && !capture->readTrace.leaves.empty()) {
            for (std::size_t b = 0; b < compiledResult->bindings.size(); ++b) {
                if (compiledResult->bindings[b].role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
                fresh->flatBinding = static_cast<std::uint32_t>(b);
                dataCounts = DataWordPositions(fresh->runs, capture->readTrace.leaves, capture->readTrace.otherReads, fresh->words, compiledResult->bindings[b].guestDescriptor, fresh->dataPositions, fresh->dataSlots);
                break;
            }
        }
        std::shared_ptr<const DispatchShape> shape;
        if (relocation != nullptr && !relocation->Empty()) {
            // A masked compare reports the data positions' live words too (the data hits).
            auto& positions = relocation->maskPositions;
            positions.insert(positions.end(), fresh->dataPositions.begin(), fresh->dataPositions.end());
            std::sort(positions.begin(), positions.end());
            positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
            auto learned = std::make_shared<DispatchShape>(ShapeOf(*relocation, fresh->runs, fresh->runBases));
            if (!learned->userMasks.empty()) shape = std::move(learned);
        } else {
            fresh->runBases.clear();
        }
        // Even an empty profile is kept: a content key leaves user words out, so a hit has to
        // check them (an empty profile requires every user word equal).
        fresh->relocation = relocation;
        if (relocation != nullptr && traceContentKeys()) {
            std::fprintf(stderr, "[dispatch-content] insert 0x%llx key %016llx: %zu user words, pointer candidates %llx (asBase %llx asValue %llx, %zu reads), relative %llx, %zu runs, %zu buffers of which %u relocate (%zu walked fields, %zu user fields, %zu patches), %zu forwarded, exact V#s by reason (bda/null/no source/sources/pointer/walked/derived/flattened/copied) %u/%u/%u/%u/%u/%u/%u/%u/%u, shape %s\n",static_cast<unsigned long long>(address), static_cast<unsigned long long>(key), userData.size(), static_cast<unsigned long long>(pointerCandidates), static_cast<unsigned long long>(capture->userDataUse.asBase), static_cast<unsigned long long>(capture->userDataUse.asValue), capture->userDataUse.reads.size(), static_cast<unsigned long long>(relocation->pointerWords), fresh->runs.size(), capture->snapshot.buffers.size(), relocation->relocatableBuffers, relocation->wordFields.size(), relocation->userFields.size(), relocation->patches.size(), relocation->forwarded.size(), relocation->refused[0], relocation->refused[1], relocation->refused[2], relocation->refused[3], relocation->refused[4], relocation->refused[5], relocation->refused[6], relocation->refused[7], relocation->refused[8], shape != nullptr ? "learned" : "none");
        }
        bool stable = true;
        if (stampValidate()) {

            fresh->memory = shaderMemory;
            fresh->captured = captured;
            for (const auto& region : captured) {
                const auto begin = region.guestAddress;
                const auto end = begin + region.bytes.size();
                if (!fresh->spans.empty() && begin <= fresh->spans.back().first + fresh->spans.back().second + 65536) {
                    auto& last = fresh->spans.back();
                    last.second = static_cast<std::size_t>(std::max(last.first + last.second, end) - last.first);
                } else {
                    fresh->spans.emplace_back(begin, static_cast<std::size_t>(end - begin));
                }
            }
            std::uint64_t generation = 0;
            for (const auto& [begin, bytes] : fresh->spans) generation = std::max(generation, GuestMemory::CollectWrites(begin, bytes));
            stable = generation != 0;
            if (stable) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);
                stable = captureStable(captured);
            }
            fresh->generation.store(generation, std::memory_order_release);
        } else if (insertCompare()) {
            const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);
            stable = captureStable(srtRegions);
        }
        std::lock_guard cacheLock(dispatchCacheMutex);
        if (profile && missedEntry != nullptr && missedDiffering) classifyDiffering(address, key, *missedEntry->variants.front(), *fresh, capture.get(), entryCounters);
        if (relocation != nullptr) {
            ++(relocation->Empty() ? entryCounters.profilesEmpty : entryCounters.profilesBuilt);
            entryCounters.profilePointerWords += static_cast<std::uint64_t>(std::popcount(relocation->pointerWords));
            entryCounters.profileBuffers += relocation->relocatableBuffers;
            for (std::size_t reason = 0; reason < FieldRefusalCount; ++reason) entryCounters.fieldRefusals[reason] += relocation->refused[reason];
        }
        // The first shape of an identity stays: a content key must not change under its entries.
        if (shape != nullptr && dispatchShapes.size() < 65536 && dispatchShapes.try_emplace(identity, shape).second) ++entryCounters.shapesLearned;
        if (profile) {
            // [dispatch-keymiss]: what a later miss of this identity can blame (the words the walk
            // only read through, and the base bits of V#s copied from the user words).
            auto& known = keyMissMemory[identity];
            known.pointerWords = pointerCandidates;
            known.userMasks.assign(userData.size(), 0u);
            for (const auto& value : capture != nullptr ? std::span<const ShaderRecompiler::DescriptorValue>(capture->snapshot.buffers) : std::span<const ShaderRecompiler::DescriptorValue>{}) {
                if (value.dwordCount != 4u) continue;
                for (std::size_t i = 0; i + 4 <= userData.size(); ++i) {
                    if (userData[i] != value.dwords[0] || userData[i + 1] != value.dwords[1] || userData[i + 2] != value.dwords[2] || userData[i + 3] != value.dwords[3]) continue;
                    known.userMasks[i] |= BufferBaseLowMask;
                    known.userMasks[i + 1] |= BufferBaseHighMask;
                }
            }
        }
        if (stable) {
            ++entryCounters.inserts;
            entryCounters.runsInserted += fresh->runs.size();
            if (!fresh->dataPositions.empty()) {
                ++entryCounters.dataInserts;
                entryCounters.dataPositionsInserted += fresh->dataPositions.size();
            }
            entryCounters.dataLeavesUnmapped += dataCounts.unmapped;
            entryCounters.dataLeavesMismatched += dataCounts.mismatched;
            entryCounters.dataLeavesAliased += dataCounts.aliased;
            attachVariant = fresh;
            const auto found = dispatchCache.find(key);
            if (found == dispatchCache.end()) {
                auto created = std::make_shared<DispatchEntry>();
                created->touched = dispatchCacheHits;
                accountVariant(*fresh, true);
                created->variants.push_back(std::move(fresh));
                dispatchOrder.push_front(key);
                created->order = dispatchOrder.begin();
                dispatchCache.emplace(key, std::move(created));
            } else if (found->second != missedEntry && !found->second->variants.empty() && found->second->variants.front()->runs == fresh->runs && found->second->variants.front()->words == fresh->words) {

                attachVariant = found->second->variants.front();
            } else {

                const auto& old = found->second;
                auto replacement = std::make_shared<DispatchEntry>();
                replacement->variants.reserve(dispatchVariants());
                accountVariant(*fresh, true);
                replacement->variants.push_back(std::move(fresh));
                for (const auto& kept : old->variants) {
                    if (replacement->variants.size() < dispatchVariants()) {
                        replacement->variants.push_back(kept);
                    } else {
                        accountVariant(*kept, false);
                        ++entryCounters.variantsEvicted;
                    }
                }
                ++entryCounters.variantsInserted;
                replacement->touched = dispatchCacheHits;
                replacement->order = old->order;
                dispatchOrder.splice(dispatchOrder.begin(), dispatchOrder, replacement->order);
                found->second = std::move(replacement);
            }

            static const bool lru = std::getenv("APS5_NO_DISPATCH_LRU") == nullptr;
            const auto over = [&] { return dispatchCache.size() > dispatchCacheEntries() || (dispatchCache.size() > 1 && dispatchCacheVariantBytes > DispatchCacheBytes()); };
            if (over()) {
                if (lru) {
                    while (over()) {
                        eraseDispatchEntry(dispatchCache.find(dispatchOrder.back()));
                        ++dispatchCacheEvictions;
                    }
                } else {
                    dispatchCacheEvictions += dispatchCache.size();
                    dispatchCache.clear();
                    dispatchOrder.clear();
                    HostCommitLog::Count("dispatch-cache", -static_cast<std::int64_t>(dispatchCacheVariantBytes), -static_cast<std::int64_t>(dispatchCacheVariants));
                    dispatchCacheVariants = 0;
                    dispatchCacheVariantBytes = 0;
                }
            }
        } else {
            ++entryCounters.unstable;
        }
        phaseTiming.Phase(PhaseInsert);
    }
}

}
