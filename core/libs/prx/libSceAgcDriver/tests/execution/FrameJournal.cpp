#include "prx/libSceAgcDriver/Journal/include/Journal.hpp"
#include "prx/libSceAgcDriver/Journal/include/Format.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderPreparationScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Decoder/Png.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Journal::Frame;

constexpr std::uintptr_t TitleBase = 0x1580000000ull;
constexpr std::uintptr_t OutsideArena = 0x600000000000ull;
constexpr std::size_t RegionBytes = 0x10000;
constexpr std::uint32_t PageReadWrite = 0x04;
constexpr std::uint32_t Handle = 7;
constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 4;
constexpr std::uint32_t GdsPattern = 0x11223344u;
constexpr std::uint32_t GdsBytes = 64;

constexpr std::array<std::uint32_t, 8> VertexCode{0x7e000208u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u};
constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
constexpr std::array<std::uint32_t, 69> FillCode{
    0xbfa00003u, 0xd7460002u, 0x04010c0au, 0x7da80408u, 0xbf88003fu, 0x7e000c09u, 0xbf070980u, 0x858a807eu,
    0x7e005700u, 0x100000ffu, 0x4f800000u, 0x7e060f00u, 0xd5766a00u, 0x02020609u, 0x7d8a0280u, 0x4c020080u,
    0x02000101u, 0xd56a0001u, 0x00020700u, 0x4c000303u, 0x4a020303u, 0x02000101u, 0xd56a0000u, 0x00020500u,
    0xd5690001u, 0x00020009u, 0x4c060302u, 0x7d860609u, 0x7d8c02f9u, 0x06068c02u, 0x87ea6a0cu, 0x50000080u,
    0xd5286a00u, 0x003200c1u, 0xd5010000u, 0x002a00c1u, 0xd5690000u, 0x00020009u, 0x4c000102u, 0x7d0a0080u,
    0xbe88246au, 0xbf880015u, 0x7d0a0081u, 0xbe8a246au, 0xbf88000cu, 0x7d0a0082u, 0xbeea246au, 0xbf880003u,
    0x7e000207u, 0xe0102000u, 0x80000002u, 0x8afe7e6au, 0xbf880003u, 0x7e000206u, 0xe0102000u, 0x80000002u,
    0xbefe046au, 0x8afe7e0au, 0xbf880003u, 0x7e000205u, 0xe0102000u, 0x80000002u, 0xbefe040au, 0x8afe7e08u,
    0xbf880003u, 0x7e000204u, 0xe0102000u, 0x80000002u, 0xbf810000u};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

constexpr std::size_t VertexHeaderOffset = 0x0;
constexpr std::size_t PixelHeaderOffset = 0x100;
constexpr std::size_t VertexCodeOffset = 0x1000;
constexpr std::size_t PixelCodeOffset = 0x1100;
constexpr std::size_t FillCodeOffset = 0x1200;
constexpr std::size_t LabelOffset = 0xf000;
constexpr std::size_t SetupOffset = 0x4000;
constexpr std::size_t FrameOffset = 0x8000;

class Region {
public:
    Region() : data(static_cast<std::byte*>(GuestArena::GuestArenaAllocateAtOrAbove_nid_postfix(TitleBase, RegionBytes, RegionBytes))) {
        GuestArena::GuestArenaCommit_nid_postfix(data, RegionBytes, PageReadWrite, RegionBytes);
#ifndef _WIN32
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, RegionBytes);
#endif
        GuestAllocations::Mutation mutation;
        mutation.Add(data, RegionBytes, true, true, true);
    }
    explicit Region(std::uintptr_t fixed) : data(reinterpret_cast<std::byte*>(fixed)) {
#ifdef _WIN32
        Require(VirtualAlloc(data, RegionBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == data, "cannot map the command region outside the guest arena");
#else
        Require(mmap(data, RegionBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == data, "cannot map the command region outside the guest arena");
#endif
        GuestAllocations::Mutation mutation;
        mutation.Add(data, RegionBytes, true, true, false);
    }
    std::uint64_t Address(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(data) + offset; }
    void Release(std::size_t offset) { std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(data + offset)).store(1, std::memory_order_release); }
    template <typename TValue, std::size_t Count>
    const TValue* Store(std::size_t offset, const std::array<TValue, Count>& values) {
        std::memcpy(data + offset, values.data(), sizeof(values));
        return reinterpret_cast<const TValue*>(data + offset);
    }
    Packet Commands(std::size_t offset, const std::vector<std::uint32_t>& words) {
        std::memcpy(data + offset, words.data(), words.size() * sizeof(std::uint32_t));
        return Packet{reinterpret_cast<std::uint32_t*>(data + offset), static_cast<std::uint32_t>(words.size()), 0, {}};
    }
    Header& HeaderAt(std::size_t offset) { return *std::construct_at(reinterpret_cast<Header*>(data + offset)); }

private:
    std::byte* data;
};

