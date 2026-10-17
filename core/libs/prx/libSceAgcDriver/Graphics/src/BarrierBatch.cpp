#include "prx/libSceAgcDriver/Graphics/include/BarrierBatch.hpp"
#include <algorithm>
#include <cstdio>

namespace AgcDriver::Graphics {

namespace {

constexpr VkAccessFlags WriteAccesses = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
constexpr VkAccessFlags ReadAccesses = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;

bool StageCovers(VkPipelineStageFlags outer, VkPipelineStageFlags inner) {
    auto rest = inner & ~outer;
    if ((outer & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) != 0) rest &= VK_PIPELINE_STAGE_HOST_BIT;
    return rest == 0;
}

bool AccessCovers(VkAccessFlags outer, VkAccessFlags inner) {
    auto rest = inner & ~outer;
    if ((outer & VK_ACCESS_MEMORY_WRITE_BIT) != 0) rest &= ~WriteAccesses;
    if ((outer & VK_ACCESS_MEMORY_READ_BIT) != 0) rest &= ~ReadAccesses;
    return rest == 0;
}

bool Overlap(const StoreSpan& a, const StoreSpan& b) {
    return a.buffer == b.buffer && a.begin < b.end && b.begin < a.end;
}

}

bool Covers(const BarrierMask& outer, const BarrierMask& inner) {
    return StageCovers(outer.srcStage, inner.srcStage) && StageCovers(outer.dstStage, inner.dstStage) && AccessCovers(outer.srcAccess, inner.srcAccess) && AccessCovers(outer.dstAccess, inner.dstAccess);
}

BarrierMask Merge(const BarrierMask& first, const BarrierMask& second) {
    return {first.srcStage | second.srcStage, first.dstStage | second.dstStage, first.srcAccess | second.srcAccess, first.dstAccess | second.dstAccess};
}

bool CanMergeExactly(const BarrierMask& first, const BarrierMask& second) {
    if (Covers(first, second) || Covers(second, first)) return true;
    if (first.srcStage == second.srcStage && first.srcAccess == second.srcAccess) return true;
    return first.dstStage == second.dstStage && first.dstAccess == second.dstAccess;
}

std::vector<BarrierMask> Coalesce(std::span<const BarrierMask> run) {
    std::vector<BarrierMask> result;
    for (const auto& barrier : run) {
        if (result.empty() || !CanMergeExactly(result.back(), barrier)) result.push_back(barrier);
        else if (Covers(barrier, result.back())) result.back() = barrier;
        else if (!Covers(result.back(), barrier)) result.back() = Merge(result.back(), barrier);
    }
    return result;
}

StoreWaves PlanStoreWaves(std::span<const StoreSpan> stores) {
    StoreWaves plan;
    const auto count = stores.size();
    if (count == 0) return plan;
    std::vector<std::uint32_t> level(count, 0);
    if (count <= 256) {
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (Overlap(stores[i], stores[j])) level[i] = std::max(level[i], level[j] + 1);
            }
        }
    } else {
        std::uint32_t wave = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t j = start; j < i; ++j) {
                if (!Overlap(stores[i], stores[j])) continue;
                ++wave;
                start = i;
                break;
            }
            level[i] = wave;
        }
    }
    const auto waves = *std::max_element(level.begin(), level.end()) + 1;
    for (std::uint32_t wave = 0; wave < waves; ++wave) {
        for (std::size_t i = 0; i < count; ++i) {
            if (level[i] == wave) plan.order.push_back(static_cast<std::uint32_t>(i));
        }
        plan.waveEnds.push_back(static_cast<std::uint32_t>(plan.order.size()));
    }
    return plan;
}

std::uint32_t CountSequentialBarriers(std::span<const StoreSpan> stores) {
    std::uint32_t barriers = 0;
    for (std::size_t i = 0; i < stores.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (!Overlap(stores[i], stores[j])) continue;
            ++barriers;
            break;
        }
    }
    return barriers;
}

std::uint32_t VerifyStoreWaves(std::span<const StoreSpan> stores, const StoreWaves& plan, std::string* first) {
    std::uint32_t violations = 0;
    const auto fail = [&](const char* what, std::size_t a, std::size_t b) {
        if (violations++ == 0 && first != nullptr) {
            char text[128];
            std::snprintf(text, sizeof(text), "%s (stores %zu and %zu)", what, a, b);
            *first = text;
        }
    };
    if (plan.order.size() != stores.size()) fail("a store is missing or doubled", plan.order.size(), stores.size());
    std::vector<std::int64_t> position(stores.size(), -1), wave(stores.size(), -1);
    std::uint32_t begin = 0;
    for (std::size_t w = 0; w < plan.waveEnds.size(); ++w) {
        for (auto k = begin; k < plan.waveEnds[w] && k < plan.order.size(); ++k) {
            const auto store = plan.order[k];
            if (store >= stores.size() || position[store] >= 0) {
                fail("a store is missing or doubled", store, k);
                continue;
            }
            position[store] = k;
            wave[store] = static_cast<std::int64_t>(w);
        }
        begin = plan.waveEnds[w];
    }
    for (std::size_t i = 0; i < stores.size(); ++i) {
        if (position[i] < 0) fail("a store is not planned", i, i);
    }
    for (std::size_t i = 0; i < stores.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (position[i] < 0 || position[j] < 0 || !Overlap(stores[i], stores[j])) continue;
            if (wave[i] == wave[j]) fail("overlapping stores in one wave", j, i);
            else if (wave[i] < wave[j]) fail("overlapping stores out of order", j, i);
        }
    }
    return violations;
}

void BarrierLedger::NoteOld(const char* site, const BarrierMask& mask) {
    open.push_back({site, mask});
}

void BarrierLedger::NoteEmitted(const BarrierMask& mask) {
    for (auto it = open.begin(); it != open.end();) {
        if (Covers(mask, it->mask)) {
            ++strict;
            it = open.erase(it);
        } else {
            ++it;
        }
    }
}

void BarrierLedger::NoteChain(VkAccessFlags coveredAccess) {
    for (auto it = open.begin(); it != open.end();) {
        const bool stages = (it->mask.dstStage & VK_PIPELINE_STAGE_HOST_BIT) == 0;
        if (stages && AccessCovers(coveredAccess, it->mask.dstAccess)) {
            ++chain;
            it = open.erase(it);
        } else {
            ++it;
        }
    }
}

std::uint32_t BarrierLedger::Settle(std::string* firstViolation) {
    const auto violations = static_cast<std::uint32_t>(open.size());
    if (violations != 0 && firstViolation != nullptr) {
        const auto& entry = open.front();
        char text[192];
        std::snprintf(text, sizeof(text), "%s: src 0x%x/0x%x dst 0x%x/0x%x", entry.site, entry.mask.srcStage, entry.mask.srcAccess, entry.mask.dstStage, entry.mask.dstAccess);
        *firstViolation = text;
    }
    open.clear();
    return violations;
}

}
