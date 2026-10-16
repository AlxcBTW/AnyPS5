#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;
using AgcDriver::Graphics::Require;

alignas(256) constexpr std::array<std::uint32_t, 6> Skipped{0xd7460000u, 0x04010c04u, 0x7e020281u, 0xe0102000u, 0x80000100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 6> Code{0xd7460000u, 0x04010c04u, 0x7e020280u, 0xe0102000u, 0x80000100u, 0xbf810000u};
constexpr std::uint32_t Untouched = 0xcafebabeu;
alignas(256) std::array<std::uint32_t, 64> Output{};
alignas(256) std::array<std::uint32_t, 64> Scratch{};
alignas(256) std::array<std::uint32_t, 4> Arguments{};

void Registers(std::vector<std::uint32_t>& words, std::uint32_t first, std::span<const std::uint32_t> values) {
    words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
    words.push_back(first);
    words.insert(words.end(), values.begin(), values.end());
}

void Setup(std::vector<std::uint32_t>& words, const void* code, std::array<std::uint32_t, 64>& output) {
    const std::array<std::uint32_t, 3> shape{64, 1, 1};
    Registers(words, 0x207, shape);
    const auto address = reinterpret_cast<std::uintptr_t>(code);
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    Registers(words, 0x20c, program);
    const std::array<std::uint32_t, 1> resource{(4u << 1u) | (1u << 7u)};
    Registers(words, 0x213, resource);
    const auto pointer = reinterpret_cast<std::uintptr_t>(output.data());
    const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(pointer), static_cast<std::uint32_t>((pointer >> 32u) & 0xffffu) | (4u << 16u), static_cast<std::uint32_t>(output.size()), 0x00014204u};
    Registers(words, 0x240, descriptor);
}

void SubmitBehindCommit(std::vector<std::uint32_t>& words) {
    std::unique_lock gpuLock(AgcDriver::GuestMemory::GpuMutex());
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    const auto start = std::chrono::steady_clock::now();
    while (DrawPipeline::Queue0().Outstanding() == 0) {
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(20), "the WRITE_DATA to the indirect arguments never reached the draw pipeline");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

void Run() {
    Output.fill(Untouched);
    Scratch.fill(Untouched);
    Arguments = {0, 1, 1, 0};
    std::vector<std::uint32_t> words;
    Setup(words, Skipped.data(), Scratch);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x41u});
    Setup(words, Code.data(), Output);
    const auto arguments = reinterpret_cast<std::uintptr_t>(Arguments.data());
    words.insert(words.end(), {0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(arguments), static_cast<std::uint32_t>(arguments >> 32u), 64u});
    words.insert(words.end(), {0xc0021600u, static_cast<std::uint32_t>(arguments), static_cast<std::uint32_t>(arguments >> 32u), 0x41u | 0x20u});
    SubmitBehindCommit(words);
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output.data()), sizeof(Output));
    Require(Arguments[0] == 64, "the WRITE_DATA to the indirect arguments did not land");
    for (std::size_t i = 0; i < Output.size(); ++i) {
        Require(Output[i] == 0, "an indirect dispatch read its arguments on the CPU before the queued WRITE_DATA that writes them had landed (word " + std::to_string(i) + ")");
    }
}

}

int main() {
    char skipped[32];
    std::snprintf(skipped, sizeof(skipped), "%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(Skipped.data())));
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
    _putenv_s("APS5_SKIP_SHADER", skipped);
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
    setenv("APS5_SKIP_SHADER", skipped, 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        for (int attempt = 0; attempt < 3; ++attempt) Run();
        AgcDriverShutdown_nid_postfix();
        std::puts("pipelined indirect dispatch arguments tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
