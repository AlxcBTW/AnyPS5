#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace AgcDriver::Graphics {

class Texture;
class StorageTexture;

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target);
bool SampleLocationsCompatibleDepth(const Context& context, VkFormat format, std::uint32_t samples);
std::uint64_t DepthSurfacesSerial();
std::uint64_t DepthSliceBytes(VkExtent2D extent, std::uint32_t bytesPerTexel);
std::uint64_t MultisampledDepthBytes_nid_no_patch(VkExtent2D extent, std::uint32_t bytesPerTexel, std::uint32_t samples);
void RequireNoResidentDepthRead_nid_no_patch(std::uint64_t address, std::size_t bytes);
bool HasResidentDepthGuards_nid_no_patch();
void RetireUnmappedDepth_nid_no_patch(std::uintptr_t address, std::size_t bytes);
void RequireNoNativeDepthFeedback_nid_no_patch(const Context& context, const DepthTarget& target, std::span<const std::shared_ptr<Texture>> sampled);
void ClearDepthSurfaces(VkDevice device);
void RetireDepthSurfaces(VkDevice device, std::uint64_t address, std::uint64_t bytes);
bool DepthSurfaceAt(std::uint64_t address);
bool DepthSurfaceOverlaps(VkDevice device, std::uint64_t address, std::uint64_t bytes);
bool DepthSurfaceAt(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource);
std::uint64_t HtileDepthClearAddress(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const std::array<std::uint32_t, 3>& numThreads);
std::size_t HtileDepthClearBytes_nid_no_patch(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, std::span<const std::uint32_t> packet, std::uint64_t indirectArguments);
void NoteHtileDepthClear(std::uint64_t htileAddress, std::size_t bytes, std::size_t nativeBytes = 0u);
struct MemoryValueFill {
    std::uint64_t address;
    std::size_t bytes;
    std::uint64_t valueAddress;
};
std::optional<MemoryValueFill> MatchMemoryValueFill(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const std::array<std::uint32_t, 3>& numThreads, const std::array<std::uint32_t, 3>& groups);
void NoteMemoryValueFill(const MemoryValueFill& fill);
bool DepthSurfaceHolds(const Context& context, const GuestTextureResource& resource, const Texture* texture);
void NoteDepthMetadataFill(std::uint64_t address, std::size_t bytes, std::uint32_t pattern);
VkImageAspectFlags HtileFillClears(std::uint32_t pattern, bool stencilInHtile);
std::uint64_t HtileMetadataBytes(VkExtent2D extent);
bool HtileFillCovers(std::uint64_t htile, VkExtent2D extent, std::uint64_t address, std::size_t bytes);
std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);
VkFormat HostDepthFormat(const Context& context, VkFormat format);
float HostDepthBiasConstant(const Context& context, VkFormat format, float constant);
std::shared_ptr<StorageTexture> DepthSurfaceStorage(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, std::uint32_t mip);
bool DepthSurfaceStoreBack(const Context& context, const StorageTexture& storage);

// Lab switch APS5_LAB_DEPTH_CLEAR_HLE=1 (APS5_LAB_DEPTH_CLEAR_VERIFY=1 adds a read-back check of every sampled
// clear). Games clear a depth plane (a shadow atlas) with a compute kernel that stores one colour to every texel of
// a storage image over the plane. The plane is a depth image, the kernel's image a colour mirror of it, so each such
// dispatch copies the whole plane depth -> staging buffer -> mirror before it and mirror -> staging buffer -> depth
// after it (2 x 64 MiB for a 16384 x 2048 D16 atlas) although the kernel reads nothing and writes every texel. With the
// switch the dispatch still runs over the mirror, but its copy-in is left out and its copy-out becomes a clear of the
// depth image to the value the kernel stores (the colour is read from guest memory, where the kernel reads it).
struct StorageImageClear {
    std::array<std::uint32_t, 8> image{};
    std::array<std::uint32_t, 4> color{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
std::optional<StorageImageClear> MatchStorageImageClear(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, const std::array<std::uint32_t, 3>& groups);
std::uint64_t StorageImageClearAddress(const StorageImageClear& clear);
bool DepthPlaneClearEnabled();
struct DepthPlaneOverwrite {
    std::array<std::uint32_t, 8> image{};
    GuestTextureResource view{};
    std::uint32_t bits = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t raw = 0;
    float depth = 0.0f;
    // The mirror whose copy-in was left out for this dispatch (set by DepthSurface::Storage, consumed by its StoreBack).
    const void* mirror = nullptr;
};
// While alive, the depth storage bindings of the calling thread's dispatch that use the clear's image take the
// clear above. The caller holds the GPU mutex with the packet's labels recorded (the colour is read from guest
// memory). Active() says whether the clear was taken; when not, the dispatch runs as before.
class DepthPlaneClear {
public:
    explicit DepthPlaneClear(const StorageImageClear& clear);
    ~DepthPlaneClear();
    DepthPlaneClear(const DepthPlaneClear&) = delete;
    DepthPlaneClear& operator=(const DepthPlaneClear&) = delete;
    bool Active() const { return installed; }

private:
    DepthPlaneOverwrite value;
    DepthPlaneOverwrite* previous = nullptr;
    bool installed = false;
};
struct DepthClearStatistics {
    std::uint64_t matched = 0;
    std::uint64_t taken = 0;
    std::uint64_t copiesSkipped = 0;
    std::uint64_t clears = 0;
    std::uint64_t bytesSkipped = 0;
    std::uint64_t verified = 0;
    std::uint64_t violations = 0;
    std::array<std::uint64_t, 8> declined{};
};
DepthClearStatistics DepthClearCounts();
// Tests: -1 lets the environment decide, 0 off, 1 the lever, 2 the lever with the read-back check; `perturb` is
// xored into the value the check expects (a check that cannot fail proves nothing).
void SetDepthClearModeForTest(int mode, std::uint32_t perturb = 0);

}

#endif