class Request final : public AgcDriver::IFlipRequest {
public:
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>&) override {}
    void Fail(std::exception_ptr) noexcept override {}
};

class Output final : public AgcDriver::IVideoOutput {
public:
    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo&) override { return std::make_shared<Request>(); }
    void Fail(std::exception_ptr) noexcept override {}
};

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void Registers(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::uint32_t first, std::initializer_list<std::uint32_t> values) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(values.size()) << 16u) | (opcode << 8u));
    commands.push_back(first);
    commands.insert(commands.end(), values);
}

void Register(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void RegisterDuringPreparation(const std::function<void()>& outside, const std::function<void()>& inside, const std::string& phase) {
    std::promise<void> holding;
    std::promise<void> proceed;
    auto preparing = std::async(std::launch::async, [&, go = proceed.get_future()] {
        AgcDriver::ShaderPreparationScope transaction;
        holding.set_value();
        go.wait();
        inside();
        transaction.Commit();
    });
    holding.get_future().wait();
    auto registering = std::async(std::launch::async, outside);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    proceed.set_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    if (preparing.wait_until(deadline) != std::future_status::ready || registering.wait_until(deadline) != std::future_status::ready) {
        std::cerr << "a shader registration " << phase << " deadlocked against a shader preparation transaction that another thread held while it registered a shader" << std::endl;
        std::_Exit(1);
    }
    preparing.get();
    registering.get();
}

void Context(std::vector<std::uint32_t>& commands, std::uint64_t color) {
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, ((Width - 1u) << 14u) | (Height - 1u)},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
    };
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
}

void Fill(std::vector<std::uint32_t>& commands, std::uint64_t program, std::uint64_t target, const std::array<std::uint32_t, 4>& pattern) {
    Registers(commands, 0x76, 0x207, {64, 1, 1});
    Registers(commands, 0x76, 0x20c, {static_cast<std::uint32_t>(program >> 8u), static_cast<std::uint32_t>(program >> 40u)});
    Registers(commands, 0x76, 0x213, {(10u << 1u) | (1u << 7u)});
    Registers(commands, 0x76, 0x240, {static_cast<std::uint32_t>(target), static_cast<std::uint32_t>((target >> 32u) & 0xffffu) | (4u << 16u), Width * Height, 0x00014004u, pattern[0], pattern[1], pattern[2], pattern[3], Width * Height, 4});
    Append(commands, 0x15, {4, 1, 1, 0x41});
}

void Draw(std::vector<std::uint32_t>& commands, std::uint64_t vertex, std::uint64_t pixel, float red) {
    for (const auto& [offset, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{
             {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 1u << 1u}, {0x8c, 0},
             {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u}, {0x00c, std::bit_cast<std::uint32_t>(red)}}) {
        Append(commands, 0x76, {offset, value});
    }
    Append(commands, 0x2d, {3, 2});
}

void GdsWrite(std::vector<std::uint32_t>& commands) {
    Append(commands, 0x50, {0x40100000u, GdsPattern, 0, 0, 0, GdsBytes});
}

void GdsCopy(std::vector<std::uint32_t>& commands, std::uint64_t target) {
    Append(commands, 0x50, {0x20000000u, 0, 0, static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(target >> 32u), GdsBytes});
}

void WaitForTitle(std::vector<std::uint32_t>& commands, std::uint64_t label) {
    Append(commands, 0x3c, {0x13, static_cast<std::uint32_t>(label), static_cast<std::uint32_t>(label >> 32u), 1, 0xffffffffu, 0x19});
}

void Flip() {
    std::array<std::uint32_t, 6> words{AgcDriver::FlipPacketHeader, Handle, 0, 1, 0, 0};
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the flip submission failed");
}

void Submit(const Packet& packet) {
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "a submission failed");
}

AgcDriver::DisplayBuffer Scanout(const Region& target) {
    return {target.Address(), 0x8000000022000000ull, Width, Height, 1, 0, 0, 0};
}

struct Live {
    std::vector<Frame> frames;
    std::uint64_t label = 0;
    std::uint64_t fill = 0;
};

