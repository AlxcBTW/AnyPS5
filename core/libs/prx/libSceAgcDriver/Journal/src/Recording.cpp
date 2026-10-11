#include "prx/libSceAgcDriver/Journal/include/Recording.hpp"
#include "prx/libSceAgcDriver/Journal/include/Format.hpp"
#include "prx/libSceAgcDriver/Journal/include/Readback.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
extern char** environ;
#endif

namespace AgcDriver::Journal {

std::atomic<bool> armed{false};

namespace {

constexpr std::size_t LineBytes = 64;

void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error("frame journal: " + reason);
}

std::string hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::uint64_t pointer(const void* value) {
    return reinterpret_cast<std::uint64_t>(value);
}

std::vector<ShaderRegister> copied(std::span<const ShaderRegister> registers) {
    return {registers.begin(), registers.end()};
}

std::vector<std::pair<std::string, std::string>> journaledEnvironment() {
    std::vector<std::pair<std::string, std::string>> result;
    const auto add = [&](std::string_view entry) {
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos || equals == 0) return;
        const auto name = entry.substr(0, equals);
        if (!name.starts_with("APS5_") && !name.starts_with("ANYPS5_")) return;
        result.emplace_back(std::string(name), std::string(entry.substr(equals + 1)));
    };
#ifdef _WIN32
    const auto block = GetEnvironmentStringsA();
    require(block != nullptr, "cannot read the process environment");
    for (const char* entry = block; *entry != '\0'; entry += std::strlen(entry) + 1) add(entry);
    FreeEnvironmentStringsA(block);
#else
    for (char** entry = environ; *entry != nullptr; ++entry) add(*entry);
#endif
    std::sort(result.begin(), result.end());
    return result;
}

bool sharedBacking(std::uint64_t address, std::uint64_t bytes) {
#ifdef _WIN32
    for (auto cursor = address; cursor < address + bytes;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) return false;
        if (info.State == MEM_COMMIT && info.Type == MEM_MAPPED) return true;
        cursor = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return false;
#else
    int file = -1;
    std::uint64_t offset = 0;
    return GuestArena::GuestArenaSharedBacking_nid_postfix(address, bytes, &file, &offset);
#endif
}

std::uint64_t availableMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    require(GlobalMemoryStatusEx(&status) != 0, "cannot query the available memory");
    return status.ullAvailPhys;
#else
    std::ifstream meminfo("/proc/meminfo");
    std::string name;
    std::uint64_t kib = 0;
    std::string unit;
    while (meminfo >> name >> kib >> unit) {
        if (name == "MemAvailable:") return kib << 10u;
    }
    throw std::runtime_error("frame journal: /proc/meminfo has no MemAvailable line");
#endif
}

class Shadow {
public:
    void Load(const std::vector<Range>& ranges) {
        regions.clear();
        for (const auto& range : ranges) {
            for (const auto& [begin, end] : range.committed) {
                Region region{begin, std::vector<std::byte>(end - begin)};
                std::memcpy(region.bytes.data(), reinterpret_cast<const void*>(begin), region.bytes.size());
                regions.push_back(std::move(region));
            }
        }
    }

    template <typename TVisit>
    void ForEach(TVisit&& visit) const {
        for (const auto& region : regions) visit(region.address, std::span<const std::byte>(region.bytes));
    }

    std::vector<TitleWrite> Changes() const {
        std::vector<TitleWrite> writes;
        for (const auto& region : regions) {
            const auto* live = reinterpret_cast<const std::byte*>(region.address);
            const auto size = region.bytes.size();
            const auto differs = [&](std::size_t at) { return std::memcmp(live + at, region.bytes.data() + at, std::min(LineBytes, size - at)) != 0; };
            for (std::size_t at = 0; at < size;) {
                if (!differs(at)) {
                    at += std::min(LineBytes, size - at);
                    continue;
                }
                auto end = at;
                while (end < size && differs(end)) end += std::min(LineBytes, size - end);
                writes.push_back({region.address + at, std::vector<std::byte>(live + at, live + end)});
                at = end;
            }
        }
        return writes;
    }

    void Refresh() {
        for (auto& region : regions) std::memcpy(region.bytes.data(), reinterpret_cast<const void*>(region.address), region.bytes.size());
    }

private:
    struct Region {
        std::uint64_t address;
        std::vector<std::byte> bytes;
    };
    std::vector<Region> regions;
};

class Recording {
public:
    static Recording& Get() {
        static Recording recording;
        return recording;
    }

