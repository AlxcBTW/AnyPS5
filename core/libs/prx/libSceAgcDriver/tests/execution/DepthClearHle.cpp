// Lab APS5_LAB_DEPTH_CLEAR_HLE: a compute kernel that stores one colour to every texel of a storage image over a depth
// plane (the UAV clear games run over shadow atlases) leaves out the copies of the plane around the dispatch and clears
// the depth image to the stored value instead. The kernel is the one a title runs: 15 words, 8 x 8 threads, the T# in
// user data 0-7, the V# of the colour in user data 8-11, groups 8 x 4 for the 64 x 32 plane.
// Three layers: the matcher alone; the lever's scope around a dispatch on the test's own device, where the depth plane
// of the harness lives (a driver submit would use another VkDevice, which has no such surface); and a PM4 submit through
// the driver, which only the matching, the scope and the colour read can reach (counters), since the driver's device
// holds no depth surface.
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using DepthFastClearHarness::Compile;
using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Draw;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::Require;
using DepthFastClearHarness::Shaders;
using DepthFastClearHarness::Surface;
using DepthFastClearHarness::Width;

namespace Graphics = AgcDriver::Graphics;

constexpr std::uint32_t Format16UNorm = 7;
constexpr std::uint32_t Format16Uint = 11;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Format32Uint = 20;
constexpr std::uint32_t Type2DArray = 13;
constexpr std::uint32_t TileLinear = 0;
constexpr std::uint32_t TileZ64KBX = 0x18;

constexpr std::array<std::uint32_t, 15> ClearKernel{0xd7460004u, 0x0401060cu, 0xf4280204u, 0xfa000000u, 0xd7460005u, 0x0405060du, 0x7e0c020eu, 0xbf8cc07fu, 0x7e000208u, 0x7e020209u, 0x7e04020au, 0x7e06020bu, 0xf0200f28u, 0x00000004u, 0xbf810000u};

class GuestBlock {
public:
    GuestBlock() {
        constexpr std::size_t bytes = 65536;
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(bytes, bytes));
#endif
        Require(block != nullptr, "depth clear: cannot allocate a guest block");
        std::memset(block, 0, bytes);
        GuestAllocations::Mutation().Add(block, bytes, true, true, true);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() const { return block; }

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

GuestBlock& NewBlock() {
    static auto* blocks = new std::deque<GuestBlock>();
    return blocks->emplace_back();
}

std::uint32_t Low(std::uint64_t value) { return static_cast<std::uint32_t>(value); }

std::uint32_t High(std::uint64_t value) { return static_cast<std::uint32_t>(value >> 32u); }

std::vector<std::uint32_t> Pm4Packet(std::uint32_t opcode, std::initializer_list<std::uint32_t> body) {
    std::vector<std::uint32_t> words{0xc0000000u | (static_cast<std::uint32_t>(body.size() - 1) << 16u) | (opcode << 8u)};
    words.insert(words.end(), body);
    return words;
}

void Submit(std::vector<std::vector<std::uint32_t>> packets) {
    std::vector<std::uint32_t> words;
    for (const auto& packet : packets) words.insert(words.end(), packet.begin(), packet.end());
    Packet dcb{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&dcb) == 0, "depth clear: DCB submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

std::array<std::uint32_t, 8> TextureDescriptor(std::uint64_t address, std::uint32_t format, std::uint32_t tile = TileLinear, std::uint32_t type = Type2DArray) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u),
        0xfacu | (tile << 20u) | (type << 28u),
        0u, 0u, 0u, 0u,
    };
}

std::array<std::uint32_t, 4> BufferDescriptor(std::uint64_t address, std::uint32_t bytes) {
    return {Low(address), High(address) & 0xffffu, bytes, 0x31016facu};
}

