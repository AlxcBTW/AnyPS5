#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

// A compute template that binds a plane through a storage image must not outlive the depth surface that
// owns the plane's memory. Templates are served from stamps (Revalidate's fast proof, the moved-buffer
// path on top of it), and no stamp tells that a depth surface appeared over the image after the build:
// the lookup would then hand out the plane's colour mirror, copied in from the depth image, and the
// stale image would be copied over the depth image after the dispatch. The program reads the first row
// of the plane, adds a read-only buffer to it, writes the sum to a buffer and stores the buffer over
// the first row, so a stale image shows in the sums, in the first row and in every other texel the
// copy-out writes back. The planes are read back by programs that are compiled anew (no template).

namespace {

using AgcDriver::Graphics::Require;
using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Draw;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::Shaders;
using DepthFastClearHarness::Surface;
using DepthFastClearHarness::Width;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Bytes = Threads * 4u;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Sentinel = 0xdeadbeefu;
constexpr std::size_t Piece = 65536;

// out[tid] = bits(plane(tid, 0)) + in[tid]; plane(tid, 0) = in[tid]. User data: s[0:3] the output V#,
// s[4:11] the plane's T#, s[16:19] the input V#.
alignas(256) constexpr std::array<std::uint32_t, 15> RowCode{
    0x7e3c0300u,                // v_mov_b32 v30, v0
    0x7e3e0280u,                // v_mov_b32 v31, 0
    0xf0001108u, 0x00010a1eu,   // image_load v10, v[30:31], s[4:11]
    0x34060082u,                // v_lshlrev_b32 v3, 2, v0
    0xe0301000u, 0x80040b03u,   // buffer_load_dword v11, v3, s[16:19], 0 offen
    0xbf8c3f70u,                // s_waitcnt vmcnt(0)
    0x4a18170au,                // v_add_nc_u32 v12, v10, v11
    0xe0701000u, 0x80000c03u,   // buffer_store_dword v12, v3, s[0:3], 0 offen
    0xf0201108u, 0x00010b1eu,   // image_store v11, v[30:31], s[4:11]
    0xbf810000u, 0xbf810000u,
};

// out[tid] = bits(plane(tid, s12)). User data: s[0:3] the output V#, s[4:11] the plane's T#, s12 the row.
alignas(256) constexpr std::array<std::uint32_t, 10> ProbeCode{
    0x7e3c0300u,                // v_mov_b32 v30, v0
    0x7e3e020cu,                // v_mov_b32 v31, s12
    0xf0001108u, 0x00010a1eu,   // image_load v10, v[30:31], s[4:11]
    0x34060082u,                // v_lshlrev_b32 v3, 2, v0
    0xbf8c3f70u,                // s_waitcnt vmcnt(0)
    0xe0701000u, 0x80000a03u,   // buffer_store_dword v10, v3, s[0:3], 0 offen
    0xbf810000u, 0xbf810000u,
};

class GuestBlock {
public:
    GuestBlock(std::size_t pieces, bool inWatch) : bytes(pieces * Piece), watched(inWatch) {
        if (inWatch) {
#ifdef _WIN32
            block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, Piece));
            if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
            constexpr std::uintptr_t alignment = Piece;
            void* mapped = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapped != MAP_FAILED) {
                const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
                const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
                if (aligned != begin) munmap(mapped, aligned - begin);
                if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
                block = reinterpret_cast<std::uint8_t*>(aligned);
                GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
            }
#endif
        } else {
#ifdef _WIN32
            block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
            block = static_cast<std::uint8_t*>(std::aligned_alloc(Piece, bytes));
#endif
            if (block != nullptr) std::memset(block, 0, bytes);
        }
        Require(block != nullptr, "dispatch move depth: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, bytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
        if (watched) {
#ifdef _WIN32
            GuestArena::GuestArenaReset_nid_postfix(block, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
#else
            munmap(block, bytes);
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
#endif
        } else {
#ifdef _WIN32
            VirtualFree(block, 0, MEM_RELEASE);
#else
            std::free(block);
#endif
        }
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* PieceAt(std::size_t index) const { return block + index * Piece; }

private:
    std::size_t bytes = 0;
    bool watched = false;
    std::uint8_t* block = nullptr;
};

// The watched block: the planes first, then the read-only inputs (at three different alignments) and the output.
struct Buffers {
    explicit Buffers(const GuestBlock& block) : block(block) {}
    std::uint32_t* InputA() const { return reinterpret_cast<std::uint32_t*>(block.PieceAt(4)); }
    std::uint32_t* InputB() const { return reinterpret_cast<std::uint32_t*>(block.PieceAt(5) + 12); }
    std::uint32_t* InputC() const { return reinterpret_cast<std::uint32_t*>(block.PieceAt(6) + 32); }
    std::uint32_t* Output() const { return reinterpret_cast<std::uint32_t*>(block.PieceAt(8)); }

private:
    const GuestBlock& block;
};

struct Env {
    AgcDriver::VulkanDevice& device;
    const Shaders& shaders;
    const Buffers& buffers;
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> TextureDescriptor(const void* data) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32Float << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u),
        0xfacu | (Type2D << 28u),
        0u, 0u, 0u, 0u,
    };
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