    template <typename TCall, typename TForward>
    void Note(TCall&& make, bool prelude, TForward&& forward) {
        std::lock_guard lock(mutex);
        if (!open.load(std::memory_order_relaxed)) {
            forward();
            if (prelude) journal.prelude.push_back(make());
            return;
        }
        JournaledCall entry{make(), {}, {}};
        requireJournaled(entry.call);
        require(GuestAllocations::GuestAllocationsGeneration_nid_postfix() == registryGeneration, "the registered guest memory changed while the journal was open; journaling memory registration is not implemented");
        entry.writes = shadow.Changes();
        {
            std::lock_guard flipLock(flipMutex);
            flips.clear();
        }
        forward();
        Settle(journal.ranges);
        {
            std::lock_guard flipLock(flipMutex);
            entry.flips = std::exchange(flips, {});
        }
        for (const auto& flip : entry.flips) journal.references.push_back(reference(flip));
        shadow.Refresh();
        if (prelude) journal.prelude.push_back(entry.call);
        journal.calls.push_back(std::move(entry));
    }

    void NoteFlip(const FlipInfo& info) {
        if (!open.load(std::memory_order_acquire)) return;
        std::lock_guard lock(flipMutex);
        flips.push_back(info);
    }

    void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);
    void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);

    void NoteDisplayBuffer(std::uint32_t handle, std::int32_t index, const DisplayBuffer& buffer) {
        Note([&] { return DisplayBufferCall{handle, index, buffer}; }, true, [&] { displayBuffers[{handle, index}] = buffer; });
    }

    void Open(const std::filesystem::path& directory) {
        std::lock_guard lock(mutex);
        openLocked(directory);
    }

    void Close() {
        std::lock_guard lock(mutex);
        closeLocked();
    }

