#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Side = 128;
constexpr std::size_t SurfaceBytes = 65536;
constexpr std::size_t KeyBytes = SurfaceBytes / 256;
constexpr VkExtent2D DepthExtent{1024, 512};
constexpr std::size_t TexelsOffset = 0;
constexpr std::size_t KeysOffset = SurfaceBytes;
constexpr std::size_t SourceKeysOffset = SurfaceBytes + 4096;
constexpr std::size_t HtileOffset = 2 * 65536;
constexpr std::size_t IndexOffset = 3 * 65536;
constexpr std::size_t IndexBytes = 4096;
constexpr std::size_t BlockBytes = 4 * 65536;

alignas(256) constexpr std::array<std::uint32_t, 7> CopyKeysCode{
    0x34020083, 0xe0341000, 0x80000201, 0xbf8c3f70, 0xe0741000, 0x80010201, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(65536, BlockBytes));
#endif
        Require(block != nullptr, "draw pre-sync: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

AgcDriver::Graphics::State ColorState(std::uint8_t* block) {
    AgcDriver::Graphics::ColorTarget color{};
    color.address = AddressOf(block + TexelsOffset);
    color.extent = {Side, Side};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.bytes = SurfaceBytes;
    color.tileMode = AgcDriver::Graphics::ColorTileMode::RenderTarget;
    color.elementBytes = 4;
    color.dccAddress = AddressOf(block + KeysOffset);
    AgcDriver::Graphics::State state{};
    state.color = color;
    state.colors = {color};
    state.hasColorTarget = true;
    return state;
}

AgcDriver::Graphics::State DepthState(std::uint8_t* block) {
    AgcDriver::Graphics::DepthTarget depth{};
    depth.address = AddressOf(block + IndexOffset);
    depth.extent = DepthExtent;
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.htileAddress = AddressOf(block + HtileOffset);
    depth.samples = 1;
    AgcDriver::Graphics::State state{};
    state.depth = depth;
    return state;
}

void RequireNothingPending(std::uint64_t address, std::size_t bytes, const char* what) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = AgcDriver::Graphics::Recorder::Active();
    Require(recorder == nullptr || !recorder->PendingWriteOverlaps(address, bytes), what);
}

void RequireWords(const std::uint8_t* data, std::size_t bytes, std::uint32_t expected, const char* what) {
    for (std::size_t offset = 0; offset + 4 <= bytes; offset += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, data + offset, sizeof(word));
        if (word == expected) continue;
        char message[256];
        std::snprintf(message, sizeof(message), "%s: word at +0x%zx is 0x%08x, expected 0x%08x", what, offset, word, expected);
        Require(false, message);
    }
}

void RunNoWriter(AgcDriver::VulkanDevice& device, std::uint8_t* block) {
    const auto before = AgcDriver::VulkanDevice::DrawPresyncCounts();
    Require(device.PresyncDraw(DepthState(block), {}) == 0, "the pre-sync of a draw waited although no recorded work writes its HTILE");
    Require(device.PresyncDraw(ColorState(block), {}) == 0, "the pre-sync of a draw waited although no recorded work writes its DCC keys");
    const auto after = AgcDriver::VulkanDevice::DrawPresyncCounts();
    Require(after.checked == before.checked + 2, "the pre-sync did not count the two draws it checked");
    Require(after.surfaces == before.surfaces + 2, "the pre-sync did not list the HTILE of the depth target and the DCC keys of the color target");
    Require(after.overlapping == before.overlapping && after.presyncs == before.presyncs, "the pre-sync found a recorded writer on an idle device");
}

void RunHtileFill(AgcDriver::VulkanDevice& device, std::uint8_t* block) {
    auto* htile = block + HtileOffset;
    const auto bytes = static_cast<std::size_t>(AgcDriver::Graphics::HtileMetadataBytes(DepthExtent));
    Require(HtileOffset + bytes <= IndexOffset, "the HTILE of the test depth target overlaps the index range");
    std::memset(htile, 0, bytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const std::array<std::uint32_t, 4> pattern{0xfffc000fu, 0xfffc000fu, 0xfffc000fu, 0xfffc000fu};
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(device.FillBuffer(AddressOf(htile), bytes, pattern), "the fill of the HTILE was not recorded");
        Require(AgcDriver::Graphics::Recorder::SnapshotWriteOverlaps(AddressOf(htile), bytes), "the recorded fill of the HTILE is not in the pending-write snapshot");
    }
    const auto before = AgcDriver::VulkanDevice::DrawPresyncCounts();
    const auto serial = device.PresyncDraw(DepthState(block), {});
    const auto after = AgcDriver::VulkanDevice::DrawPresyncCounts();
    Require(serial != 0, "the pre-sync did not wait for the recorded fill of the draw's HTILE");
    Require(after.htile == before.htile + 1 && after.overlapping == before.overlapping + 1 && after.presyncs == before.presyncs + 1, "the pre-sync did not count the HTILE writer it waited for");
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        device.ReapRecorded(serial);
    }
    RequireNothingPending(AddressOf(htile), bytes, "the HTILE fill is still pending after the pre-sync and its reap");
    RequireWords(htile, bytes, pattern[0], "the HTILE after the pre-synced fill");
}