// One dispatch of the kernel over `plane` with the colour in `color`: `groupsX` x `groupsY` groups of 8 x 8 threads.
void DispatchClear(const GuestBlock& code, const GuestBlock& plane, const GuestBlock& color, std::uint32_t format, std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t tile = TileLinear) {
    const auto kernel = code.Address();
    const auto texture = TextureDescriptor(plane.Address(), format, tile);
    const auto buffer = BufferDescriptor(color.Address(), 16u);
    std::vector<std::vector<std::uint32_t>> packets{
        Pm4Packet(0x76, {0x20c, Low(kernel >> 8u)}),
        Pm4Packet(0x76, {0x20d, High(kernel >> 8u) & 0xffu}),
        Pm4Packet(0x76, {0x207, 8}),
        Pm4Packet(0x76, {0x208, 8}),
        Pm4Packet(0x76, {0x209, 1}),
        Pm4Packet(0x76, {0x213, (12u << 1u) | 0x80u | 0x100u | 0x200u | (1u << 11u)}),
    };
    for (std::uint32_t word = 0; word < 8u; ++word) packets.push_back(Pm4Packet(0x76, {0x240u + word, texture[word]}));
    for (std::uint32_t word = 0; word < 4u; ++word) packets.push_back(Pm4Packet(0x76, {0x248u + word, buffer[word]}));
    packets.push_back(Pm4Packet(0x15, {groupsX, groupsY, 1, 0x41}));
    Submit(std::move(packets));
}

// One dispatch of the kernel over `plane` through `device`, recorded under the lever's scope as Driver::dispatch does.
void RunClear(AgcDriver::VulkanDevice& device, const GuestBlock& code, const GuestBlock& plane, const GuestBlock& color, std::uint32_t format, std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t tile = TileLinear) {
    std::vector<std::uint32_t> userData(12);
    const auto texture = TextureDescriptor(plane.Address(), format, tile);
    const auto buffer = BufferDescriptor(color.Address(), 16u);
    std::copy(texture.begin(), texture.end(), userData.begin());
    std::copy(buffer.begin(), buffer.end(), userData.begin() + 8);
    const std::span<const std::uint32_t> kernel(reinterpret_cast<const std::uint32_t*>(code.Data()), ClearKernel.size());
    const ShaderRecompiler::ShaderComputeStageInfo compute{{8, 8, 1}, 0u, {true, true, true}, false, 2u, {0, 0, 0}};
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{
        {static_cast<std::uintptr_t>(code.Address()), std::as_bytes(kernel)},
        {static_cast<std::uintptr_t>(color.Address()), std::span<const std::byte>(reinterpret_cast<const std::byte*>(color.Data()), 16)},
    }};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, static_cast<std::uintptr_t>(code.Address()), kernel, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto program = ShaderRecompiler::Recompile(request);
    std::optional<Graphics::DepthPlaneClear> scope;
    if (const auto clear = Graphics::MatchStorageImageClear(kernel, userData, compute, {groupsX, groupsY, 1u})) scope.emplace(*clear);
    device.Dispatch(program, groupsX, groupsY, 1, {}, static_cast<std::uintptr_t>(code.Address()));
    scope.reset();
    device.WaitIdle();
}

void SetColor(GuestBlock& color, float value) {
    const std::array<float, 4> channels{value, value, value, value};
    std::memcpy(color.Data(), channels.data(), sizeof(channels));
    AgcDriver::GuestMemory::MarkWritten(color.Address(), sizeof(channels));
}

void SetRaw(GuestBlock& color, std::uint32_t raw) {
    const std::array<std::uint32_t, 4> channels{raw, 0u, 0u, 0u};
    std::memcpy(color.Data(), channels.data(), sizeof(channels));
    AgcDriver::GuestMemory::MarkWritten(color.Address(), sizeof(channels));
}

Graphics::DepthClearStatistics Difference(const Graphics::DepthClearStatistics& now, const Graphics::DepthClearStatistics& before) {
    Graphics::DepthClearStatistics difference;
    difference.matched = now.matched - before.matched;
    difference.taken = now.taken - before.taken;
    difference.copiesSkipped = now.copiesSkipped - before.copiesSkipped;
    difference.clears = now.clears - before.clears;
    difference.bytesSkipped = now.bytesSkipped - before.bytesSkipped;
    difference.verified = now.verified - before.verified;
    difference.violations = now.violations - before.violations;
    for (std::size_t reason = 0; reason < difference.declined.size(); ++reason) difference.declined[reason] = now.declined[reason] - before.declined[reason];
    return difference;
}

std::uint64_t Declined(const Graphics::DepthClearStatistics& statistics) {
    std::uint64_t total = 0;
    for (const auto count : statistics.declined) total += count;
    return total;
}

