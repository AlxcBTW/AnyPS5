#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BARRIERBATCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BARRIERBATCH_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::Graphics {

struct BarrierMask {
    VkPipelineStageFlags srcStage = 0;
    VkPipelineStageFlags dstStage = 0;
    VkAccessFlags srcAccess = 0;
    VkAccessFlags dstAccess = 0;
    bool operator==(const BarrierMask&) const = default;
};

bool Covers(const BarrierMask& outer, const BarrierMask& inner);
BarrierMask Merge(const BarrierMask& first, const BarrierMask& second);
bool CanMergeExactly(const BarrierMask& first, const BarrierMask& second);
std::vector<BarrierMask> Coalesce(std::span<const BarrierMask> run);

struct StoreSpan {
    std::uint64_t buffer = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

struct StoreWaves {
    std::vector<std::uint32_t> order;
    std::vector<std::uint32_t> waveEnds;
};
StoreWaves PlanStoreWaves(std::span<const StoreSpan> stores);
std::uint32_t CountSequentialBarriers(std::span<const StoreSpan> stores);
std::uint32_t VerifyStoreWaves(std::span<const StoreSpan> stores, const StoreWaves& plan, std::string* first = nullptr);

class BarrierLedger {
public:
    void NoteOld(const char* site, const BarrierMask& mask);
    void NoteEmitted(const BarrierMask& mask);
    void NoteChain(VkAccessFlags coveredAccess);
    std::uint32_t Settle(std::string* first = nullptr);
    std::uint64_t Strict() const { return strict; }
    std::uint64_t Chain() const { return chain; }

private:
    struct Entry {
        const char* site;
        BarrierMask mask;
    };
    std::vector<Entry> open;
    std::uint64_t strict = 0, chain = 0;
};

}

#endif