private:
    void openLocked(const std::filesystem::path& directory) {
        require(!open.load(std::memory_order_relaxed), "a journal is already open");
        auto& driver = DriverDetail::Driver::Get();
        for (const auto handle : driver.VideoOutputHandles()) require(outputs.contains(handle), "video output " + std::to_string(handle) + " was registered before the journal was armed");
        require(driver.WaitQuiescent(), "a submission waits for a title write; the journal opens only when every accepted submission has completed");
        const auto gds = Pm4::GdsAddress();
        JournalFile next;
        next.prelude = journal.prelude;
        next.environment = journaledEnvironment();
        std::uintptr_t arenaBase = 0;
        std::size_t arenaBytes = 0;
        GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
        next.arenaBase = arenaBase;
        next.arenaBytes = arenaBytes;
        next.arenaWriteWatched = GuestArena::GuestArenaWriteWatched_nid_postfix();
        registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        std::vector<Range> shared;
        std::uint64_t committedBytes = 0;
        for (const auto& range : GuestAllocations::GuestAllocationsAcquireAll_nid_postfix()) {
            if (!range->readable) continue;
            Range entry{range->address, range->bytes, range->readable, range->writable, range->gpu, range->allocationAddress, range->allocationBytes, 0, 0, GuestMemory::CommittedRanges(range->address, range->bytes)};
            for (const auto& [begin, end] : entry.committed) committedBytes += end - begin;
            if (sharedBacking(range->address, range->bytes)) shared.push_back(entry);
            next.ranges.push_back(std::move(entry));
        }
        requireUnshared(shared);
        requireMemoryFor(committedBytes);
        std::sort(next.ranges.begin(), next.ranges.end(), [](const Range& a, const Range& b) { return a.address < b.address; });
        Settle(next.ranges);
        GuestMemory::FlushGpuWrites(gds, Pm4::GdsBytes);
        auto queues = driver.Checkpoint();
        next.checkpoint.queues = std::move(queues.queues);
        next.checkpoint.resetGraphics = queues.resetGraphics;
        next.checkpoint.gds.resize(Pm4::GdsBytes);
        std::memcpy(next.checkpoint.gds.data(), reinterpret_cast<const void*>(gds), Pm4::GdsBytes);
        journal = std::move(next);
        for (const auto& call : journal.prelude) requireJournaled(call);
        std::filesystem::create_directories(directory);
        shadow.Load(journal.ranges);
        BlockWriter blocks(directory);
        shadow.ForEach([&](std::uint64_t address, std::span<const std::byte> bytes) {
            for (std::size_t at = 0; at < bytes.size();) {
                const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size() - at, ImageBlockBytes - (address + at) % ImageBlockBytes));
                const auto block = bytes.subspan(at, length);
                if (std::any_of(block.begin(), block.end(), [](std::byte value) { return value != std::byte{0}; })) journal.image.push_back({address + at, blocks.Store(block), static_cast<std::uint32_t>(length)});
                at += length;
            }
        });
        this->directory = directory;
        open.store(true, std::memory_order_release);
    }

    void closeLocked() {
        require(open.load(std::memory_order_relaxed), "no journal is open");
        open.store(false, std::memory_order_release);
        WriteJournal(directory, journal);
        auto prelude = std::move(journal.prelude);
        journal = {};
        journal.prelude = std::move(prelude);
        shadow = {};
    }

    static void requireUnshared(const std::vector<Range>& shared) {
        if (shared.empty()) return;
        std::uint64_t bytes = 0;
        for (const auto& range : shared) bytes += range.bytes;
        std::string first;
        for (std::size_t index = 0; index < shared.size() && index < 4; ++index) first += (index == 0 ? "" : ", ") + hex(shared[index].address) + "+" + hex(shared[index].bytes);
        throw std::runtime_error("frame journal: " + std::to_string(shared.size()) + " registered ranges (" + std::to_string(bytes >> 20) + " MiB, first " + first + ") are mapped from shared backing that other addresses can alias; journaling alias groups is not implemented");
    }

    static void requireMemoryFor(std::uint64_t imageBytes) {
        constexpr std::uint64_t Headroom = std::uint64_t{4} << 30;
        const auto available = availableMemory();
        require(available >= imageBytes + Headroom, "the memory image needs " + std::to_string(imageBytes >> 20) + " MiB in this process, " + std::to_string(available >> 20) + " MiB are available and " + std::to_string(Headroom >> 20) + " MiB stay free");
    }

    bool journaled(std::uint64_t address, std::uint64_t bytes) const {
        const auto after = std::upper_bound(journal.ranges.begin(), journal.ranges.end(), address, [](std::uint64_t value, const Range& range) { return value < range.address; });
        if (after == journal.ranges.begin()) return false;
        const auto& range = *std::prev(after);
        return address - range.address <= range.bytes && bytes <= range.bytes - (address - range.address);
    }

    void requireShader(std::uint64_t address) const {
        if (address == 0) return;
        require(journaled(address, sizeof(Shader)), "shader header " + hex(address) + " lies outside the journaled guest memory");
        const auto* shader = reinterpret_cast<const Shader*>(address);
        const auto code = pointer(const_cast<const void*>(shader->code));
        require(journaled(code, shader->shader_size), "the code " + hex(code) + "+" + hex(shader->shader_size) + " of shader " + hex(address) + " lies outside the journaled guest memory");
    }

    void requireJournaled(const Call& call) const {
        if (const auto* item = std::get_if<RegisterShaderCall>(&call)) {
            requireShader(item->shader);
        } else if (const auto* item = std::get_if<ResolveShaderAbiCall>(&call)) {
            requireShader(item->shader);
        } else if (const auto* item = std::get_if<ResolveGraphicsAbiCall>(&call)) {
            requireShader(item->vertex);
            requireShader(item->pixel);
        } else if (const auto* item = std::get_if<ResolveGraphicsStagesAbiCall>(&call)) {
            for (const auto stage : item->stages) requireShader(stage);
        }
    }

    Frame reference(const FlipInfo& flip) {
        if (flip.index < 0) return {};
        const auto buffer = displayBuffers.find({flip.handle, flip.index});
        require(buffer != displayBuffers.end(), "a flip shows display buffer " + std::to_string(flip.index) + " of video output " + std::to_string(flip.handle) + ", which the journal has no display buffer for");
        return ReadFrame(buffer->second);
    }

    std::mutex mutex;
    std::atomic<bool> open{false};
    std::filesystem::path directory;
    JournalFile journal;
    Shadow shadow;
    std::uint64_t registryGeneration = 0;
    std::map<std::pair<std::uint32_t, std::int32_t>, DisplayBuffer> displayBuffers;
    std::map<std::uint32_t, std::pair<std::shared_ptr<IVideoOutput>, std::shared_ptr<IVideoOutput>>> outputs;
    std::mutex flipMutex;
    std::vector<FlipInfo> flips;
};