// Compiled once per plane: a fresh compile has a fresh variant id, so only the runs of one result can share a template.
ShaderRecompiler::RecompileResult RowProgram(AgcDriver::VulkanDevice& device, const void* plane, const std::uint32_t* input, const std::uint32_t* output) {
    std::vector<std::uint32_t> userData(20, 0u);
    const auto out = BufferDescriptor(output, Bytes);
    const auto texture = TextureDescriptor(plane);
    const auto in = BufferDescriptor(input, Bytes);
    std::copy(out.begin(), out.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    std::copy(in.begin(), in.end(), userData.begin() + 16);
    return Compile(device, RowCode, userData);
}

void RunRow(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& compiled, const std::uint32_t* input, const std::uint32_t* output) {
    auto result = compiled;
    std::size_t elements = 0;
    for (auto& binding : result.bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
        Require(binding.guestDescriptor.size() == static_cast<std::size_t>(binding.count) * 4u, "dispatch move depth: the program's buffer bindings have another shape");
        for (std::uint32_t element = 0; element < binding.count; ++element, ++elements) {
            auto* current = binding.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u;
            const auto built = static_cast<std::uint64_t>(current[0]) | (static_cast<std::uint64_t>(current[1] & 0xffffu) << 32u);
            const auto words = built == static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(output)) ? BufferDescriptor(output, Bytes) : BufferDescriptor(input, Bytes);
            std::copy(words.begin(), words.end(), current);
        }
    }
    Require(elements == 2, "dispatch move depth: the program does not bind an input and an output");
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(RowCode.data()));
    device.WaitIdle();
}

std::array<std::uint32_t, Threads> ReadRow(const Env& env, const void* plane, std::uint32_t y) {
    std::vector<std::uint32_t> userData(16, 0u);
    auto* output = env.buffers.Output();
    const auto out = BufferDescriptor(output, Bytes);
    const auto texture = TextureDescriptor(plane);
    std::copy(out.begin(), out.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    userData[12] = y;
    const auto program = Compile(env.device, ProbeCode, userData);
    std::fill(output, output + Threads, Sentinel);
    env.device.Dispatch(program, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(ProbeCode.data()));
    env.device.WaitIdle();
    std::array<std::uint32_t, Threads> lanes{};
    std::copy(output, output + Threads, lanes.begin());
    return lanes;
}

void FillInput(std::uint32_t* where, float first) {
    for (std::uint32_t lane = 0; lane < Threads; ++lane) where[lane] = std::bit_cast<std::uint32_t>(first + static_cast<float>(lane) / 1024.0f);
}

void FillPlane(std::uint8_t* plane, float value) {
    auto* words = reinterpret_cast<std::uint32_t*>(plane);
    std::fill(words, words + Piece / sizeof(std::uint32_t), std::bit_cast<std::uint32_t>(value));
}

// The depth surface over a plane: every texel becomes `depth` (the harness' triangle is at z = 0.5 of the viewport's depth range).
void DrawDepth(const Env& env, const void* plane, float depth) {
    const Surface surface{reinterpret_cast<std::uintptr_t>(plane), 0, 0, false, VK_FORMAT_D32_SFLOAT};
    const float low = depth <= 0.5f ? 0.0f : 2.0f * depth - 1.0f;
    const float high = depth <= 0.5f ? 2.0f * depth : 1.0f;
    Require(Draw(env.device, env.shaders, surface, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS, .minDepth = low, .maxDepth = high}) == Covered, "dispatch move depth: the depth producer did not cover the surface");
}

// What the plane holds: the first row (the lanes' texels) and every other texel, as the program leaves them.
struct PlaneModel {
    std::array<std::uint32_t, Threads> row0{};
    std::uint32_t rest = 0;

