// The checks behind the driver's empty indirect draw skip (Driver::draw): which indirect draws take the
// CPU path before any stage is captured (Driver::IndirectCpuBeforeCapture, the stage-independent part of
// ClassifyIndirectDraw) and which records the CPU loop draws (Driver::IndirectRecordDraws).
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>

using AgcDriver::DriverDetail::Driver;
using AgcDriver::Graphics::IndirectDrawPath;
using AgcDriver::Graphics::ShaderPath;
using AgcDriver::Graphics::ShaderStages;
using AgcDriver::Pm4::DrawArguments;
using AgcDriver::Pm4::DrawParameters;
using Support = AgcDriver::VulkanDevice::IndirectDrawSupport;

namespace {

constexpr std::uint32_t NoLocation = 0x280u;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

DrawParameters indirectDraw(bool indexed, std::uint32_t count = 1, std::uint32_t indexCount = 0) {
    DrawParameters draw{0, indexCount, indexed ? 2u : 0u, 0, 0, indexed, 0, 0};
    DrawParameters::IndirectDraw indirect{};
    indirect.arguments = 0x10000;
    indirect.opcode = indexed ? 0x25u : 0x24u;
    indirect.recordBytes = indexed ? 20u : 16u;
    indirect.stride = indirect.recordBytes;
    indirect.count = count;
    indirect.baseVertexLocation = NoLocation;
    indirect.startInstanceLocation = NoLocation;
    indirect.drawIndexLocation = NoLocation;
    draw.indirect = indirect;
    return draw;
}

ShaderStages vertexStages() {
    return {ShaderPath::Vertex, 0, 64, 64, std::nullopt, std::nullopt};
}

ShaderStages meshStages() {
    ShaderStages stages{ShaderPath::Geometry, 0, 32, 32, std::nullopt, std::nullopt};
    stages.mesh = ShaderRecompiler::MeshConfiguration{4, 64, 192, 192, 64, 64, 0, 0, 4};
    return stages;
}

void testCpuBeforeCapture() {
    const Support full{true, true, true};
    // A plain vertex-path draw is decided by the front stage: nothing is known before the capture.
    check(!Driver::IndirectCpuBeforeCapture(vertexStages(), false, indirectDraw(false), full), "a vertex-path draw was sent to the CPU before its capture");
    check(!Driver::IndirectCpuBeforeCapture(vertexStages(), false, indirectDraw(true), full), "an indexed vertex-path draw was sent to the CPU before its capture");
    // A non-vertex path always takes the CPU path; so does a rect list, unless APS5_GPU_INDEXED_INDIRECT is
    // set (the agc_driver_empty_indirect_gpu_indexed registration), which sends it down the GPU-side path
    // with the vertex-path draw's remaining checks (draw index SGPR, device gaps).
    const char* indexedText = std::getenv("APS5_GPU_INDEXED_INDIRECT");
    const bool gpuIndexed = indexedText != nullptr && indexedText[0] != '\0' && std::string(indexedText) != "0";
    check(gpuIndexed == AgcDriver::Graphics::GpuIndexedIndirect(), "the GPU indexed indirect switch does not follow APS5_GPU_INDEXED_INDIRECT");
    check(AgcDriver::Graphics::GpuIndexedIndirectVerify() == (indexedText != nullptr && std::string(indexedText) == "verify"), "the verify mode does not follow APS5_GPU_INDEXED_INDIRECT=verify");
    for (const bool indexed : {false, true}) {
        const auto rect = Driver::IndirectCpuBeforeCapture(vertexStages(), true, indirectDraw(indexed), full);
        if (gpuIndexed) check(!rect, "a rect-list indirect draw stayed on the CPU under APS5_GPU_INDEXED_INDIRECT");
        else check(rect == IndirectDrawPath::NonVertexPath, "a rect-list indirect draw was not a CPU draw");
        if (!gpuIndexed) continue;
        auto drawIndex = indirectDraw(indexed, 4);
        drawIndex.indirect->drawIndexEnabled = true;
        drawIndex.indirect->drawIndexSgpr = 7;
        check(Driver::IndirectCpuBeforeCapture(vertexStages(), true, drawIndex, full) == IndirectDrawPath::DrawIndex, "a rect-list draw with a draw index SGPR stayed on the GPU");
        auto counted = indirectDraw(indexed, 4);
        counted.indirect->countIndirect = true;
        counted.indirect->countAddress = 0x20000;
        check(!Driver::IndirectCpuBeforeCapture(vertexStages(), true, counted, full), "a counted rect-list draw was sent to the CPU with the count feature");
        check(Driver::IndirectCpuBeforeCapture(vertexStages(), true, counted, Support{true, false, false}) == IndirectDrawPath::FeatureGap, "a counted rect-list draw stayed on the GPU without the count feature");
        ShaderStages rectMesh = meshStages();
        check(Driver::IndirectCpuBeforeCapture(rectMesh, true, indirectDraw(indexed), full) == IndirectDrawPath::NonVertexPath, "a rect-list mesh draw stayed on the GPU");
    }
    ShaderStages tessellation{ShaderPath::Tessellation, 0, 64, 64, std::nullopt, std::nullopt};
    check(Driver::IndirectCpuBeforeCapture(tessellation, false, indirectDraw(true), full) == IndirectDrawPath::NonVertexPath, "a tessellation indirect draw was not a CPU draw");
    // The single-record mesh draw without SGPR locations runs on the GPU (RecordMeshArguments):
    // indexed always, non-indexed unless APS5_NO_GPU_MESH_INDIRECT=1 (the agc_driver_empty_indirect_no_gpu_mesh
    // registration runs this test with it).
    const bool gpuMesh = std::getenv("APS5_NO_GPU_MESH_INDIRECT") == nullptr;
    check(gpuMesh == AgcDriver::Graphics::GpuMeshIndirectDraws(), "the GPU mesh indirect switch does not follow APS5_NO_GPU_MESH_INDIRECT");
    check(!Driver::IndirectCpuBeforeCapture(meshStages(), false, indirectDraw(true), full), "the GPU-side mesh draw was sent to the CPU");
    if (gpuMesh) check(!Driver::IndirectCpuBeforeCapture(meshStages(), false, indirectDraw(false), full), "the GPU-side non-indexed mesh draw was sent to the CPU");
    else check(Driver::IndirectCpuBeforeCapture(meshStages(), false, indirectDraw(false), full) == IndirectDrawPath::NonVertexPath, "a non-indexed mesh indirect draw stayed on the GPU under APS5_NO_GPU_MESH_INDIRECT=1");
    for (const bool indexed : {true, false}) {
        auto located = indirectDraw(indexed);
        located.indirect->startInstanceLocation = 0x30cu;
        check(Driver::IndirectCpuBeforeCapture(meshStages(), false, located, full) == IndirectDrawPath::NonVertexPath, "a mesh draw with a start instance location stayed on the GPU");
        located = indirectDraw(indexed);
        located.indirect->baseVertexLocation = 0x30cu;
        check(Driver::IndirectCpuBeforeCapture(meshStages(), false, located, full) == IndirectDrawPath::NonVertexPath, "a mesh draw with a base vertex location stayed on the GPU");
        auto multi = indirectDraw(indexed, 2);
        check(Driver::IndirectCpuBeforeCapture(meshStages(), false, multi, full) == IndirectDrawPath::NonVertexPath, "a two-record mesh draw stayed on the GPU");
        auto counted = indirectDraw(indexed, 1);
        counted.indirect->countIndirect = true;
        counted.indirect->countAddress = 0x20000;
        check(Driver::IndirectCpuBeforeCapture(meshStages(), false, counted, full) == IndirectDrawPath::NonVertexPath, "a counted mesh draw stayed on the GPU");
        auto indexedDraw = indirectDraw(indexed, 1);
        indexedDraw.indirect->drawIndexEnabled = true;
        indexedDraw.indirect->drawIndexLocation = 0x30du;
        check(Driver::IndirectCpuBeforeCapture(meshStages(), false, indexedDraw, full) == IndirectDrawPath::NonVertexPath, "a mesh draw with a draw index location stayed on the GPU");
    }
    // A draw index the front program reads in an SGPR, and device gaps, are known before the capture.
    auto drawIndex = indirectDraw(false, 4);
    drawIndex.indirect->drawIndexEnabled = true;
    drawIndex.indirect->drawIndexSgpr = 12;
    check(Driver::IndirectCpuBeforeCapture(vertexStages(), false, drawIndex, full) == IndirectDrawPath::DrawIndex, "a draw index SGPR was not a CPU draw");
    drawIndex.indirect->drawIndexSgpr = -1;
    check(!Driver::IndirectCpuBeforeCapture(vertexStages(), false, drawIndex, full), "an unread draw index sent the draw to the CPU");
    auto counted = indirectDraw(false, 4);
    counted.indirect->countIndirect = true;
    counted.indirect->countAddress = 0x20000;
    check(!Driver::IndirectCpuBeforeCapture(vertexStages(), false, counted, full), "a counted draw on a device with count support went to the CPU");
    check(Driver::IndirectCpuBeforeCapture(vertexStages(), false, counted, Support{true, true, false}) == IndirectDrawPath::FeatureGap, "a counted draw without count support stayed on the GPU");
    check(Driver::IndirectCpuBeforeCapture(vertexStages(), false, counted, Support{true, false, true}) == IndirectDrawPath::FeatureGap, "a counted multi draw without multi-draw support stayed on the GPU");
    // The first reason wins, in ClassifyIndirectDraw's order.
    drawIndex.indirect->drawIndexSgpr = 3;
    check(Driver::IndirectCpuBeforeCapture(meshStages(), false, drawIndex, full) == IndirectDrawPath::NonVertexPath, "the non-vertex path was not the first reason");
}

void testRecordDraws() {
    const auto plain = indirectDraw(false);
    check(Driver::IndirectRecordDraws(DrawArguments{3, 1, 0, 0, 0}, plain), "a record with vertices and an instance drew nothing");
    check(!Driver::IndirectRecordDraws(DrawArguments{0, 1, 0, 0, 0}, plain), "a record without vertices drew");
    check(!Driver::IndirectRecordDraws(DrawArguments{3, 0, 0, 0, 0}, plain), "a record without instances drew");
    check(Driver::IndirectRecordDraws(DrawArguments{3, 1, 0xfffffff0u, 0, 7}, plain), "a non-indexed record's start vertex was bounded");
    const auto indexed = indirectDraw(true, 1, 96);
    check(Driver::IndirectRecordDraws(DrawArguments{6, 2, 90, 5, 0}, indexed), "an indexed record inside the bound range drew nothing");
    check(!Driver::IndirectRecordDraws(DrawArguments{6, 2, 96, 0, 0}, indexed), "an indexed record starting at the end of the range drew");
    check(!Driver::IndirectRecordDraws(DrawArguments{6, 2, 200, 0, 0}, indexed), "an indexed record past the range drew");
}

}

int main() {
    try {
        testCpuBeforeCapture();
        testRecordDraws();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "empty indirect test failed: %s\n", error.what());
        return 1;
    }
    std::puts("empty indirect tests passed");
    return 0;
}
