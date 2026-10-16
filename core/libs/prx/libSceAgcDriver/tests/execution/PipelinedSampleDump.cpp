#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// APS5_LAB_PIPE_DUMPS: the occlusion sample dumps (EVENT_WRITE, event 0x39) are queued on the draw committer in stream order. The test runs
// one compute dispatch first (it opens the device, so the recorder exists and the dump area is host-imported where the device allows it),
// then a stream of labels, dumps, memory copies out of a dump's range, and a predicate that is a dump's block:
//   - every dump is queued (the counter), none drains the worker;
//   - each dump writes the ready bit into the first 8 bytes of its 16 blocks and nothing between them;
//   - a COPY_DATA that reads a dump's range sees the dump (the copy drains the pipeline through the registered range);
//   - a predicated WRITE_DATA whose predicate is a dump block sees the dump (SET_PREDICATION keeps the pipeline, the predicated packet drains);
//   - the labels between the dumps land.

namespace {

void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("pipelined sample dump: " + what);
}

constexpr std::uint64_t Ready = 1ull << 63u;
constexpr std::uint64_t Sentinel = 0x1111111111111111ull;
alignas(256) constexpr std::array<std::uint32_t, 6> ZeroFill{0xd7460000u, 0x04010c04u, 0x7e020280u, 0xe0102000u, 0x80000100u, 0xbf810000u};
// Dump areas of 256 bytes: A at words 0..31 (begin dump at +0, end dump at +8), B at 32..63, copies at 64 and 65.
alignas(4096) std::array<std::uint64_t, 1024> Area{};
alignas(16) std::array<std::uint32_t, 8> Labels{};

std::vector<std::uint32_t> ZeroFillCommands() {
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const auto output = reinterpret_cast<std::uintptr_t>(Area.data());
    const std::vector<std::uint32_t> data{static_cast<std::uint32_t>(output), static_cast<std::uint32_t>((output >> 32u) & 0xffffu) | (4u << 16u), static_cast<std::uint32_t>(sizeof(Area) / 4), 0x00014204u};
    const std::array<std::uint32_t, 3> shape{64, 1, 1};
    registers(0x207, shape);
    const auto address = reinterpret_cast<std::uintptr_t>(ZeroFill.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resource{(static_cast<std::uint32_t>(data.size()) << 1u) | (1u << 7u)};
    registers(0x213, resource);
    registers(0x240, data);
    words.insert(words.end(), {0xc0031500u, static_cast<std::uint32_t>(sizeof(Area) / 4 / 64), 1, 1, 0x41u});
    return words;
}

void Append(std::vector<std::uint32_t>& words, std::initializer_list<std::uint32_t> packet) {
    words.insert(words.end(), packet.begin(), packet.end());
}

void WriteData(std::vector<std::uint32_t>& words, const volatile void* destination, std::uint32_t value, bool predicated = false) {
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    Append(words, {0xc0033700u | (predicated ? 1u : 0u), 0x00100200u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(static_cast<std::uint64_t>(address) >> 32u), value});
}

void SampleDump(std::vector<std::uint32_t>& words, std::uint64_t address) {
    Append(words, {0xc0024600u, 0x139u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)});
}

void CopyQword(std::vector<std::uint32_t>& words, std::uint64_t source, std::uint64_t destination) {
    Append(words, {0xc0044000u, 0x10201u, static_cast<std::uint32_t>(source), static_cast<std::uint32_t>(source >> 32u), static_cast<std::uint32_t>(destination), static_cast<std::uint32_t>(destination >> 32u)});
}

void SetPredication(std::vector<std::uint32_t>& words, std::uint64_t address, bool on) {
    Append(words, {0xc0022000u, on ? ((3u << 16u) | 0x100u) : 0u, on ? static_cast<std::uint32_t>(address) : 0u, on ? static_cast<std::uint32_t>(address >> 32u) : 0u});
}

void Submit(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&packet) == 0, "submission failed");
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
    _putenv_s("APS5_LAB_PIPE_DUMPS", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
    setenv("APS5_LAB_PIPE_DUMPS", "1", 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        using namespace AgcDriver::DriverDetail;
        {
            // Opens the driver's device and imports the dump area; the zero fill leaves the area zero.
            auto words = ZeroFillCommands();
            Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
            AgcDriver::Submit(&packet, 0);
            AgcDriverWaitIdle_nid_postfix();
            AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Area.data()), sizeof(Area));
        }
        Area.fill(Sentinel);
        Labels.fill(0);
        const auto base = reinterpret_cast<std::uintptr_t>(Area.data());
        const std::uint64_t dumpA = base, dumpB = base + 256, copies = base + 512;
        const auto queuedBefore = sampleDumpsQueued.load();
        std::vector<std::uint32_t> words;
        WriteData(words, &Labels[0], 1);
        SampleDump(words, dumpA);
        WriteData(words, &Labels[1], 2);
        SampleDump(words, dumpA + 8);
        CopyQword(words, dumpA, copies);
        CopyQword(words, dumpA + 248, copies + 8);
        SampleDump(words, dumpB);
        SetPredication(words, dumpB, true);
        WriteData(words, &Labels[2], 3, true);
        SetPredication(words, 0, false);
        WriteData(words, &Labels[3], 4);
        Submit(words);
        AgcDriverWaitIdle_nid_postfix();
        AgcDriver::GuestMemory::FlushGpuWrites(base, sizeof(Area));
        check(Labels[0] == 1 && Labels[1] == 2 && Labels[3] == 4, "a label between the dumps did not land");
        check(Labels[2] == 3, "the predicated WRITE_DATA did not see the dump its predicate lives in (label " + std::to_string(Labels[2]) + ")");
        for (std::uint64_t block = 0; block < 16; ++block) {
            check(Area[block * 2] == Ready, "the begin dump of query A wrote block " + std::to_string(block) + " wrongly (" + std::to_string(Area[block * 2]) + ")");
            check(Area[block * 2 + 1] == Ready, "the end dump of query A wrote block " + std::to_string(block) + " wrongly (" + std::to_string(Area[block * 2 + 1]) + ")");
            check(Area[32 + block * 2] == Ready, "dump B wrote block " + std::to_string(block) + " wrongly");
            check(Area[32 + block * 2 + 1] == Sentinel, "dump B wrote between its blocks (block " + std::to_string(block) + ")");
        }
        check(Area[64] == Ready, "the COPY_DATA out of the begin dump did not see the dump (" + std::to_string(Area[64]) + ")");
        check(Area[65] == Ready, "the COPY_DATA out of the end dump did not see the dump (" + std::to_string(Area[65]) + ")");
        check(Area[66] == Sentinel, "a COPY_DATA wrote past its eight bytes");
        const auto queued = sampleDumpsQueued.load() - queuedBefore;
        check(queued == 3, "the sample dumps were not queued on the draw committer (" + std::to_string(queued) + " of 3 queued)");
        AgcDriverShutdown_nid_postfix();
        std::puts("pipelined sample dump tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