// The matcher: the kernel's words, the user data count, the group size and the covered threads decide.
void MatcherCases() {
    std::vector<std::uint32_t> userData(12);
    for (std::uint32_t word = 0; word < userData.size(); ++word) userData[word] = 0x1000u + word;
    ShaderRecompiler::ShaderComputeStageInfo compute{{8, 8, 1}, 0u, {true, true, true}, false, 2u, {0, 0, 0}};
    const std::array<std::uint32_t, 3> groups{8, 4, 1};
    std::vector<std::uint32_t> code(ClearKernel.begin(), ClearKernel.end());
    code.insert(code.end(), {0u, 0u, 0u, 0u});

    const auto clear = Graphics::MatchStorageImageClear(code, userData, compute, groups);
    Require(clear.has_value(), "the clear kernel was not recognized");
    Require(clear->width == 64u && clear->height == 32u, "the clear covers " + std::to_string(clear->width) + " x " + std::to_string(clear->height) + " texels, expected 64 x 32");
    for (std::uint32_t word = 0; word < 8u; ++word) Require(clear->image[word] == 0x1000u + word, "the clear's T# is not user data 0-7");
    for (std::uint32_t word = 0; word < 4u; ++word) Require(clear->color[word] == 0x1008u + word, "the clear's colour V# is not user data 8-11");

    for (std::size_t word = 0; word < ClearKernel.size(); ++word) {
        auto changed = code;
        changed[word] ^= 0x00000100u;
        Require(!Graphics::MatchStorageImageClear(changed, userData, compute, groups), "a kernel with word " + std::to_string(word) + " changed was recognized");
    }
    Require(!Graphics::MatchStorageImageClear(std::span(code).first(ClearKernel.size() - 1), userData, compute, groups), "a truncated kernel was recognized");
    Require(Graphics::MatchStorageImageClear(std::span(code).first(ClearKernel.size()), userData, compute, groups).has_value(), "the kernel alone, without trailing words, was not recognized");

    auto shortUserData = userData;
    shortUserData.pop_back();
    Require(!Graphics::MatchStorageImageClear(code, shortUserData, compute, groups), "eleven user registers were accepted (s12 would be a user register, not the group id)");
    auto longUserData = userData;
    longUserData.push_back(0u);
    Require(!Graphics::MatchStorageImageClear(code, longUserData, compute, groups), "thirteen user registers were accepted (s12 would be a user register, not the group id)");

    auto wide = compute;
    wide.numThreads = {8, 8, 2};
    Require(!Graphics::MatchStorageImageClear(code, userData, wide, groups), "8 x 8 x 2 threads were accepted");
    wide.numThreads = {16, 4, 1};
    Require(!Graphics::MatchStorageImageClear(code, userData, wide, groups), "16 x 4 threads were accepted");
    for (std::size_t axis = 0; axis < 3u; ++axis) {
        auto missing = compute;
        missing.groupIdEnable[axis] = false;
        Require(!Graphics::MatchStorageImageClear(code, userData, missing, groups), "a dispatch without the group id of axis " + std::to_string(axis) + " was accepted");
    }
    auto oneComponent = compute;
    oneComponent.threadIdComponentCount = 1u;
    Require(!Graphics::MatchStorageImageClear(code, userData, oneComponent, groups), "a dispatch with only the x thread id was accepted");

    Require(!Graphics::MatchStorageImageClear(code, userData, compute, {8, 4, 2}), "two layers of groups were accepted");
    Require(!Graphics::MatchStorageImageClear(code, userData, compute, {0, 4, 1}), "zero groups along x were accepted");
    Require(!Graphics::MatchStorageImageClear(code, userData, compute, {8, 0, 1}), "zero groups along y were accepted");

    auto partial = compute;
    partial.partialThreads = {60, 30, 1};
    const auto cut = Graphics::MatchStorageImageClear(code, userData, partial, groups);
    Require(cut.has_value() && cut->width == 60u && cut->height == 30u, "a dispatch of 60 x 30 threads does not cover 60 x 30 texels");
}

