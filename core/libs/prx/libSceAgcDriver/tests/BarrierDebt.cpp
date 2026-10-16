#include "prx/libSceAgcDriver/Graphics/include/BarrierDebt.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
using Images = std::vector<std::pair<VkImage, bool>>;

VkImage image(std::uint64_t id) {
    static_assert(sizeof(VkImage) == sizeof(std::uint64_t));
    VkImage handle = VK_NULL_HANDLE;
    std::memcpy(&handle, &id, sizeof(handle));
    return handle;
}

constexpr VkAccessFlags ShaderAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

// A dispatch's trailing barrier as VulkanDevice::recordDispatch hands it over.
OwedBarrier TrailingBarrier() {
    return OwedBarrier{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT};
}

HazardFootprint footprint(Ranges reads, Ranges writes, Images images = {}) {
    HazardFootprint result;
    result.reads = std::move(reads);
    result.writes = std::move(writes);
    result.images = std::move(images);
    return result;
}

void testFootprint() {
    HazardFootprint listed;
    Expect(listed.Known(), "a footprint without a reason is listed");
    HazardFootprint unlisted;
    unlisted.unknown = "address-based";
    Expect(!unlisted.Known(), "a footprint with a reason is not listed");
    const auto both = footprint({{0x10, 0x20}}, {{0x30, 0x40}}, {{image(1), true}});
    const auto access = both.Access();
    Expect(access.reads.size() == 1 && access.writes.size() == 1 && access.images.size() == 1 && access.attachments.empty() && !access.anyReads && !access.anyWrites, "the access view carries the lists and no unknown accesses");
}

void testDecide() {
    BarrierDebt debt;
    const auto reader = footprint({{0x1000, 0x2000}}, {});
    Expect(!debt.Active() && debt.Members() == 0 && debt.Decide(reader.Access(), ShaderAccess) == DebtVerdict::NotOwed, "nothing owed: no verdict");
    const auto producer = footprint({{0x1000, 0x1100}}, {{0x4000, 0x4100}});
    debt.Start(producer.Access(), TrailingBarrier(), ShaderAccess);
    Expect(debt.Active() && debt.Members() == 1 && debt.HeadCovered() == ShaderAccess, "a started run has one member and its head's coverage");
    // read after read of the producer's input is fine, a read of its output is not
    Expect(debt.Decide(footprint({{0x1000, 0x1100}}, {}).Access(), ShaderAccess) == DebtVerdict::Keep, "read after read");
    PassHazard kind = PassHazard::None;
    Expect(debt.Decide(footprint({{0x40f0, 0x4200}}, {}).Access(), ShaderAccess, &kind) == DebtVerdict::Hazard && kind == PassHazard::ReadAfterWrite, "read after write");
    Expect(debt.Decide(footprint({}, {{0x4080, 0x4090}}).Access(), ShaderAccess, &kind) == DebtVerdict::Hazard && kind == PassHazard::WriteAfterWrite, "write after write");
    Expect(debt.Decide(footprint({}, {{0x10ff, 0x1200}}).Access(), ShaderAccess, &kind) == DebtVerdict::Hazard && kind == PassHazard::WriteAfterRead, "write after read");
    Expect(debt.Decide(footprint({{0x2000, 0x3000}}, {{0x4100, 0x4200}}).Access(), ShaderAccess) == DebtVerdict::Keep, "disjoint reads and writes, writes touching the producer's");
    Expect(debt.Decide(footprint({}, {}).Access(), ShaderAccess) == DebtVerdict::Keep, "a dispatch that touches no listed memory");
    // the verdict does not change the run
    Expect(debt.Members() == 1, "Decide does not join");
}

