#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 256;
constexpr std::uint32_t SlotBytes = Threads * 4u;
constexpr std::uint32_t Slots = 8;
constexpr std::size_t BlockBytes = 65536;
alignas(256) constexpr std::array<std::uint32_t, 9> CopyCode{0xd7460000u, 0x04010c08u, 0xe0302000u, 0x80000100u, 0xbf8c3f70u, 0xe0702000u, 0x80010100u, 0xbf810000u, 0xbf9f0000u};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "address-free keys: cannot allocate a guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        try {
            GuestAllocations::Mutation().Remove(block);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "address-free keys: the guest block stays registered: %s\n", error.what());
            return;
        }
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Words(std::size_t byteOffset) const { return reinterpret_cast<std::uint32_t*>(block + byteOffset); }
    std::uint64_t Address(std::size_t byteOffset) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block + byteOffset)); }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 4> Descriptor(std::uint64_t address) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), Threads, 0x31016facu};
}

class Copies {
public:
    explicit Copies(AgcDriver::VulkanDevice& device) : device(device) {}

    std::shared_ptr<AgcDriver::Graphics::ShaderResources> Run(std::uint64_t input, std::uint64_t output, bool prepare) {
        std::vector<std::uint32_t> userData;
        for (const auto word : Descriptor(input)) userData.push_back(word);
        for (const auto word : Descriptor(output)) userData.push_back(word);
        const std::span<const std::uint32_t> code(CopyCode);
        const auto codeAddress = reinterpret_cast<std::uintptr_t>(code.data());
        const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{codeAddress, std::as_bytes(code)}}};
        const ShaderRecompiler::ShaderComputeStageInfo compute{{64, 1, 1}, 0, {true, false, false}, false, 1, {}};
        ShaderRecompiler::RecompileRequest request{
            {ShaderStage::Compute, codeAddress, code, 0, {}},
            {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
            device.Target(),
            {0, 0, 0, 128}
        };
        results.push_back(std::make_unique<ShaderRecompiler::RecompileResult>(ShaderRecompiler::Recompile(request)));
        const auto& result = *results.back();
        auto prepared = prepare ? device.PrepareDispatch(result, {}, Threads, codeAddress) : nullptr;
        device.Dispatch(result, Threads / 64u, 1, 1, {}, codeAddress, std::move(prepared));
        device.WaitIdle();
        const AgcDriver::Graphics::CompiledShader shader{ShaderStage::Compute, &result, 0};
        auto key = AgcDriver::Graphics::ShaderResources::ContentKey(shader, false, false, AddressFree());
        const auto handle = reinterpret_cast<std::uint64_t>(device.Device());
        key.push_back(static_cast<std::uint32_t>(handle));
        key.push_back(static_cast<std::uint32_t>(handle >> 32u));
        return AgcDriver::Graphics::SharedResourceCache().Find(key);
    }

    static bool AddressFree() {
        return std::getenv("APS5_NO_ADDR_FREE_KEYS") == nullptr;
    }

private:
    AgcDriver::VulkanDevice& device;
    std::vector<std::unique_ptr<ShaderRecompiler::RecompileResult>> results;
};

void Expect(const std::uint32_t* output, const std::uint32_t* input, const std::string& what) {
    for (std::uint32_t i = 0; i < Threads; ++i) {
        Require(output[i] == input[i], what + ": word " + std::to_string(i) + " is " + std::to_string(output[i]) + ", expected " + std::to_string(input[i]));
    }
}

void Fill(std::uint32_t* words, std::uint32_t seed) {
    for (std::uint32_t i = 0; i < Threads; ++i) words[i] = seed * 0x10001u + i * 7u + 1u;
}

struct CachedTemplatesRelease {
    AgcDriver::VulkanDevice& device;
    ~CachedTemplatesRelease() {
        device.WaitIdle();
        AgcDriver::Graphics::SharedResourceCache().Clear();
    }
};

int ExecuteTests(AgcDriver::VulkanDevice& device) {
    const GuestBlock ring;
    const GuestBlock outputs;
    const CachedTemplatesRelease release{device};
    for (std::uint32_t slot = 0; slot < Slots; ++slot) Fill(ring.Words(slot * SlotBytes), slot);
    Copies copies(device);
    const auto clear = [&] { std::memset(outputs.Words(0), 0xcd, SlotBytes); };
    clear();
    const auto first = copies.Run(ring.Address(0), outputs.Address(0), false);
    Expect(outputs.Words(0), ring.Words(0), "the copy from slot 0");
    if (first == nullptr) {
        std::puts("skipped, host imports are unavailable: the first copy built no reusable template");
        return VulkanTestSkipped;
    }
    const bool addressFree = Copies::AddressFree();
    std::vector<std::shared_ptr<AgcDriver::Graphics::ShaderResources>> served;
    for (std::uint32_t round = 0; round < 2; ++round) {
        for (std::uint32_t slot = 1; slot < Slots; ++slot) {
            clear();
            const auto object = copies.Run(ring.Address(slot * SlotBytes), outputs.Address(0), (slot + round) % 2 == 0);
            const auto what = "the copy from slot " + std::to_string(slot) + " in round " + std::to_string(round);
            Expect(outputs.Words(0), ring.Words(slot * SlotBytes), what);
            Require(object != nullptr, what + " left no template");
            if (addressFree) Require(object == first, what + " did not reuse the template of slot 0");
            else Require(object != first, what + " reused the template of slot 0 without address-free keys");
            served.push_back(object);
        }
    }
    Fill(ring.Words(3 * SlotBytes), 0x55u);
    clear();
    copies.Run(ring.Address(3 * SlotBytes), outputs.Address(0), false);
    Expect(outputs.Words(0), ring.Words(3 * SlotBytes), "the copy from slot 3 after the CPU changed it");
    clear();
    copies.Run(ring.Address(0), outputs.Address(0), true);
    Expect(outputs.Words(0), ring.Words(0), "the copy from slot 0 again");
    const auto misaligned = 2 * SlotBytes + 4u;
    clear();
    copies.Run(ring.Address(misaligned), outputs.Address(0), false);
    Expect(outputs.Words(0), ring.Words(misaligned), "a copy from a buffer at another offset alignment");
    std::memset(outputs.Words(SlotBytes), 0xcd, SlotBytes);
    copies.Run(ring.Address(5 * SlotBytes), outputs.Address(SlotBytes), false);
    Expect(outputs.Words(SlotBytes), ring.Words(5 * SlotBytes), "a copy into a written buffer that moved");
    return 0;
}

}

int main() {
#ifdef _WIN32
    _putenv("APS5_PIN_WAIT_MS=5000");
#else
    setenv("APS5_PIN_WAIT_MS", "5000", 1);
#endif
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto result = ExecuteTests(*device);
        if (result == 0) std::puts("dispatch address-free key tests passed");
        return result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