Live Record(const std::filesystem::path& journal) {
    AgcDriver::Journal::Arm();
    Region code;
    Region target;
    Region commands(OutsideArena);
    const auto output = std::make_shared<Output>();
    AgcDriverRegisterVideoOutput_nid_postfix(Handle, output);
    AgcDriver::Journal::NoteDisplayBuffer(Handle, 0, Scanout(target));
    const auto* vertexCode = code.Store(VertexCodeOffset, VertexCode);
    const auto* pixelCode = code.Store(PixelCodeOffset, PixelCode);
    code.Store(FillCodeOffset, FillCode);
    auto& vertexHeader = code.HeaderAt(VertexHeaderOffset);
    auto& pixelHeader = code.HeaderAt(PixelHeaderOffset);
    RegisterDuringPreparation([&] { Register(vertexHeader, vertexCode, sizeof(VertexCode), 2); }, [&] { Register(pixelHeader, pixelCode, sizeof(PixelCode), 1); }, "before the journal opened");
    std::vector<std::uint32_t> setup;
    Context(setup, target.Address());
    GdsWrite(setup);
    Submit(commands.Commands(SetupOffset, setup));
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::Journal::Open(journal);
    RegisterDuringPreparation([&] { AgcDriverRegisterShader_nid_postfix(&vertexHeader.shader); }, [&] { AgcDriverRegisterShader_nid_postfix(&pixelHeader.shader); }, "while the journal was open");
    std::vector<Frame> frames;
    const auto present = [&] {
        Flip();
        AgcDriverWaitIdle_nid_postfix();
        frames.push_back({Width, Height, AgcDriver::ReadDisplayBuffer(Scanout(target))});
    };
    std::vector<std::uint32_t> first;
    Fill(first, code.Address(FillCodeOffset), target.Address(), {0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff808080u});
    Draw(first, code.Address(VertexCodeOffset), code.Address(PixelCodeOffset), 1.0f);
    GdsCopy(first, target.Address());
    Submit(commands.Commands(FrameOffset, first));
    present();
    AgcDriverSuspendPoint_nid_postfix();
    std::vector<std::uint32_t> second;
    WaitForTitle(second, code.Address(LabelOffset));
    Context(second, target.Address());
    Fill(second, code.Address(FillCodeOffset), target.Address(), {0xff204060u, 0xff604020u, 0xff000000u, 0xffffffffu});
    Draw(second, code.Address(VertexCodeOffset), code.Address(PixelCodeOffset), 0.5f);
    Submit(commands.Commands(FrameOffset, second));
    code.Release(LabelOffset);
    present();
    AgcDriver::Journal::Close();
    return {frames, code.Address(LabelOffset), code.Address(FillCodeOffset)};
}

void RequireJournal(const std::filesystem::path& directory, std::uint64_t label) {
    const auto journal = AgcDriver::Journal::ReadJournal(directory);
    const auto shaders = std::count_if(journal.prelude.begin(), journal.prelude.end(), [](const AgcDriver::Journal::Call& call) { return std::holds_alternative<AgcDriver::Journal::RegisterShaderCall>(call); });
    Require(shaders == 2, "the prelude holds " + std::to_string(shaders) + " shader registrations, expected the 2 made before the journal opened");
    Require(journal.calls.size() == 7, "the journal holds " + std::to_string(journal.calls.size()) + " calls, expected 2 shader registrations, 2 frame submissions, 2 flips and a suspend point");
    Require(std::holds_alternative<AgcDriver::Journal::RegisterShaderCall>(journal.calls[0].call) && std::holds_alternative<AgcDriver::Journal::RegisterShaderCall>(journal.calls[1].call), "the shader registrations made while the journal was open are not its first calls");
    Require(std::holds_alternative<AgcDriver::Journal::SubmitCall>(journal.calls[2].call), "the first frame submission does not follow the shader registrations");
    const auto writes = [&](std::size_t index) { return !journal.calls[index].writes.empty(); };
    Require((writes(0) || writes(1) || writes(2)) && writes(5), "no title writes precede the frame submissions whose commands the title stored");
    Require(std::any_of(journal.calls[6].writes.begin(), journal.calls[6].writes.end(), [&](const AgcDriver::Journal::TitleWrite& write) { return write.address <= label && label < write.address + write.bytes.size(); }), "the label the title stored after the second submission is not a title write of the next call");
    Require(std::any_of(journal.ranges.begin(), journal.ranges.end(), [](const AgcDriver::Journal::Range& range) { return range.address == OutsideArena; }), "the command region outside the guest arena is not in the journal");
    const auto queue = journal.checkpoint.queues.find(0);
    Require(queue != journal.checkpoint.queues.end() && queue->second.context.contains(0x318), "the checkpoint lost the colour target register set before the journal opened");
    std::uint32_t gds = 0;
    std::memcpy(&gds, journal.checkpoint.gds.data(), sizeof(gds));
    Require(gds == GdsPattern, "the checkpoint's GDS does not hold the bytes written before the journal opened");
    Require(journal.references.size() == 2, "the journal holds " + std::to_string(journal.references.size()) + " reference frames, expected 2");
}

