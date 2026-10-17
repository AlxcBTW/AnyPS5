#include "prx/libSceAgcDriver/Graphics/include/BarrierBatch.hpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

constexpr VkPipelineStageFlags All = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
constexpr VkPipelineStageFlags Transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
constexpr VkPipelineStageFlags Compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags Host = VK_PIPELINE_STAGE_HOST_BIT;
constexpr VkAccessFlags SW = VK_ACCESS_SHADER_WRITE_BIT;
constexpr VkAccessFlags SR = VK_ACCESS_SHADER_READ_BIT;
constexpr VkAccessFlags TW = VK_ACCESS_TRANSFER_WRITE_BIT;
constexpr VkAccessFlags TR = VK_ACCESS_TRANSFER_READ_BIT;
constexpr VkAccessFlags CAW = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags HR = VK_ACCESS_HOST_READ_BIT;

void testCovers() {
    const BarrierMask passEnd{All, All, SW | CAW | TW, SR | SW | TR | TW};
    const BarrierMask leading{All, Transfer, SW | TW | CAW, TW};
    Expect(Covers(passEnd, leading), "a pass end's barrier covers the label run's leading barrier");
    Expect(!Covers(leading, passEnd), "the leading barrier does not cover the pass end's");
    const BarrierMask toHost{All, Transfer | Host, SW | TW | CAW, TW | HR};
    Expect(!Covers(passEnd, toHost), "ALL_COMMANDS does not hold the host stage");
    Expect(Covers(toHost, leading), "a wider destination covers the narrower one");
    const BarrierMask memory{All, All, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    Expect(Covers(memory, passEnd), "MEMORY_READ and MEMORY_WRITE hold the specific accesses");
    Expect(Covers(passEnd, passEnd) && Covers(BarrierMask{}, BarrierMask{}), "a barrier covers itself, the empty one the empty one");
    const BarrierMask computeOnly{Compute, Compute, SW, SR};
    Expect(!Covers(computeOnly, passEnd), "a compute-only barrier does not cover ALL_COMMANDS");
}

void testMerge() {
    const BarrierMask a{Compute, Transfer, SW, TR};
    const BarrierMask b{Transfer, Compute, TW, SR};
    const auto merged = Merge(a, b);
    Expect(merged == BarrierMask{Compute | Transfer, Compute | Transfer, SW | TW, TR | SR}, "a merge unites every mask");
    Expect(Covers(merged, a) && Covers(merged, b), "the merge covers both");
}

void testCanMergeExactly() {
    const BarrierMask wide{All, All, SW | TW, SR | SW | TR | TW};
    const BarrierMask narrow{Compute, Transfer, SW, TR};
    Expect(CanMergeExactly(wide, narrow) && CanMergeExactly(narrow, wide), "one covering the other merges");
    const BarrierMask sameSource1{Compute, Transfer, SW, TR};
    const BarrierMask sameSource2{Compute, Compute, SW, SR};
    Expect(CanMergeExactly(sameSource1, sameSource2), "the same source merges, the destinations unite");
    const BarrierMask sameDest1{Compute, Transfer, SW, TR};
    const BarrierMask sameDest2{Transfer, Transfer, TW, TR};
    Expect(CanMergeExactly(sameDest1, sameDest2), "the same destination merges, the sources unite");
    const BarrierMask crossing1{Compute, Transfer, SW, TR};
    const BarrierMask crossing2{Transfer, Compute, TW, SR};
    Expect(!CanMergeExactly(crossing1, crossing2), "sources and destinations that both differ would order pairs neither orders");
}

void testCoalesce() {
    Expect(Coalesce({}).empty(), "no barriers, none coalesced");
    const BarrierMask passEnd{All, All, SW | CAW | TW, SR | SW | TR | TW};
    const BarrierMask leading{All, Transfer, SW | TW | CAW, TW};
    const BarrierMask run[] = {passEnd, leading, leading};
    const auto result = Coalesce(run);
    Expect(result.size() == 1 && result[0] == passEnd, "a pass end followed by barriers it covers is one barrier");
    const BarrierMask apart[] = {{Compute, Transfer, SW, TR}, {Transfer, Compute, TW, SR}, {Transfer, Compute, TW, SR}};
    const auto kept = Coalesce(apart);
    Expect(kept.size() == 2 && kept[0] == apart[0] && kept[1] == apart[1], "crossing barriers stay apart, a repeat folds into its neighbour");
    const BarrierMask pair[] = {{Compute, All, SW, SR | TR}, {Compute, All, SW, TW}};
    const auto united = Coalesce(pair);
    Expect(united.size() == 1 && united[0] == BarrierMask{Compute, All, SW, SR | TR | TW}, "the same source and destination stage unite the accesses");
}

std::vector<StoreSpan> spans(std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> list, std::uint64_t buffer = 1) {
    std::vector<StoreSpan> result;
    for (const auto& [begin, end] : list) result.push_back({buffer, begin, end});
    return result;
}

void testWaves() {
    Expect(PlanStoreWaves({}).order.empty(), "no stores, no waves");
    auto alternating = spans({{0, 4}, {8, 12}, {0, 4}, {8, 12}, {0, 4}, {8, 12}});
    auto plan = PlanStoreWaves(alternating);
    Expect(CountSequentialBarriers(alternating) == 4, "alternating stores take four sequential barriers");
    Expect(plan.waveEnds.size() == 3 && plan.waveEnds[0] == 2 && plan.waveEnds[1] == 4 && plan.waveEnds[2] == 6, "alternating stores form three waves of two");
    Expect(VerifyStoreWaves(alternating, plan) == 0, "the alternating plan verifies");
    auto disjoint = spans({{0, 4}, {4, 8}, {8, 12}});
    plan = PlanStoreWaves(disjoint);
    Expect(plan.waveEnds.size() == 1 && plan.order == std::vector<std::uint32_t>({0, 1, 2}) && CountSequentialBarriers(disjoint) == 0, "disjoint stores are one wave in input order");
    auto chain = spans({{0, 4}, {0, 4}, {0, 4}, {0, 4}});
    plan = PlanStoreWaves(chain);
    Expect(plan.waveEnds.size() == 4 && CountSequentialBarriers(chain) == 3, "a chain of four needs three barriers either way");
    auto spanning = spans({{0, 4}, {8, 12}, {2, 10}, {20, 24}});
    plan = PlanStoreWaves(spanning);
    Expect(plan.waveEnds.size() == 2 && plan.order == std::vector<std::uint32_t>({0, 1, 3, 2}), "a spanning store follows both and a disjoint one moves ahead of it");
    Expect(VerifyStoreWaves(spanning, plan) == 0, "the spanning plan verifies");
    std::vector<StoreSpan> buffers = {{1, 0, 4}, {2, 0, 4}, {1, 0, 4}};
    plan = PlanStoreWaves(buffers);
    Expect(plan.waveEnds.size() == 2 && CountSequentialBarriers(buffers) == 1, "equal ranges in different buffers are disjoint");
    auto touching = spans({{0, 4}, {4, 8}, {3, 5}});
    Expect(PlanStoreWaves(touching).waveEnds.size() == 2, "a store over a border waits for both neighbours");
}

void testWavesNeverMoreThanSequential() {
    std::uint64_t state = 12345;
    const auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(state >> 33);
    };
    for (int round = 0; round < 300; ++round) {
        const auto count = 1 + next() % 40;
        std::vector<StoreSpan> stores;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint64_t begin = (next() % 12) * 4;
            stores.push_back({1 + next() % 2, begin, begin + 4 + (next() % 3) * 4});
        }
        const auto plan = PlanStoreWaves(stores);
        std::string what;
        Expect(VerifyStoreWaves(stores, plan, &what) == 0, what.c_str());
        Expect(plan.waveEnds.empty() || plan.waveEnds.size() - 1 <= CountSequentialBarriers(stores), "a plan never takes more barriers than the one-by-one path");
    }
}