void testImagesInRun() {
    BarrierDebt debt;
    debt.Start(footprint({}, {}, {{image(5), false}, {image(6), true}}).Access(), TrailingBarrier(), ShaderAccess);
    PassHazard kind = PassHazard::None;
    Expect(debt.Decide(footprint({}, {}, {{image(5), false}}).Access(), ShaderAccess) == DebtVerdict::Keep, "two reads of an image");
    Expect(debt.Decide(footprint({}, {}, {{image(5), true}}).Access(), ShaderAccess, &kind) == DebtVerdict::Hazard && kind == PassHazard::Image, "a store to an image the run read");
    Expect(debt.Decide(footprint({}, {}, {{image(6), false}}).Access(), ShaderAccess, &kind) == DebtVerdict::Hazard && kind == PassHazard::Image, "a read of an image the run stored");
    Expect(debt.Decide(footprint({}, {}, {{image(7), true}}).Access(), ShaderAccess) == DebtVerdict::Keep, "a store to another image");
    Expect(debt.Decide(footprint({}, {}, {{VK_NULL_HANDLE, true}}).Access(), ShaderAccess) == DebtVerdict::Keep, "null images are ignored");
}

void testHeadCoverage() {
    BarrierDebt debt;
    debt.Start(footprint({}, {{0x100, 0x110}}).Access(), TrailingBarrier(), VK_ACCESS_SHADER_READ_BIT);
    Expect(debt.Decide(footprint({{0x900, 0x910}}, {}).Access(), ShaderAccess) == DebtVerdict::Uncovered, "a head that covered only shader reads takes no dispatch that writes");
    Expect(debt.Decide(footprint({{0x900, 0x910}}, {}).Access(), VK_ACCESS_SHADER_READ_BIT) == DebtVerdict::Keep, "...but one that asks for reads only");
    debt.Start(footprint({}, {{0x100, 0x110}}).Access(), TrailingBarrier(), 0);
    Expect(debt.Decide(footprint({}, {}).Access(), ShaderAccess) == DebtVerdict::Uncovered, "an uncovered head takes no follower");
}

void testJoinAndTake() {
    BarrierDebt debt;
    debt.Start(footprint({{0x10, 0x20}}, {{0x100, 0x200}}).Access(), OwedBarrier{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT}, ShaderAccess);
    const auto second = footprint({{0x30, 0x40}}, {{0x300, 0x400}});
    Expect(debt.Decide(second.Access(), ShaderAccess) == DebtVerdict::Keep, "an independent second dispatch");
    debt.Join(second.Access(), OwedBarrier{VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT});
    Expect(debt.Members() == 2, "two members");
    // the run's accesses accumulate: the third dispatch is checked against both
    Expect(debt.Decide(footprint({{0x110, 0x120}}, {}).Access(), ShaderAccess) == DebtVerdict::Hazard, "reads what the first member wrote");
    Expect(debt.Decide(footprint({{0x310, 0x320}}, {}).Access(), ShaderAccess) == DebtVerdict::Hazard, "reads what the second member wrote");
    Expect(debt.Decide(footprint({}, {{0x35, 0x38}}).Access(), ShaderAccess) == DebtVerdict::Hazard, "writes what the second member read");
    const auto owed = debt.Barrier();
    Expect(owed.sourceStage == (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT) && owed.destinationStage == (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT)
        && owed.sourceAccess == (VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT) && owed.destinationAccess == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT), "the owed barrier is the union of the members'");
    const auto taken = debt.Take();
    Expect(taken.sourceAccess == owed.sourceAccess && !debt.Active() && debt.Members() == 0 && debt.HeadCovered() == 0, "Take hands the barrier over and ends the run");
    Expect(debt.Decide(footprint({{0x110, 0x120}}, {}).Access(), ShaderAccess) == DebtVerdict::NotOwed, "after Take the accesses are gone too");
    debt.Start(footprint({}, {{0x500, 0x510}}).Access(), TrailingBarrier(), ShaderAccess);
    Expect(debt.Decide(footprint({{0x110, 0x120}}, {}).Access(), ShaderAccess) == DebtVerdict::Keep && debt.Decide(footprint({{0x505, 0x506}}, {}).Access(), ShaderAccess) == DebtVerdict::Hazard, "a new run knows only its own accesses");
    debt.Clear();
    Expect(!debt.Active(), "Clear ends the run");
}

// The brute-force oracle against the interval sets, over many random runs.
struct Random {
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    std::uint64_t below(std::uint64_t limit) { return next() % limit; }
};