std::string Quoted(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

std::vector<Frame> Replay(const std::filesystem::path& replayer, const std::filesystem::path& journal, const std::filesystem::path& frames, const std::string& options = {}) {
    std::string command = Quoted(replayer) + " " + Quoted(journal) + " --frames " + Quoted(frames) + options;
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    Require(std::system(command.c_str()) == 0, "the replay of " + journal.string() + " failed or diverged from its reference frames");
    std::vector<Frame> result;
    for (std::size_t index = 0;; ++index) {
        std::ifstream file(frames / ("flip-" + std::to_string(index) + ".png"), std::ios::binary);
        if (!file.is_open()) break;
        const std::vector<std::uint8_t> png{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto image = Decoder::Png::Decode(png);
        Require(image.has_value(), "a replayed frame is not a PNG");
        Frame frame{image->width, image->height, std::vector<std::byte>(image->pixels.size())};
        for (std::size_t at = 0; at + 3 < image->pixels.size(); at += 4) {
            frame.bgra[at] = static_cast<std::byte>(image->pixels[at + 2]);
            frame.bgra[at + 1] = static_cast<std::byte>(image->pixels[at + 1]);
            frame.bgra[at + 2] = static_cast<std::byte>(image->pixels[at]);
            frame.bgra[at + 3] = static_cast<std::byte>(image->pixels[at + 3]);
        }
        result.push_back(std::move(frame));
    }
    return result;
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

void RequireGpuTimes(const std::filesystem::path& path, const Live& recorded) {
    std::ifstream file(path);
    Require(file.is_open(), "the replay wrote no GPU times");
    const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Require(json.find("\"iterations\": 2") != std::string::npos, "the GPU times do not cover both iterations");
    Require(json.find("\"kind\": \"dispatch\", \"queue\": 0, \"program\": \"" + Hex(recorded.fill) + "\"") != std::string::npos, "the GPU times have no entry for the pattern-fill dispatch on queue 0");
    Require(json.find("\"frameMicroseconds\": 0,") == std::string::npos, "the replayed frame took no GPU time");
}

std::uint32_t Pixel(const Frame& frame, std::uint32_t x, std::uint32_t y) {
    std::uint32_t value = 0;
    std::memcpy(&value, frame.bgra.data() + (static_cast<std::size_t>(y) * frame.width + x) * 4, sizeof(value));
    return value;
}

void RequireWorkload(const std::vector<Frame>& live) {
    Require(live.size() == 2 && live[0] != live[1], "the two live frames are not two different images");
    const auto texel = std::bit_cast<std::array<std::uint8_t, 4>>(GdsPattern);
    const auto gds = std::bit_cast<std::uint32_t>(std::array<std::uint8_t, 4>{texel[2], texel[1], texel[0], texel[3]});
    for (std::uint32_t x = 0; x < GdsBytes / 4; ++x) Require(Pixel(live[0], x, 0) == gds, "the first frame does not show the GDS bytes copied over its first row");
    for (const auto& frame : live) {
        std::set<std::uint32_t> colors;
        for (std::uint32_t y = 0; y < Height; ++y) {
            for (std::uint32_t x = 0; x < Width; ++x) colors.insert(Pixel(frame, x, y));
        }
        Require(colors.size() >= 3, "a live frame shows " + std::to_string(colors.size()) + " colors, too few for a fill, a draw and a copy");
    }
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "usage: agc_driver_frame_journal_tests <agc_journal_replay>");
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
#ifdef _WIN32
        const auto process = _getpid();
#else
        const auto process = getpid();
#endif
        const auto directory = std::filesystem::temp_directory_path() / ("agc-frame-journal-" + std::to_string(process));
        std::filesystem::remove_all(directory);
        const auto recorded = Record(directory / "journal");
        const auto& live = recorded.frames;
        AgcDriverShutdown_nid_postfix();
        RequireJournal(directory / "journal", recorded.label);
        RequireWorkload(live);
        const auto first = Replay(argv[1], directory / "journal", directory / "first");
        const auto second = Replay(argv[1], directory / "journal", directory / "second", " --repeat 2 --gpu-times " + Quoted(directory / "gpu-times.json"));
        RequireGpuTimes(directory / "gpu-times.json", recorded);
        Require(first.size() == live.size() && second.size() == live.size(), "a replay presented " + std::to_string(first.size()) + " and " + std::to_string(second.size()) + " frames, the live run " + std::to_string(live.size()));
        Require(first == second, "two replays of one journal are not bit-identical");
        Require(first == live, "the replay differs from the live run's frames");
        std::filesystem::remove_all(directory);
        std::cout << "A journaled fill, draw and GDS copy over two frames, the second waiting for a title write, replayed twice bit-identical to the live run\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdown) {
            std::cerr << "shutdown: " << shutdown.what() << '\n';
        }
        return 1;
    }
}
