#include "prx/libSceAgcDriver/Graphics/include/ScratchWindowUnion.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {
int failures = 0;
using Run = std::pair<std::uint64_t, std::uint64_t>;

void expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void refusals() {
    const auto rejects = [](std::initializer_list<Run> runs, std::uint64_t begin = 0,
                            std::uint64_t end = 128, std::uint64_t block = 8) {
        return !PlanScratchWindowUnion(std::span(runs.begin(), runs.size()), begin, end, block);
    };
    expect(rejects({}), "empty input must retain the old path");
    expect(rejects({{0, 8}}), "one window needs no batch");
    expect(rejects({{0, 8}, {8, 8}}), "empty run");
    expect(rejects({{0, 16}, {8, 24}}), "overlap may not inflate the kept-byte budget");
    expect(rejects({{16, 24}, {0, 8}}), "out-of-order runs");
    expect(rejects({{0, 8}, {17, 24}}), "misaligned begin");
    expect(rejects({{0, 8}, {16, 23}}), "misaligned end");
    expect(rejects({{0, 8}, {16, 24}}, 8), "preceding slice");
    expect(rejects({{0, 8}, {120, 136}}), "following slice");
    expect(rejects({{0, 8}, {16, 24}}, 0, 128, 0), "zero block size");
    expect(rejects({{0, 8}, {16, 24}}, 16, 8), "inverted slice");
    expect(rejects({{0, 8}, {32, 40}}), "excessive sparse span");
    expect(rejects({{0, 2u << 20u}, {4u << 20u, 6u << 20u}}, 0, 6u << 20u), "absolute scratch-gap cap");
    const std::array<Run, 2> gap{{{16, 24}, {32, 48}}};
    expect(PlanScratchWindowUnion(gap, 16, 80, 8) == std::optional<Run>{{16, 48}}, "one slice with a bounded gap must batch");
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    const std::array<Run, 2> high{{{limit - 31, limit - 23}, {limit - 15, limit - 7}}};
    const auto highPlan = PlanScratchWindowUnion(high, limit - 31, limit - 7, 8);
    expect(highPlan && *highPlan == Run{limit - 31, limit - 7}, "high addresses must not wrap");
    const std::array<Run, 2> huge{{{0, limit / 2}, {limit / 2, limit}}};
    expect(PlanScratchWindowUnion(huge, 0, limit, 1).has_value(), "adjacent very large runs must not overflow a doubled-byte budget");
}

void exactPublication() {
    // Exhaust every selection in a small slice. CPU-owned and another GPU producer's bytes
    // alternate in the omitted blocks. Retiling the envelope must not publish either gap.
    constexpr std::size_t block = 8, blocks = 8, base = 16, bytes = base + block * blocks + 16;
    unsigned accepted = 0;
    for (unsigned selected = 1; selected < (1u << blocks); ++selected) {
        std::vector<Run> runs;
        for (std::size_t i = 0; i < blocks; ++i) {
            if ((selected & (1u << i)) == 0) continue;
            const auto begin = base + i * block;
            if (!runs.empty() && runs.back().second == begin) runs.back().second += block;
            else runs.emplace_back(begin, begin + block);
        }
        const auto original = runs;
        const auto plan = PlanScratchWindowUnion(runs, base, base + blocks * block, block);
        expect(runs == original, "planning changed the publication ranges");
        if (!plan) continue;
        ++accepted;
        std::array<unsigned char, bytes> guest{}, expected{};
        for (std::size_t i = 0; i < bytes; ++i) guest[i] = expected[i] = static_cast<unsigned char>(i * 37u + 11u);
        for (std::size_t i = 0; i < blocks; ++i) {
            if ((selected & (1u << i)) != 0) std::fill_n(expected.begin() + base + i * block, block, 0xa5);
        }
        std::vector<unsigned char> scratch(static_cast<std::size_t>(plan->second - plan->first), 0xa5);
        for (const auto& [begin, end] : runs) {
            std::copy_n(scratch.begin() + static_cast<std::size_t>(begin - plan->first),
                static_cast<std::size_t>(end - begin), guest.begin() + static_cast<std::size_t>(begin));
        }
        expect(guest == expected, "envelope publication changed a gap or slice canary");
    }
    expect(accepted != 0, "the exhaustive publication test must exercise batching");
}

void descriptorLimits() {
    expect(ScratchWindowDescriptorFits(0, 65536, 256, 65536), "exact descriptor limit");
    expect(!ScratchWindowDescriptorFits(0, 65537, 256, 65536), "widening beyond descriptor limit");
    expect(ScratchWindowDescriptorFits(256, 65536, 256, 65536), "aligned descriptor offset has zero residual");
    expect(!ScratchWindowDescriptorFits(260, 65536, 256, 65536), "descriptor residual offset must count toward the limit");
    expect(ScratchWindowDescriptorFits(3, 1, 1, 4), "minimum four-byte descriptor alignment");
    expect(!ScratchWindowDescriptorFits(3, 2, 1, 4), "descriptor range rounds upward to words");
    expect(!ScratchWindowDescriptorFits(0, 0, 256, 65536), "empty descriptor range");
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    expect(!ScratchWindowDescriptorFits(0, limit, 4, limit), "descriptor range rounding overflow");
    expect(!ScratchWindowDescriptorFits(1ull << 32u, 4, 1ull << 33u, limit), "push-constant descriptor base exceeds uint32");
    const std::array<Run, 2> original{{{0, 65536}, {131072, 196608}}};
    const auto envelope = PlanScratchWindowUnion(original, 0, 262144, 65536);
    expect(envelope && !ScratchWindowDescriptorFits(0, envelope->second - envelope->first, 256, 131072),
        "otherwise eligible union must fall back when its descriptor is too large");
    for (const auto& [begin, end] : original) {
        expect(ScratchWindowDescriptorFits(0, end - begin, 256, 131072), "original split windows still fit after union refusal");
    }
}
}

int main() {
    refusals();
    exactPublication();
    descriptorLimits();
    if (failures == 0) std::puts("scratch window union tests passed");
    return failures == 0 ? 0 : 1;
}