// Runs the same sequence of clears under `mode` and returns what the lever counted. The depth the plane holds is
// found by depth tests of the harness' triangle (z = 0.5) against it; the guest memory is read after a flush.
Graphics::DepthClearStatistics ClearSequence(AgcDriver::VulkanDevice& device, const Shaders& shaders, int mode, std::uint32_t perturb = 0) {
    Graphics::SetDepthClearModeForTest(mode, perturb);
    const auto before = Graphics::DepthClearCounts();
    const std::string label = "mode " + std::to_string(mode) + ": ";
    auto& code = NewBlock();
    std::memcpy(code.Data(), ClearKernel.data(), sizeof(ClearKernel));
    auto& plane = NewBlock();
    auto& color = NewBlock();
    const Surface surface{plane.Address(), 0, 0, false, VK_FORMAT_D32_SFLOAT};
    const auto drawLess = [&] { return Draw(device, shaders, surface, {.depthCompare = VK_COMPARE_OP_LESS}); };
    const auto drawGreater = [&] { return Draw(device, shaders, surface, {.depthCompare = VK_COMPARE_OP_GREATER}); };
    const auto drawEqual = [&] { return Draw(device, shaders, surface, {.depthCompare = VK_COMPARE_OP_EQUAL}); };

    Require(Draw(device, shaders, surface, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}) == Covered, label + "the producer did not cover the plane");
    Require(drawLess() == 0u, label + "the plane does not hold the 0.5 the producer wrote");
    Require(drawEqual() == Covered, label + "the plane does not hold exactly 0.5");

    SetColor(color, 1.0f);
    RunClear(device, code, plane, color, Format32Float, 8, 4);
    Require(drawLess() == Covered, label + "after a clear to 1.0 the depth test 0.5 < 1.0 did not pass everywhere");
    Require(drawGreater() == 0u, label + "after a clear to 1.0 a texel passed 0.5 > depth");

    SetColor(color, 0.25f);
    RunClear(device, code, plane, color, Format32Float, 8, 4);
    Require(drawLess() == 0u, label + "after a clear to 0.25 a texel passed 0.5 < depth");
    Require(drawGreater() == Covered, label + "after a clear to 0.25 the depth test 0.5 > 0.25 did not pass everywhere");

    SetColor(color, 0.5f);
    RunClear(device, code, plane, color, Format32Float, 8, 4);
    Require(drawEqual() == Covered, label + "after a clear to 0.5 the plane does not hold exactly 0.5");

    // Half the plane: the clear cannot be taken (the other half keeps its depth), whatever the mode.
    SetColor(color, 0.75f);
    RunClear(device, code, plane, color, Format32Float, 4, 4);
    Require(drawGreater() == 0u, label + "after a clear of the left half to 0.75 a texel passed 0.5 > depth");
    Require(drawEqual() == Covered / 2u, label + "after a clear of the left half the right half does not keep 0.5");
    Require(drawLess() == Covered / 2u, label + "after a clear of the left half the left half does not hold 0.75");

    // Values the lever does not take: an integer format, a negative value, a value above 1.
    SetColor(color, 2.0f);
    RunClear(device, code, plane, color, Format32Float, 8, 4);
    Require(drawEqual() == 0u, label + "after a clear to 2.0 a texel still holds exactly 0.5");

    SetColor(color, 0.125f);
    RunClear(device, code, plane, color, Format32Float, 8, 4);
    Require(drawGreater() == Covered, label + "after the last full clear the depth test 0.5 > 0.125 did not pass everywhere");
    Require(drawLess() == 0u, label + "after the last full clear a texel passed 0.5 < depth");

    // The guest memory after the store-back: the mirror was written by the dispatch in every mode.
    Graphics::StorageTexture::FlushPending(plane.Address(), Width * Height * sizeof(float), nullptr, "test");
    device.WaitIdle();
    for (std::uint32_t texel = 0; texel < Width * Height; ++texel) {
        float value = 0.0f;
        std::memcpy(&value, plane.Data() + texel * sizeof(float), sizeof(value));
        Require(value == 0.125f, label + "guest depth texel " + std::to_string(texel) + " is " + std::to_string(value) + " after the flush, expected 0.125");
    }
    return Difference(Graphics::DepthClearCounts(), before);
}