class JournaledOutput final : public IVideoOutput {
public:
    explicit JournaledOutput(std::shared_ptr<IVideoOutput> output) : output(std::move(output)) {}
    std::shared_ptr<IFlipRequest> Reserve(const FlipInfo& info) override {
        auto request = output->Reserve(info);
        Recording::Get().NoteFlip(info);
        return request;
    }
    void WaitForFlipRoom() override { output->WaitForFlipRoom(); }
    std::shared_ptr<IRenderingWait> CaptureRenderingWait(std::uint32_t index) override { return output->CaptureRenderingWait(index); }
    void Fail(std::exception_ptr error) noexcept override { output->Fail(error); }

private:
    std::shared_ptr<IVideoOutput> output;
};

void Recording::RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    const std::shared_ptr<IVideoOutput> journaled = output == nullptr ? nullptr : std::make_shared<JournaledOutput>(output);
    Note([&] { return VideoOutputCall{handle, true}; }, true, [&] {
        DriverDetail::Driver::Get().RegisterVideoOutput(handle, journaled);
        outputs[handle] = {output, journaled};
    });
}

void Recording::UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    Note([&] { return VideoOutputCall{handle, false}; }, true, [&] {
        const auto found = outputs.find(handle);
        const bool wrapped = found != outputs.end() && found->second.first == output;
        DriverDetail::Driver::Get().UnregisterVideoOutput(handle, wrapped ? found->second.second : output);
        if (wrapped) outputs.erase(found);
    });
}

SubmitCall submitted(const Packet* packet, std::uint32_t queue) {
    GuestMemory::CheckRange(packet, sizeof(Packet), alignof(Packet));
    SubmitCall call{queue, pointer(packet->addr), packet->flags, {}};
    if (packet->dw_num != 0) {
        GuestMemory::CheckRange(packet->addr, static_cast<std::size_t>(packet->dw_num) * sizeof(std::uint32_t), alignof(std::uint32_t));
        call.words.assign(packet->addr, packet->addr + packet->dw_num);
    }
    return call;
}

}

void Arm() {
    armed.store(true, std::memory_order_release);
}

void Open(const std::filesystem::path& directory) {
    require(Armed(), "the journal must be armed before it is opened");
    Recording::Get().Open(directory);
}

void Close() {
    Recording::Get().Close();
}

void NoteDisplayBuffer(std::uint32_t handle, std::int32_t index, const DisplayBuffer& buffer) {
    if (!Armed()) return;
    Recording::Get().NoteDisplayBuffer(handle, index, buffer);
}

void Submit(const Packet* packet, std::uint32_t queue) {
    Recording::Get().Note([&] { return submitted(packet, queue); }, false, [&] { DriverDetail::Driver::Get().Submit(packet, queue); });
}

void SuspendPoint() {
    Recording::Get().Note([] { return SuspendCall{}; }, false, [] { DriverDetail::Driver::Get().SuspendPoint(); });
}

void RegisterShader(const Shader* shader) {
    Recording::Get().Note([&] { return RegisterShaderCall{pointer(shader)}; }, true, [&] { DriverDetail::Driver::Get().RegisterShader(shader); });
}

void ResolveShaderAbi(const Shader* shader, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
    Recording::Get().Note([&] { return ResolveShaderAbiCall{pointer(shader), copied(context), copied(primitive)}; }, true, [&] { DriverDetail::Driver::Get().ResolveShaderAbi(shader, context, primitive); });
}

void ResolveGraphicsStagesAbi(std::span<const Shader* const> stages, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
    const auto make = [&] {
        ResolveGraphicsStagesAbiCall call{{}, copied(context), copied(primitive)};
        for (const auto* stage : stages) call.stages.push_back(pointer(stage));
        return call;
    };
    Recording::Get().Note(make, true, [&] { DriverDetail::Driver::Get().ResolveGraphicsStagesAbi(stages, context, primitive); });
}

void ResolveGraphicsAbi(const Shader* vertex, const Shader* pixel, std::uint32_t primitiveType) {
    Recording::Get().Note([&] { return ResolveGraphicsAbiCall{pointer(vertex), pointer(pixel), primitiveType}; }, true, [&] { DriverDetail::Driver::Get().ResolveGraphicsAbi(vertex, pixel, primitiveType); });
}

void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    Recording::Get().RegisterVideoOutput(handle, output);
}

void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    Recording::Get().UnregisterVideoOutput(handle, output);
}

}

extern "C" void AgcDriverJournalDisplayBuffer_nid_postfix(std::uint32_t handle, std::int32_t index, const AgcDriver::DisplayBuffer& buffer) {
    AgcDriver::Journal::NoteDisplayBuffer(handle, index, buffer);
}