    void Set(float value) {
        rest = std::bit_cast<std::uint32_t>(value);
        row0.fill(rest);
    }

    std::array<std::uint32_t, Threads> Dispatch(const std::uint32_t* input) {
        std::array<std::uint32_t, Threads> sums{};
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            sums[lane] = row0[lane] + input[lane];
            row0[lane] = input[lane];
        }
        return sums;
    }
};

void Totals(const std::string& what);

void Step(const Env& env, const ShaderRecompiler::RecompileResult& program, PlaneModel& model, const std::uint32_t* input, const std::string& what) {
    const auto expected = model.Dispatch(input);
    auto* output = env.buffers.Output();
    std::fill(output, output + Threads, Sentinel);
    RunRow(env.device, program, input, output);
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        Require(output[lane] == expected[lane], "dispatch move depth: " + what + ": lane " + std::to_string(lane) + " is " + Hex(output[lane]) + ", expected " + Hex(expected[lane]));
    }
    Totals(what);
}

void Verify(const Env& env, const void* plane, const PlaneModel& model, const std::string& what) {
    for (const std::uint32_t y : {0u, 1u, Height - 1u}) {
        const auto lanes = ReadRow(env, plane, y);
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            const auto expected = y == 0 ? model.row0[lane] : model.rest;
            Require(lanes[lane] == expected, "dispatch move depth: " + what + ": row " + std::to_string(y) + ", texel " + std::to_string(lane) + " of the plane is " + Hex(lanes[lane]) + ", expected " + Hex(expected));
        }
    }
}

struct MoveSteps {
    std::uint64_t checked, exact, moved, refused;
};

MoveSteps Since(const AgcDriver::VulkanDevice::DispatchMoveCount& before) {
    const auto now = AgcDriver::VulkanDevice::DispatchMoveCounts();
    return {now.checked - before.checked, now.exact - before.exact, now.moved - before.moved, now.refused - before.refused};
}

void Totals(const std::string& what) {
    const auto now = AgcDriver::VulkanDevice::DispatchMoveCounts();
    std::printf("dispatch move depth: %s: in total checked %llu, exact %llu, moved %llu, refused %llu, dropped by stage A %llu\n", what.c_str(), static_cast<unsigned long long>(now.checked), static_cast<unsigned long long>(now.exact), static_cast<unsigned long long>(now.moved), static_cast<unsigned long long>(now.refused), static_cast<unsigned long long>(now.filtered));
}

void DescribeBindings(const ShaderRecompiler::RecompileResult& program) {
    for (const auto& binding : program.bindings) {
        std::string written;
        for (const bool bit : binding.bufferWritten) written += bit ? '1' : '0';
        std::printf("dispatch move depth: binding role %d kind %d count %u, %zu descriptor words, buffers written [%s], images written %zu\n", static_cast<int>(binding.role), static_cast<int>(binding.kind), binding.count, binding.guestDescriptor.size(), written.c_str(), binding.imageWritten.size());
    }
}

void Print(const char* what, const MoveSteps& step) {
    std::printf("dispatch move depth: %s: checked %llu, exact %llu, moved %llu, refused %llu\n", what, static_cast<unsigned long long>(step.checked), static_cast<unsigned long long>(step.exact), static_cast<unsigned long long>(step.moved), static_cast<unsigned long long>(step.refused));
}

// No depth surface anywhere near the plane: the template serves the repeats (the positive control).
bool Control(const Env& env, std::uint8_t* plane) {
    FillPlane(plane, 0.5f);
    PlaneModel model;
    model.Set(0.5f);
    const auto program = RowProgram(env.device, plane, env.buffers.InputA(), env.buffers.Output());
    DescribeBindings(program);
    Step(env, program, model, env.buffers.InputA(), "control: the first dispatch");
    Totals("control, after the first dispatch");
    const auto start = AgcDriver::VulkanDevice::DispatchMoveCounts();
    Step(env, program, model, env.buffers.InputB(), "control: an input moved");
    Totals("control, after an input moved");
    Step(env, program, model, env.buffers.InputC(), "control: the input moved again");
    Totals("control, after the input moved again");
    Step(env, program, model, env.buffers.InputC(), "control: an exact repeat");
    Totals("control, after an exact repeat");
    const auto steps = Since(start);
    Print("control, a plane without a depth surface", steps);
    Verify(env, plane, model, "control");
    Require(steps.refused == 0, "dispatch move depth: the control's repeats were refused");
    return steps.checked != 0;
}