// An integer storage view of a 32-bit plane clears to the bits the colour holds.
void IntegerViewCase(AgcDriver::VulkanDevice& device, const Shaders& shaders, int mode) {
    Graphics::SetDepthClearModeForTest(mode);
    const auto before = Graphics::DepthClearCounts();
    const std::string label = "integer view, mode " + std::to_string(mode) + ": ";
    auto& code = NewBlock();
    std::memcpy(code.Data(), ClearKernel.data(), sizeof(ClearKernel));
    auto& plane = NewBlock();
    auto& color = NewBlock();
    const Surface surface{plane.Address(), 0, 0, false, VK_FORMAT_D32_SFLOAT};
    Require(Draw(device, shaders, surface, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}) == Covered, label + "the producer did not cover the plane");
    const std::array<std::uint32_t, 4> bits{std::bit_cast<std::uint32_t>(0.25f), 0u, 0u, 0u};
    std::memcpy(color.Data(), bits.data(), sizeof(bits));
    AgcDriver::GuestMemory::MarkWritten(color.Address(), sizeof(bits));
    RunClear(device, code, plane, color, Format32Uint, 8, 4, TileZ64KBX);
    Require(Draw(device, shaders, surface, {.depthCompare = VK_COMPARE_OP_GREATER}) == Covered, label + "after a clear to the bits of 0.25 the depth test 0.5 > 0.25 did not pass everywhere");
    Require(Draw(device, shaders, surface, {.depthCompare = VK_COMPARE_OP_LESS}) == 0u, label + "after a clear to the bits of 0.25 a texel passed 0.5 < depth");
    const auto difference = Difference(Graphics::DepthClearCounts(), before);
    if (mode == 0) Require(difference.matched == 0u && difference.clears == 0u, label + "the lever counted with the switch off");
    else Require(difference.clears == 1u, label + "the clear was not taken (" + std::to_string(difference.clears) + " clears, " + std::to_string(Declined(difference)) + " declined)");
}

// A PM4 submit through the driver: its device holds no depth surface (the harness' draws are another VkDevice's), so only
// what happens before the plane is looked up can be seen: the matching of the kernel with the registers the packets set,
// the scope, the colour read. The counters say whether Driver::dispatch got that far.
void DriverMatchCase(AgcDriver::VulkanDevice& device, const Shaders& shaders, int mode) {
    Graphics::SetDepthClearModeForTest(mode);
    const auto before = Graphics::DepthClearCounts();
    const std::string label = "driver submit, mode " + std::to_string(mode) + ": ";
    auto& code = NewBlock();
    std::memcpy(code.Data(), ClearKernel.data(), sizeof(ClearKernel));
    auto& plane = NewBlock();
    auto& color = NewBlock();
    const Surface surface{plane.Address(), 0, 0, false, VK_FORMAT_D32_SFLOAT};
    Require(Draw(device, shaders, surface, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}) == Covered, label + "the producer did not cover the plane");
    SetColor(color, 1.0f);
    DispatchClear(code, plane, color, Format32Float, 8, 4);
    const auto difference = Difference(Graphics::DepthClearCounts(), before);
    if (mode == 0) Require(difference.matched == 0u && difference.taken == 0u, label + "the driver's dispatch reached the lever with the switch off");
    else Require(difference.matched == 1u && difference.taken == 1u, label + "the driver's dispatch matched " + std::to_string(difference.matched) + " and took " + std::to_string(difference.taken) + " clears (declined " + std::to_string(Declined(difference)) + "), expected 1 and 1");
}

