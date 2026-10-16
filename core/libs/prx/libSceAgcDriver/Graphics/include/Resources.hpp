#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESOURCES_HPP

#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver::Graphics {

class Buffer {
public:
    Buffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    // A byte range of `parent` (APS5_LAB_SNAPSHOT_RING): Handle() is the parent's VkBuffer, so a
    // descriptor must use {Handle(), ViewOffset(), Bytes().size()}; the parent lives as long as the slice.
    Buffer(std::shared_ptr<Buffer> parent, std::size_t viewOffset, std::size_t size);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    VkBuffer Handle() const;
    VkDeviceSize ViewOffset() const { return viewOffset; }
    bool IsSlice() const { return parent != nullptr; }
    VkDeviceAddress DeviceAddress() const;
    // Host-visible buffers only: a device-local one (properties without HOST_VISIBLE, the staging
    // shadows of GuestBufferMemory) has no mapping and its bytes move by GPU copies alone.
    std::span<std::byte> Bytes();
    bool Mapped() const { return mapping != nullptr; }
    void Invalidate();

private:
    void initializeAddress(VkBufferUsageFlags usage);
    void release() noexcept;
    Context context;
    VkDeviceAddress deviceAddress = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapping = nullptr;
    std::size_t size;
    std::size_t capacity;
    // Fully made (or taken from the pool), so release returns it to the pool instead of destroying
    // the handles a failed construction left.
    bool ready = false;
    VkDeviceSize allocationBytes = 0;
    VkDeviceSize offset = 0;
    bool slab = false;
    VkBufferUsageFlags usage;
    VkMemoryPropertyFlags properties;
    std::shared_ptr<BufferPool> cache;
    std::shared_ptr<Buffer> parent;
    VkDeviceSize viewOffset = 0;
};

// Number of exact-size VkBuffers bound over pooled memory since start (BindExactBuffer, all callers).
std::uint64_t ExactBufferRebinds();

// Device-local scratch memory for GPU-side layout conversion. The detiler reads and writes scattered
// elements, which crawls across PCIe, so guest bytes move between host and device buffers with DMA
// copies and are only swizzled in video memory.
class DeviceBuffer {
public:
    DeviceBuffer(const Context& context, std::size_t size, VkBufferUsageFlags usage);
    ~DeviceBuffer();
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    VkBuffer Handle() const;
    std::size_t Size() const;

private:
    void release() noexcept;
    Context context;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::size_t size;
    std::size_t capacity;
    VkDeviceSize allocationBytes = 0;
    VkBufferUsageFlags usage;
    std::shared_ptr<BufferPool> cache;
    bool hostPlaced = false;
};

// Records a whole-range buffer copy.
void CopyBuffer(const Context& context, VkCommandBuffer commands, VkBuffer source, VkDeviceSize sourceOffset, VkBuffer destination, VkDeviceSize destinationOffset, VkDeviceSize bytes);

// Records a global memory barrier.
void RecordMemoryBarrier(const Context& context, VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess);

// Resolves every DeviceFunctions entry point of the context's device (once, at device setup).
void FillDeviceFunctions(const Context& context, DeviceFunctions& functions);

class RenderTarget {
public:
    RenderTarget(const Context& context, const ColorTarget& target, bool blending);
    ~RenderTarget();
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    VkImage Image() const;
    VkImageView View() const;

private:
    void release() noexcept;
    Context context;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

class CommandBatch {
public:
    explicit CommandBatch(const Context& context);
    ~CommandBatch();
    CommandBatch(const CommandBatch&) = delete;
    CommandBatch& operator=(const CommandBatch&) = delete;
    VkCommandBuffer Handle() const;
    void SubmitAndWait();
    void Submit();
    void Wait();

private:
    void release() noexcept;
    Context context;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    bool submitted = false;
};

}

#endif