HazardFootprint randomFootprint(Random& random, std::uint64_t space) {
    HazardFootprint use;
    for (auto count = random.below(4); count != 0; --count) {
        const auto begin = random.below(space);
        use.reads.emplace_back(begin, begin + random.below(space / 8 + 1));
    }
    for (auto count = random.below(3); count != 0; --count) {
        const auto begin = random.below(space);
        use.writes.emplace_back(begin, begin + random.below(space / 8 + 1));
    }
    for (auto count = random.below(3); count != 0; --count) use.images.emplace_back(image(random.below(5)), random.below(2) != 0);
    return use;
}

void testAgainstOracle() {
    Random random;
    std::uint64_t keeps = 0, hazards = 0;
    for (int round = 0; round < 4000; ++round) {
        const auto space = round % 3 == 0 ? 64u : round % 3 == 1 ? 1024u : 65536u;
        BarrierDebt debt;
        std::vector<HazardFootprint> members;
        for (int index = 0; index < 12; ++index) {
            const auto use = randomFootprint(random, space);
            if (!debt.Active()) {
                debt.Start(use.Access(), TrailingBarrier(), ShaderAccess);
                members = {use};
                continue;
            }
            bool conflict = false;
            for (const auto& member : members) conflict = conflict || AccessesConflict(member.Access(), use.Access());
            const auto verdict = debt.Decide(use.Access(), ShaderAccess);
            if ((verdict == DebtVerdict::Keep) == conflict) {
                std::fprintf(stderr, "FAIL: round %d member %d: the interval sets say %s, the pairwise comparison says %s\n", round, index, verdict == DebtVerdict::Keep ? "keep" : "hazard", conflict ? "conflict" : "independent");
                ++failures;
                return;
            }
            if (verdict == DebtVerdict::Keep) {
                debt.Join(use.Access(), TrailingBarrier());
                members.push_back(use);
                ++keeps;
            } else {
                debt.Take();
                debt.Start(use.Access(), TrailingBarrier(), ShaderAccess);
                members = {use};
                ++hazards;
            }
        }
    }
    // the generator must exercise both outcomes
    Expect(keeps > 100 && hazards > 100, "the random runs keep and settle often enough to mean something");
}

void testOracle() {
    Expect(!AccessesConflict(footprint({{0, 10}}, {}).Access(), footprint({{0, 10}}, {}).Access()), "read and read");
    Expect(AccessesConflict(footprint({}, {{0, 10}}).Access(), footprint({{9, 12}}, {}).Access()), "write then read");
    Expect(AccessesConflict(footprint({{0, 10}}, {}).Access(), footprint({}, {{9, 12}}).Access()), "read then write");
    Expect(AccessesConflict(footprint({}, {{0, 10}}).Access(), footprint({}, {{9, 12}}).Access()), "write then write");
    Expect(!AccessesConflict(footprint({}, {{0, 10}}).Access(), footprint({{10, 12}}, {{20, 30}}).Access()), "touching ranges do not overlap");
    Expect(!AccessesConflict(footprint({}, {{5, 5}}).Access(), footprint({{0, 10}}, {}).Access()), "an empty write touches nothing");
    Expect(AccessesConflict(footprint({}, {}, {{image(1), true}}).Access(), footprint({}, {}, {{image(1), false}}).Access()), "a stored image read later");
    Expect(!AccessesConflict(footprint({}, {}, {{image(1), false}}).Access(), footprint({}, {}, {{image(1), false}}).Access()), "an image read twice");
    PassAccess unknownWriter{};
    unknownWriter.anyWrites = true;
    Expect(AccessesConflict(unknownWriter, footprint({{0, 4}}, {}).Access()), "an unknown writer against a ranged read");
    Expect(!AccessesConflict(unknownWriter, footprint({}, {}, {{image(1), true}}).Access()), "an unknown writer against an image only");
}

}

int main() {
    testFootprint();
    testDecide();
    testImagesInRun();
    testHeadCoverage();
    testJoinAndTake();
    testAgainstOracle();
    testOracle();
    if (failures != 0) {
        std::fprintf(stderr, "%d barrier debt checks failed\n", failures);
        return 1;
    }
    std::printf("barrier debt tests passed\n");
    return 0;
}