// The depth surface exists before the first dispatch: the template holds the plane's colour mirror, which the
// lookup copies in from the depth image every time.
void Mirror(const Env& env, std::uint8_t* plane, const char* where) {
    const std::string at = std::string(where) + ", the mirror: ";
    FillPlane(plane, 0.5f);
    PlaneModel model;
    DrawDepth(env, plane, 0.25f);
    model.Set(0.25f);
    const auto program = RowProgram(env.device, plane, env.buffers.InputA(), env.buffers.Output());
    Step(env, program, model, env.buffers.InputA(), at + "the first dispatch");
    Verify(env, plane, model, at + "after the first dispatch");
    DrawDepth(env, plane, 0.75f);
    model.Set(0.75f);
    const auto start = AgcDriver::VulkanDevice::DispatchMoveCounts();
    Step(env, program, model, env.buffers.InputB(), at + "an input moved after the depth was drawn again");
    Verify(env, plane, model, at + "after the moved dispatch");
    Step(env, program, model, env.buffers.InputB(), at + "an exact repeat");
    DrawDepth(env, plane, 0.25f);
    model.Set(0.25f);
    Step(env, program, model, env.buffers.InputC(), at + "the input moved again after another redraw");
    Verify(env, plane, model, at + "after the last dispatch");
    Print((at + "repeats and redraws").c_str(), Since(start));
}

// The depth surface appears after the template was built over a plain storage image of the memory: the repeat must
// not be served from that image. `moved` selects an input that moved, else the very input of the first dispatch.
void Appears(const Env& env, std::uint8_t* plane, const char* where, bool moved) {
    const std::string at = std::string(where) + (moved ? ", a surface appears, input moved: " : ", a surface appears, exact repeat: ");
    FillPlane(plane, 0.125f);
    PlaneModel model;
    model.Set(0.125f);
    const auto program = RowProgram(env.device, plane, env.buffers.InputA(), env.buffers.Output());
    Step(env, program, model, env.buffers.InputA(), at + "the first dispatch, no depth surface yet");
    DrawDepth(env, plane, 0.25f);
    model.Set(0.25f);
    const auto start = AgcDriver::VulkanDevice::DispatchMoveCounts();
    Step(env, program, model, moved ? env.buffers.InputB() : env.buffers.InputA(), at + "the dispatch after the surface appeared");
    Print((at + "the dispatch after the surface appeared").c_str(), Since(start));
    Totals(at + "after that dispatch");
    Verify(env, plane, model, at + "after that dispatch");
    DrawDepth(env, plane, 0.75f);
    model.Set(0.75f);
    Step(env, program, model, env.buffers.InputC(), at + "the input moved after the next redraw");
    Verify(env, plane, model, at + "after the last dispatch");
}

bool RunTests(const Env& env, const GuestBlock& watched, const GuestBlock& unwatched) {
    FillInput(env.buffers.InputA(), 0.0625f);
    FillInput(env.buffers.InputB(), 0.109375f);
    FillInput(env.buffers.InputC(), 0.15625f);
    const bool templates = Control(env, watched.PieceAt(0));
    if (!templates) std::puts("dispatch move depth: the device kept no template (no host imports); the moved-buffer path is not exercised");
    Mirror(env, watched.PieceAt(1), "inside the write watch");
    Appears(env, watched.PieceAt(2), "inside the write watch", true);
    Appears(env, watched.PieceAt(3), "inside the write watch", false);
    Mirror(env, unwatched.PieceAt(0), "outside the write watch");
    Appears(env, unwatched.PieceAt(1), "outside the write watch", true);
    Appears(env, unwatched.PieceAt(2), "outside the write watch", false);
    return templates;
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_LAB_DISPATCH_MOVE", "1");
        _putenv_s("APS5_LAB_FAST_PROOF", "2");
#else
        setenv("APS5_LAB_DISPATCH_MOVE", "1", 1);
        setenv("APS5_LAB_FAST_PROOF", "2", 1);
#endif
        const GuestBlock watched(10, true);
        const GuestBlock unwatched(4, false);
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto shaders = DepthFastClearHarness::Compile(*device);
        const Buffers buffers(watched);
        const Env env{*device, shaders, buffers};
        if (!RunTests(env, watched, unwatched)) return VulkanTestSkipped;
        std::puts("dispatch move depth tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
