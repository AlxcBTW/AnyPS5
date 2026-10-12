#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <span>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
alignas(256) std::array<std::uint32_t, Threads> Counted{};
alignas(256) std::array<std::uint32_t, Threads> Other{};
alignas(256) std::array<std::uint32_t, Threads> Slow{};

alignas(256) constexpr std::array<std::uint32_t, 8> IncrementCode{0x34020082u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0x4a040481u, 0xe0701000u, 0x80000201u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 6> OtherCode{0x34020082u, 0x4a0400ffu, 0x0000a000u, 0xe0701000u, 0x80000201u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 6> SlowCode{0x34020082u, 0x4a0400ffu, 0x0000b000u, 0xe0701000u, 0x80000201u, 0xbf810000u};

std::atomic<std::uint32_t> hookCalls{0};
std::atomic<bool> creating{false};
std::atomic<bool> created{false};
std::atomic<int> hookDelayMs{0};

void SlowCreate() {
    hookCalls.fetch_add(1);
    creating.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(hookDelayMs.load()));
    created.store(true);
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::uint32_t* output) {
    const auto address = reinterpret_cast<std::uintptr_t>(output);
    const std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), Threads * 4u, 0x31016facu};
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

void DispatchLocked(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& shader, std::span<const std::uint32_t> code, std::shared_ptr<AgcDriver::PreparedDispatch> prepared) {
    std::lock_guard gpuLock(AgcDriver::GuestMemory::GpuMutex());
    device.Dispatch(shader, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()), std::move(prepared));
}

void CheckSharedCreation(AgcDriver::VulkanDevice& device) {
    Counted.fill(0u);
    const auto shader = Compile(device, IncrementCode, Counted.data());
    hookCalls.store(0);
    hookDelayMs.store(300);
    AgcDriver::VulkanDevice::SetComputePipelineCreateHookForTesting(&SlowCreate);
    const auto before = AgcDriver::VulkanDevice::ComputePipelinesCreated();
    std::atomic<std::uint32_t> waits{0};
    std::atomic<std::uint32_t> ready{0};
    const auto worker = [&] {
        ready.fetch_add(1);
        while (ready.load() < 2u) std::this_thread::yield();
        auto prepared = device.PrepareDispatch(shader, {}, 0, 0, [&] { waits.fetch_add(1); });
        DispatchLocked(device, shader, IncrementCode, std::move(prepared));
    };
    std::thread first(worker);
    std::thread second(worker);
    first.join();
    second.join();
    AgcDriver::VulkanDevice::SetComputePipelineCreateHookForTesting(nullptr);
    device.WaitIdle();
    const auto creations = AgcDriver::VulkanDevice::ComputePipelinesCreated() - before;
    Require(creations == 1u && hookCalls.load() == 1u, "two dispatches of one uncompiled variant created its pipeline " + std::to_string(creations) + " times");
    Require(waits.load() == 2u, "a dispatch waited for the pipeline without flushing first (" + std::to_string(waits.load()) + " of 2 flushed)");
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        Require(Counted[lane] == 2u, "lane " + std::to_string(lane) + " counted " + std::to_string(Counted[lane]) + " of 2 dispatches");
    }
}

void CheckLockFreeDuringCreate(AgcDriver::VulkanDevice& device) {
    Other.fill(0u);
    Slow.fill(0u);
    const auto other = Compile(device, OtherCode, Other.data());
    DispatchLocked(device, other, OtherCode, nullptr);
    device.WaitIdle();
    Other.fill(0u);
    const auto slow = Compile(device, SlowCode, Slow.data());
    creating.store(false);
    created.store(false);
    hookDelayMs.store(1500);
    AgcDriver::VulkanDevice::SetComputePipelineCreateHookForTesting(&SlowCreate);
    std::thread worker([&] {
        auto prepared = device.PrepareDispatch(slow, {}, 0, 0, {});
        DispatchLocked(device, slow, SlowCode, std::move(prepared));
    });
    while (!creating.load()) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    {
        std::lock_guard gpuLock(AgcDriver::GuestMemory::GpuMutex());
        device.Dispatch(other, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(OtherCode.data()));
        device.SubmitRecorded();
    }
    const auto lockedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const bool overlapped = !created.load();
    worker.join();
    AgcDriver::VulkanDevice::SetComputePipelineCreateHookForTesting(nullptr);
    device.WaitIdle();
    Require(overlapped, "another dispatch had to wait for a pipeline creation to finish (" + std::to_string(lockedMs) + " ms)");
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        Require(Other[lane] == lane + 0xa000u && Slow[lane] == lane + 0xb000u, "lane " + std::to_string(lane) + " wrote " + std::to_string(Other[lane]) + " and " + std::to_string(Slow[lane]));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        CheckSharedCreation(*device);
        CheckLockFreeDuringCreate(*device);
        std::puts("compute pipeline outside lock tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
