// The MeshArguments pass of a GPU-side mesh indirect draw (Recorder::RecordMeshArguments,
// MeshArguments.comp) against the CPU resolve of the same record (ResolveMeshArguments), for
// indexed (DRAW_INDEX_INDIRECT) and non-indexed (DRAW_INDIRECT) records: empty records, one and
// several groups and instances, a non-zero first index, a non-zero or maximal start vertex (which
// a non-indexed record never applies: the CP writes it only to the SH register its packet names,
// and the GPU path takes only packets that name none), strips, and the device limits. Only the
// compute pass runs, so the test also runs on devices without a mesh compiler for these programs
// (llvmpipe), where tests/Mesh.cpp skips.
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Buffer;
using AgcDriver::Graphics::MeshArgumentRules;
using AgcDriver::Graphics::MeshArguments;
using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::Require;

constexpr ShaderRecompiler::MeshConfiguration Points{1u, 1u, 1u, 1u, 1u, 64u, 1024u, 0u, 4u};
constexpr ShaderRecompiler::MeshConfiguration Triangles{4u, 32u, 96u, 96u, 32u, 128u, 2048u, 0u, 4u};
constexpr ShaderRecompiler::MeshConfiguration Strip{6u, 8u, 10u, 10u, 8u, 64u, 1024u, 0u, 4u};

struct Case {
    const char* what;
    const ShaderRecompiler::MeshConfiguration* mesh;
    bool indexed;
    std::uint32_t indexCount;
    // Indexed: count, instances, first index, vertex offset, start instance. Non-indexed: count,
    // instances, start vertex, start instance (16 bytes).
    std::array<std::uint32_t, 5> record;
};

std::string Text(const MeshArguments& arguments) {
    char text[128];
    std::snprintf(text, sizeof(text), "{groups %u, instances %u, layers %u, index count %u, first index %u}", arguments.groups, arguments.instances, arguments.layers, arguments.indexCount, arguments.firstIndex);
    return text;
}

bool Same(const MeshArguments& a, const MeshArguments& b) {
    return a.groups == b.groups && a.instances == b.instances && a.layers == b.layers && a.indexCount == b.indexCount && a.firstIndex == b.firstIndex;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        auto* recorder = Recorder::Active();
        Require(recorder != nullptr, "the device has no active recorder");
        const auto& context = recorder->GraphicsContext();

        const std::array<Case, 15> cases{{
            {"non-indexed: no vertices", &Triangles, false, 0u, {0u, 1u, 0u, 0u, 0u}},
            {"non-indexed: no instances", &Triangles, false, 0u, {96u, 0u, 0u, 0u, 0u}},
            {"non-indexed: no complete triangle", &Triangles, false, 0u, {2u, 1u, 0u, 0u, 0u}},
            {"non-indexed: one partial group", &Triangles, false, 0u, {6u, 1u, 0u, 0u, 0u}},
            {"non-indexed: several groups and instances", &Triangles, false, 0u, {300u, 3u, 0u, 0u, 0u}},
            {"non-indexed: a start vertex and start instance", &Triangles, false, 0u, {300u, 3u, 1000u, 7u, 0u}},
            {"non-indexed: the highest start vertex", &Triangles, false, 0u, {9u, 1u, 0xffffffffu, 0u, 0u}},
            {"non-indexed: groups times instances over the device limit", &Triangles, false, 0u, {96u * 7u, 600u, 0u, 0u, 0u}},
            {"non-indexed: groups times instances at the device limit", &Triangles, false, 0u, {96u * 6u, 600u, 0u, 0u, 0u}},
            {"non-indexed: groups over the device limit", &Points, false, 0u, {0xffffffffu, 1u, 0u, 0u, 0u}},
            {"non-indexed strip", &Strip, false, 0u, {11u, 2u, 5u, 0u, 0u}},
            {"indexed: count clamped to the index buffer", &Triangles, true, 300u, {200u, 3u, 150u, 0u, 0u}},
            {"indexed: first index past the index buffer", &Triangles, true, 300u, {9u, 1u, 300u, 0u, 0u}},
            {"indexed: one full group with a vertex offset", &Triangles, true, 300u, {96u, 2u, 0u, 5u, 9u}},
            {"indexed strip", &Strip, true, 64u, {11u, 1u, 3u, 0u, 0u}},
        }};
        std::string failures;
        for (const auto& entry : cases) {
            auto rules = AgcDriver::Graphics::MeshArgumentRulesFor(context, *entry.mesh, entry.indexCount, entry.indexed);
            rules.maxGroups = 1000u;
            rules.maxInstances = 600u;
            rules.maxTotal = 4000u;
            const AgcDriver::Pm4::DrawArguments arguments = entry.indexed ? AgcDriver::Pm4::DrawArguments{entry.record[0], entry.record[1], entry.record[2], entry.record[3], entry.record[4]} : AgcDriver::Pm4::DrawArguments{entry.record[0], entry.record[1], entry.record[2], 0u, entry.record[3]};
            const auto expected = AgcDriver::Graphics::ResolveMeshArguments(arguments, rules);
            if (!entry.indexed && (expected.firstIndex != 0u || (expected.indexCount != entry.record[0]))) failures += std::string(entry.what) + ": the CPU resolve applied the start vertex: " + Text(expected) + "\n";

            auto record = std::make_shared<Buffer>(context, 32u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            std::memset(record->Bytes().data(), 0xa5, record->Bytes().size());
            std::memcpy(record->Bytes().data(), entry.record.data(), entry.indexed ? 20u : 16u);
            auto output = std::make_shared<Buffer>(context, sizeof(MeshArguments), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            std::memset(output->Bytes().data(), 0xcd, sizeof(MeshArguments));
            {
                std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
                const auto commands = recorder->Commands();
                const std::array<std::uint32_t, 8> words{rules.indexCount, rules.inputSize, rules.step, rules.primitivesPerGroup, rules.maxGroups, rules.maxInstances, rules.maxTotal, rules.indexed};
                Require(recorder->RecordMeshArguments(commands, record->DeviceAddress(), output->DeviceAddress(), words), "the device has no mesh argument pass");
                AgcDriver::Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
                recorder->Keep(record);
                recorder->Keep(output);
                recorder->Sync();
            }
            MeshArguments gpu{};
            std::memcpy(&gpu, output->Bytes().data(), sizeof(gpu));
            const bool same = Same(gpu, expected);
            std::cout << entry.what << ": GPU " << Text(gpu) << (same ? std::string(" = CPU\n") : ", CPU " + Text(expected) + "\n");
            if (!same) failures += std::string(entry.what) + ": the GPU pass wrote " + Text(gpu) + ", the CPU resolve gives " + Text(expected) + "\n";
        }
        if (!failures.empty()) {
            std::cerr << "mesh indirect arguments differ:\n" << failures;
            return 1;
        }
        std::cout << "mesh indirect argument tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