// Reaps the completions of recorded work until `done` holds (the checks of verify mode run when their batch completes).
template<typename TDone>
void AwaitCompletions(AgcDriver::VulkanDevice& device, TDone done) {
    for (int attempt = 0; attempt < 150 && !done(); ++attempt) {
        device.WaitIdle();
        AgcDriverWaitIdle_nid_postfix();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// A 16-bit plane, which the title's shadow atlases are: an R16 unorm view cleared to a colour of 0 and 1 (0.5 is 32767.5 in
// 16 bits, no value to take), and an R16 uint view with the depth swizzle cleared to raw values. Verify mode compares every
// texel of the mirror and of the depth image with the raw value, so the round trip through raw / 65535 is exact there.
void SixteenBitCase(AgcDriver::VulkanDevice& device, const Shaders& shaders, int mode) {
    Graphics::SetDepthClearModeForTest(mode);
    const auto before = Graphics::DepthClearCounts();
    const std::string label = "16-bit plane, mode " + std::to_string(mode) + ": ";
    auto& code = NewBlock();
    std::memcpy(code.Data(), ClearKernel.data(), sizeof(ClearKernel));
    auto& plane = NewBlock();
    auto& color = NewBlock();
    const Surface surface{plane.Address(), 0, 0, false, VK_FORMAT_D16_UNORM};
    const auto drawLess = [&] { return Draw(device, shaders, surface, {.depthCompare = VK_COMPARE_OP_LESS}); };
    const auto drawGreater = [&] { return Draw(device, shaders, surface, {.depthCompare = VK_COMPARE_OP_GREATER}); };
    Require(Draw(device, shaders, surface, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}) == Covered, label + "the producer did not cover the plane");

    SetColor(color, 1.0f);
    try {
        RunClear(device, code, plane, color, Format16UNorm, 8, 4);
    } catch (const std::exception& error) {
        // A device without 16-bit unorm storage images cannot run the baseline either: nothing to compare.
        if (std::string(error.what()).find("unsupported format") == std::string::npos) throw;
        std::printf("16-bit plane skipped: %s\n", error.what());
        return;
    }
    Require(drawLess() == Covered, label + "after a clear to 1.0 the depth test 0.5 < 1.0 did not pass everywhere");
    Require(drawGreater() == 0u, label + "after a clear to 1.0 a texel passed 0.5 > depth");

    SetColor(color, 0.0f);
    RunClear(device, code, plane, color, Format16UNorm, 8, 4);
    Require(drawGreater() == Covered, label + "after a clear to 0.0 the depth test 0.5 > 0.0 did not pass everywhere");
    Require(drawLess() == 0u, label + "after a clear to 0.0 a texel passed 0.5 < depth");

    SetColor(color, 0.5f);
    RunClear(device, code, plane, color, Format16UNorm, 8, 4);

    // Another plane for the uint view: one mirror format per plane, as a title has it.
    auto& rawPlane = NewBlock();
    const Surface rawSurface{rawPlane.Address(), 0, 0, false, VK_FORMAT_D16_UNORM};
    const auto rawLess = [&] { return Draw(device, shaders, rawSurface, {.depthCompare = VK_COMPARE_OP_LESS}); };
    const auto rawGreater = [&] { return Draw(device, shaders, rawSurface, {.depthCompare = VK_COMPARE_OP_GREATER}); };
    Require(Draw(device, shaders, rawSurface, {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}) == Covered, label + "the producer of the uint plane did not cover it");
    SetRaw(color, 0x4000u);
    RunClear(device, code, rawPlane, color, Format16Uint, 8, 4, TileZ64KBX);
    Require(rawGreater() == Covered, label + "after a clear to the raw value 0x4000 the depth test 0.5 > 0.25 did not pass everywhere");
    Require(rawLess() == 0u, label + "after a clear to the raw value 0x4000 a texel passed 0.5 < depth");

    SetRaw(color, 0xffffu);
    RunClear(device, code, rawPlane, color, Format16Uint, 8, 4, TileZ64KBX);
    Require(rawLess() == Covered, label + "after a clear to the raw value 0xffff the depth test 0.5 < 1.0 did not pass everywhere");
    Require(rawGreater() == 0u, label + "after a clear to the raw value 0xffff a texel passed 0.5 > depth");

    if (mode >= 2) AwaitCompletions(device, [&] { return Graphics::DepthClearCounts().verified + Graphics::DepthClearCounts().violations >= before.verified + before.violations + 4u; });
    const auto difference = Difference(Graphics::DepthClearCounts(), before);
    if (mode == 0) {
        Require(difference.matched == 0u && difference.clears == 0u, label + "the lever counted with the switch off");
        return;
    }
    Require(difference.clears == 4u, label + "the lever took " + std::to_string(difference.clears) + " clears, expected 4 (unorm 1.0 and 0.0, raw 0x4000 and 0xffff)");
    Require(difference.declined[2] >= 1u, label + "the unorm clear to 0.5 was not declined for its value (" + std::to_string(difference.declined[2]) + ")");
    Require(difference.bytesSkipped == difference.clears * 2u * Width * Height * sizeof(std::uint16_t), label + "the copies left out add up to " + std::to_string(difference.bytesSkipped) + " bytes");
    if (mode >= 2) {
        Require(difference.violations == 0u, label + "verify mode found " + std::to_string(difference.violations) + " violations");
        Require(difference.verified == 4u, label + "verify mode checked " + std::to_string(difference.verified) + " clears, expected 4");
    }
}

}

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto&& body) {
        try {
            body();
        } catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
    };
    try {
        run("matcher", [] { MatcherCases(); });
        const auto device = OpenVulkanTestDevice();
        if (!device) return failures != 0 ? 1 : VulkanTestSkipped;
        const auto shaders = Compile(*device);

        run("switch off", [&] {
            const auto off = ClearSequence(*device, shaders, 0);
            Require(off.matched == 0u && off.taken == 0u && off.clears == 0u && off.copiesSkipped == 0u, "the lever counted with the switch off");
        });

        run("lever", [&] {
            const auto on = ClearSequence(*device, shaders, 1);
            // Six dispatches match the kernel: the clears to 1.0, 0.25, 0.5 and 0.125 are taken, the one of half the plane and the one to 2.0 are not.
            Require(on.matched >= 6u, "the lever matched " + std::to_string(on.matched) + " clear dispatches, expected 6");
            Require(on.clears == 4u, "the lever took " + std::to_string(on.clears) + " clears, expected 4 (1.0, 0.25, 0.5, 0.125)");
            Require(on.copiesSkipped >= on.clears, "fewer copy-ins were left out than clears taken");
            Require(on.bytesSkipped == on.clears * 2u * Width * Height * sizeof(float), "the copies left out add up to " + std::to_string(on.bytesSkipped) + " bytes");
            Require(on.declined[2] >= 1u, "the clear to 2.0 was not declined for its value (" + std::to_string(on.declined[2]) + ")");
            Require(on.declined[5] >= 1u, "the clear of half the plane was not declined for its coverage (" + std::to_string(on.declined[5]) + ")");
        });

        run("verify", [&] {
            const auto before = Graphics::DepthClearCounts();
            const auto checked = ClearSequence(*device, shaders, 2);
            Require(checked.clears == 4u, "verify mode took " + std::to_string(checked.clears) + " clears, expected 4");
            AwaitCompletions(*device, [&] { return Graphics::DepthClearCounts().verified + Graphics::DepthClearCounts().violations >= before.verified + before.violations + 4u; });
            const auto settled = Difference(Graphics::DepthClearCounts(), before);
            Require(settled.violations == 0u, "verify mode found " + std::to_string(settled.violations) + " violations in the lever's clears");
            Require(settled.verified == 4u, "verify mode checked " + std::to_string(settled.verified) + " clears, expected 4");
        });

        // A check that cannot fail proves nothing: with the expected value perturbed it must report.
        run("verify perturbed", [&] {
            const auto before = Graphics::DepthClearCounts();
            ClearSequence(*device, shaders, 2, 0x1u);
            AwaitCompletions(*device, [&] { return Graphics::DepthClearCounts().violations >= before.violations + 4u; });
            const auto reported = Graphics::DepthClearCounts().violations - before.violations;
            Require(reported == 4u, "verify mode with a perturbed expectation reported " + std::to_string(reported) + " violations, expected 4");
        });
        Graphics::SetDepthClearModeForTest(-1);

        for (const int mode : {0, 1}) run(mode == 0 ? "integer view, switch off" : "integer view, lever", [&] { IntegerViewCase(*device, shaders, mode); });
        for (const int mode : {0, 1, 2}) run(mode == 0 ? "16-bit plane, switch off" : mode == 1 ? "16-bit plane, lever" : "16-bit plane, verify", [&] { SixteenBitCase(*device, shaders, mode); });
        for (const int mode : {0, 1}) run(mode == 0 ? "driver submit, switch off" : "driver submit, lever", [&] { DriverMatchCase(*device, shaders, mode); });
        Graphics::SetDepthClearModeForTest(-1);
        LibcRunShutdown_nid_postfix();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
    if (failures != 0) return 1;
    std::puts("depth clear tests passed");
    return 0;
}