void RunIndexFill(AgcDriver::VulkanDevice& device, std::uint8_t* block) {
    auto* indices = block + IndexOffset;
    std::memset(indices, 0, IndexBytes);
    const std::array<std::uint32_t, 4> pattern{0x00020001u, 0x00020001u, 0x00020001u, 0x00020001u};
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(device.FillBuffer(AddressOf(indices), IndexBytes, pattern), "the fill of the index buffer was not recorded");
    }
    const AgcDriver::Pm4::DrawParameters draw{AddressOf(indices), static_cast<std::uint32_t>(IndexBytes / 2), 2, 1, 0};
    const auto before = AgcDriver::VulkanDevice::DrawPresyncCounts();
    const auto serial = device.PresyncDraw(AgcDriver::Graphics::State{}, draw);
    const auto after = AgcDriver::VulkanDevice::DrawPresyncCounts();
    Require(serial != 0, "the pre-sync did not wait for the recorded fill of the draw's index buffer");
    Require(after.index == before.index + 1 && after.presyncs == before.presyncs + 1, "the pre-sync did not count the index writer it waited for");
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        device.ReapRecorded(serial);
    }
    RequireNothingPending(AddressOf(indices), IndexBytes, "the index fill is still pending after the pre-sync and its reap");
    RequireWords(indices, IndexBytes, pattern[0], "the index buffer after the pre-synced fill");
}

void RunKeyFill(AgcDriver::VulkanDevice& device, std::uint8_t* block) {
    auto* keys = block + KeysOffset;
    std::memset(keys, 0xff, KeyBytes);
    const std::array<std::uint32_t, 4> pattern{0u, 0u, 0u, 0u};
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(device.FillBuffer(AddressOf(keys), KeyBytes, pattern), "the fill of the keys was not recorded");
        Require(AgcDriver::Graphics::DccKeyWritersPending(AddressOf(keys), KeyBytes), "a recorded fill of the keys is not seen as a pending key writer");
        Require(!AgcDriver::Graphics::DccKeyWriterNeedsSync(AddressOf(keys), KeyBytes), "the keys of a recorded constant fill need a synchronization");
    }
    const auto before = AgcDriver::VulkanDevice::DrawPresyncCounts();
    const auto serial = device.PresyncDraw(ColorState(block), {});
    const auto after = AgcDriver::VulkanDevice::DrawPresyncCounts();
    Require(after.keys == before.keys + 1, "the pre-sync did not see the pending fill of the color target's keys");
    Require(serial == 0 && after.presyncs == before.presyncs, "the pre-sync waited for a fill whose keys the locked lookup derives from the pending stores");
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    device.WaitIdle();
}

void RunKeyKernel(AgcDriver::VulkanDevice& device, std::uint8_t* block) {
    auto* keys = block + KeysOffset;
    auto* source = block + SourceKeysOffset;
    std::memset(keys, 0xff, KeyBytes);
    std::memset(source, 0x00, KeyBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::vector<std::uint32_t> copyData(16, 0u);
    const auto from = BufferDescriptor(source, KeyBytes);
    const auto to = BufferDescriptor(keys, KeyBytes);
    std::copy(from.begin(), from.end(), copyData.begin());
    std::copy(to.begin(), to.end(), copyData.begin() + 4);
    const auto copy = Compile(device, CopyKeysCode, copyData);
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        device.Dispatch(copy, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(CopyKeysCode.data()));
        Require(AgcDriver::Graphics::DccKeyWritersPending(AddressOf(keys), KeyBytes), "a recorded kernel that copies data over the keys is not seen as a pending key writer");
    }
    const auto before = AgcDriver::VulkanDevice::DrawPresyncCounts();
    const auto serial = device.PresyncDraw(ColorState(block), {});
    const auto after = AgcDriver::VulkanDevice::DrawPresyncCounts();
    Require(serial != 0, "the pre-sync did not wait for the kernel that copies data over the color target's keys");
    Require(after.keys == before.keys + 1 && after.presyncs == before.presyncs + 1, "the pre-sync did not count the key writer it waited for");
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        device.ReapRecorded(serial);
    }
    RequireNothingPending(AddressOf(keys), KeyBytes, "the key copy is still pending after the pre-sync and its reap");
    RequireWords(keys, KeyBytes, 0u, "the keys after the pre-synced copy");
}

void RunUnderTheMutex(AgcDriver::VulkanDevice& device, std::uint8_t* block) {
    const auto before = AgcDriver::VulkanDevice::DrawPresyncCounts();
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(device.PresyncDraw(DepthState(block), {}) == 0, "the pre-sync ran under the GPU mutex");
    }
    Require(AgcDriver::VulkanDevice::DrawPresyncCounts().checked == before.checked, "the pre-sync checked a draw under the GPU mutex");
}

void EnableSwitch() {
#ifdef _WIN32
    _putenv_s("APS5_LAB_DRAW_PRESYNC", "1");
#else
    setenv("APS5_LAB_DRAW_PRESYNC", "1", 1);
#endif
}

}

int main() {
    try {
        EnableSwitch();
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!device->CanWaitUnlocked()) {
            std::puts("skipped, the device has no timeline semaphores: the pre-sync is not tested");
            return VulkanTestSkipped;
        }
        Require(AgcDriver::VulkanDevice::DrawPresync(), "APS5_LAB_DRAW_PRESYNC=1 does not enable the draw pre-sync");
        GuestBlock guest;
        auto* block = guest.Data();
        {
            const std::array<std::uint32_t, 4> pattern{0u, 0u, 0u, 0u};
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            const bool filled = device->FillBuffer(AddressOf(block + IndexOffset), IndexBytes, pattern);
            device->WaitIdle();
            if (!filled) {
                std::puts("skipped, host imports are unavailable: the writers of the test are not recorded");
                return VulkanTestSkipped;
            }
        }
        RunNoWriter(*device, block);
        RunHtileFill(*device, block);
        RunIndexFill(*device, block);
        RunKeyFill(*device, block);
        RunKeyKernel(*device, block);
        RunUnderTheMutex(*device, block);
        std::puts("draw pre-sync tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