void testWavesLarge() {
    std::vector<StoreSpan> stores;
    for (int i = 0; i < 600; ++i) stores.push_back({1, static_cast<std::uint64_t>(i % 7) * 4, static_cast<std::uint64_t>(i % 7) * 4 + 4});
    const auto plan = PlanStoreWaves(stores);
    std::string what;
    Expect(VerifyStoreWaves(stores, plan, &what) == 0, what.c_str());
    Expect(plan.order.size() == stores.size(), "a large plan keeps every store");
}

void testVerifyCatchesBadPlans() {
    const auto stores = spans({{0, 4}, {0, 4}, {8, 12}});
    StoreWaves sameWave{{0, 1, 2}, {3}};
    Expect(VerifyStoreWaves(stores, sameWave) == 1, "overlapping stores in one wave are a violation");
    StoreWaves reversed{{1, 0, 2}, {1, 2, 3}};
    Expect(VerifyStoreWaves(stores, reversed) >= 1, "overlapping stores out of order are a violation");
    StoreWaves missing{{0, 1}, {1, 2}};
    std::string what;
    Expect(VerifyStoreWaves(stores, missing, &what) >= 1 && !what.empty(), "a missing store is a violation with a description");
    StoreWaves doubled{{0, 0, 1, 2}, {1, 2, 3, 4}};
    Expect(VerifyStoreWaves(stores, doubled) >= 1, "a doubled store is a violation");
}

void testLedger() {
    const BarrierMask staging{All | Host, Transfer, SW | TW, TR};
    BarrierLedger chained;
    chained.NoteOld("staging-in leading", staging);
    chained.NoteChain(SR | SW | TR | TW | HR);
    Expect(chained.Settle() == 0 && chained.Chain() == 1 && chained.Strict() == 0, "a covered destination access resolves by chain");
    BarrierLedger uncovered;
    uncovered.NoteOld("staging-in leading", staging);
    uncovered.NoteChain(SR | SW);
    std::string what;
    Expect(uncovered.Settle(&what) == 1 && what.find("staging-in leading") != std::string::npos, "an uncovered destination access is a violation named by its site");
    BarrierLedger toHost;
    toHost.NoteOld("host", BarrierMask{All, Host, SW, HR});
    toHost.NoteChain(HR | SR | SW | TR | TW);
    Expect(toHost.Settle() == 1, "the chain never resolves a host destination");
    BarrierLedger merged;
    const BarrierMask image{All, Transfer, SW | CAW, TR};
    const BarrierMask seed{All | Host, Transfer, SW | TW, TR | TW};
    merged.NoteOld("image", image);
    merged.NoteOld("seed", seed);
    merged.NoteEmitted(Merge(image, seed));
    Expect(merged.Settle() == 0 && merged.Strict() == 2, "a merged barrier resolves both of its parts strictly");
    BarrierLedger partial;
    partial.NoteOld("image", image);
    partial.NoteOld("seed", seed);
    partial.NoteEmitted(image);
    Expect(partial.Settle() == 1, "a barrier that covers only one part leaves the other open");
}

}

int main() {
    testCovers();
    testMerge();
    testCanMergeExactly();
    testCoalesce();
    testWaves();
    testWavesNeverMoreThanSequential();
    testWavesLarge();
    testVerifyCatchesBadPlans();
    testLedger();
    if (failures != 0) {
        std::fprintf(stderr, "%d barrier batch checks failed\n", failures);
        return 1;
    }
    std::printf("barrier batch tests passed\n");
    return 0;
}
