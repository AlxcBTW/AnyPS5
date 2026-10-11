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
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

std::vector<std::byte> codeOf(const Shader* shader) {
    const auto* first = reinterpret_cast<const std::byte*>(const_cast<const void*>(shader->code));
    return {first, first + shader->shader_size};
}

ShaderHeader copied(const Shader* shader, std::size_t bytes) {
    if (shader == nullptr) return {};
    const auto* first = reinterpret_cast<const std::byte*>(shader);
    return {pointer(shader), std::vector<std::byte>(first, first + bytes)};
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

std::uint64_t mixWord(std::uint64_t lane, std::uint64_t word) {
    return std::rotl((lane ^ word) * 0x9e3779b97f4a7c15ull, 31) * 0xbf58476d1ce4e5b9ull;
}

std::uint64_t lineHash(const std::byte* line, std::size_t bytes) {
    std::array<std::uint64_t, LineBytes / sizeof(std::uint64_t)> words{};
    static_assert(std::tuple_size_v<decltype(words)> == 8);
    std::memcpy(words.data(), line, std::min(sizeof(words), bytes));
    auto a = mixWord(bytes, words[0]);
    auto b = mixWord(0x9e3779b97f4a7c15ull, words[1]);
    auto c = mixWord(0xbf58476d1ce4e5b9ull, words[2]);
    auto d = mixWord(0x94d049bb133111ebull, words[3]);
    a = mixWord(a, words[4]);
    b = mixWord(b, words[5]);
    c = mixWord(c, words[6]);
    d = mixWord(d, words[7]);
    return a ^ std::rotl(b, 16) ^ std::rotl(c, 32) ^ std::rotl(d, 48);
}

class Shadow {
public:
    static std::uint64_t BytesFor(std::uint64_t imageBytes) { return (imageBytes + LineBytes - 1) / LineBytes * sizeof(std::uint64_t); }

    template <typename TVisit>
    void Load(const std::vector<Range>& ranges, TVisit&& visit) {
        regions.clear();
        for (const auto& range : ranges) {
            for (const auto& [begin, end] : range.committed) regions.push_back({begin, end - begin, std::vector<std::uint64_t>((end - begin + LineBytes - 1) / LineBytes)});
        }
        for (auto& region : regions) hashRegion(region, visit);
    }

    std::vector<TitleWrite> Changes() const {
        std::vector<TitleWrite> writes;
        std::array<std::byte, LineBytes> line;
        for (const auto& region : regions) {
            bool extending = false;
            for (std::uint64_t at = 0; at < region.bytes; at += LineBytes) {
                const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(LineBytes, region.bytes - at));
                std::memcpy(line.data(), reinterpret_cast<const void*>(region.address + at), length);
                if (lineHash(line.data(), length) == region.lines[at / LineBytes]) {
                    extending = false;
                    continue;
                }
                if (!extending) writes.push_back({region.address + at, {}});
                writes.back().bytes.insert(writes.back().bytes.end(), line.begin(), line.begin() + static_cast<std::ptrdiff_t>(length));
                extending = true;
            }
        }
        return writes;
    }

    void Refresh() {
        for (auto& region : regions) hashRegion(region, [](std::uint64_t, std::span<const std::byte>) {});
    }

private:
    struct Region {
        std::uint64_t address;
        std::uint64_t bytes;
        std::vector<std::uint64_t> lines;
    };

    template <typename TVisit>
    void hashRegion(Region& region, TVisit&& visit) {
        std::vector<std::byte> chunk(ImageBlockBytes);
        for (std::uint64_t at = 0; at < region.bytes;) {
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(region.bytes - at, ImageBlockBytes - (region.address + at) % ImageBlockBytes));
            std::memcpy(chunk.data(), reinterpret_cast<const void*>(region.address + at), length);
            for (std::size_t line = 0; line < length; line += LineBytes) region.lines[(at + line) / LineBytes] = lineHash(chunk.data() + line, std::min(LineBytes, length - line));
            visit(region.address + at, std::span<const std::byte>(chunk.data(), length));
            at += length;
        }
    }

    std::vector<Region> regions;
};

class Recording {
public:
    static Recording& Get() {
        static Recording recording;
        return recording;
    }

    template <typename TCall, typename TForward>
    void NoteSubmission(TCall&& make, TForward&& forward) {
        for (;;) {
            if (admit()) {
                const Admitted admitted{admission};
                forward();
                return;
            }
            std::lock_guard lock(mutex);
            if (!open.load(std::memory_order_relaxed)) continue;
            journalLocked(make(), forward);
            return;
        }
    }

    template <typename TCall, typename TForward>
    void NoteRegistration(TCall&& make, TForward&& forward) {
        forward();
        std::lock_guard preludeLock(preludeMutex);
        registeredLocked(make());
    }

    void NoteFlip(const FlipInfo& info) {
        if (!open.load(std::memory_order_acquire)) return;
        std::lock_guard lock(flipMutex);
        flips.push_back(info);
    }

    void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);
    void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);

    void NoteDisplayBuffer(std::uint32_t handle, std::int32_t index, const DisplayBuffer& buffer) {
        std::lock_guard preludeLock(preludeMutex);
        displayBuffers[{handle, index}] = buffer;
        registeredLocked(DisplayBufferCall{handle, index, buffer});
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
    static constexpr std::uint64_t Claimed = std::uint64_t{1} << 63u;

    struct Admitted {
        std::atomic<std::uint64_t>& admission;
        ~Admitted() { admission.fetch_sub(1, std::memory_order_acq_rel); }
    };

    bool admit() {
        auto current = admission.load(std::memory_order_acquire);
        while ((current & Claimed) == 0) {
            if (admission.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel)) return true;
        }
        return false;
    }

    static bool registration(const Call& call) {
        return !std::holds_alternative<SubmitCall>(call) && !std::holds_alternative<SuspendCall>(call);
    }

    void registeredLocked(Call call) {
        if ((admission.load(std::memory_order_acquire) & Claimed) != 0) pending.push_back(std::move(call));
        else prelude.push_back(std::move(call));
    }

    template <typename TForward>
    void journalLocked(Call call, TForward& forward) {
        std::vector<JournaledCall> entries;
        {
            std::lock_guard preludeLock(preludeMutex);
            for (auto& item : std::exchange(pending, {})) entries.push_back({std::move(item), {}, {}});
        }
        entries.push_back({std::move(call), {}, {}});
        for (const auto& entry : entries) requireJournaled(entry.call);
        require(GuestAllocations::GuestAllocationsGeneration_nid_postfix() == registryGeneration, "the registered guest memory changed while the journal was open; journaling memory registration is not implemented");
        entries.front().writes = shadow.Changes();
        {
            std::lock_guard flipLock(flipMutex);
            flips.clear();
        }
        forward();
        Settle(journal.ranges);
        {
            std::lock_guard flipLock(flipMutex);
            entries.back().flips = std::exchange(flips, {});
        }
        for (const auto& flip : entries.back().flips) journal.references.push_back(reference(flip));
        shadow.Refresh();
        for (auto& entry : entries) journal.calls.push_back(std::move(entry));
    }

    std::vector<Call> claim() {
        std::vector<Call> registered;
        {
            std::lock_guard preludeLock(preludeMutex);
            admission.fetch_or(Claimed, std::memory_order_acq_rel);
            registered = prelude;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((admission.load(std::memory_order_acquire) & ~Claimed) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                const auto inFlight = admission.load(std::memory_order_acquire) & ~Claimed;
                release();
                throw std::runtime_error("frame journal: " + std::to_string(inFlight) + " driver submissions stayed in flight for 5 s; the journal opens only between submissions");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return registered;
    }

    void release() {
        std::lock_guard preludeLock(preludeMutex);
        for (const auto& call : journal.calls) {
            if (registration(call.call)) prelude.push_back(call.call);
        }
        for (auto& call : std::exchange(pending, {})) prelude.push_back(std::move(call));
        admission.fetch_and(~Claimed, std::memory_order_acq_rel);
    }

    void openLocked(const std::filesystem::path& directory) {
        require(!open.load(std::memory_order_relaxed), "a journal is already open");
        JournalFile next;
        next.prelude = claim();
        try {
            fill(next, directory);
        } catch (...) {
            journal = {};
            shadow = {};
            release();
            throw;
        }
        open.store(true, std::memory_order_release);
    }

    static bool within(const std::vector<Range>& ranges, std::uint64_t address, std::uint64_t bytes) {
        const auto after = std::upper_bound(ranges.begin(), ranges.end(), address, [](std::uint64_t value, const Range& range) { return value < range.address; });
        if (after == ranges.begin()) return false;
        const auto& range = *std::prev(after);
        return address - range.address <= range.bytes && bytes <= range.bytes - (address - range.address);
    }

    static bool overlaps(const std::vector<Range>& ranges, std::uint64_t begin, std::uint64_t end) {
        const auto after = std::upper_bound(ranges.begin(), ranges.end(), end - 1, [](std::uint64_t value, const Range& range) { return value < range.address; });
        return after != ranges.begin() && std::prev(after)->address + std::prev(after)->bytes > begin;
    }

    static std::vector<Range> unregisteredCode(const JournalFile& next) {
        constexpr std::uint64_t PageBytes = 0x1000;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pages;
        for (const auto& call : next.prelude) {
            const auto* registration = std::get_if<RegisterShaderCall>(&call);
            if (registration == nullptr || registration->shader.bytes.size() < sizeof(Shader)) continue;
            Shader fields;
            std::memcpy(&fields, registration->shader.bytes.data(), sizeof(Shader));
            const auto code = pointer(const_cast<const void*>(fields.code));
            if (fields.shader_size == 0 || within(next.ranges, code, fields.shader_size) || !GuestMemory::Accessible(reinterpret_cast<const void*>(code), fields.shader_size)) continue;
            const auto begin = code & ~(PageBytes - 1);
            const auto end = (code + fields.shader_size + PageBytes - 1) & ~(PageBytes - 1);
            if (!overlaps(next.ranges, begin, end)) pages.emplace_back(begin, end);
        }
        std::sort(pages.begin(), pages.end());
        std::vector<Range> ranges;
        for (const auto& [begin, end] : pages) {
            if (!ranges.empty() && begin <= ranges.back().address + ranges.back().bytes) {
                const auto last = std::max(ranges.back().address + ranges.back().bytes, end);
                ranges.back().bytes = last - ranges.back().address;
                ranges.back().allocationBytes = ranges.back().bytes;
                ranges.back().committed.back().second = last;
                continue;
            }
            ranges.push_back({begin, end - begin, true, false, false, begin, end - begin, 0, 0, {{begin, end}}});
        }
        for (auto& range : ranges) range.writable = GuestMemory::Accessible(reinterpret_cast<const void*>(range.address), range.bytes, true);
        return ranges;
    }

    void fill(JournalFile& next, const std::filesystem::path& directory) {
        auto& driver = DriverDetail::Driver::Get();
        {
            std::lock_guard preludeLock(preludeMutex);
            for (const auto handle : driver.VideoOutputHandles()) require(outputs.contains(handle), "video output " + std::to_string(handle) + " was registered before the journal was armed");
        }
        require(driver.WaitQuiescent(), "a submission waits for a title write; the journal opens only when every accepted submission has completed");
        const auto gds = Pm4::GdsAddress();
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
        std::sort(next.ranges.begin(), next.ranges.end(), [](const Range& a, const Range& b) { return a.address < b.address; });
        for (auto& entry : unregisteredCode(next)) {
            for (const auto& [begin, end] : entry.committed) committedBytes += end - begin;
            if (sharedBacking(entry.address, entry.bytes)) shared.push_back(entry);
            next.ranges.push_back(std::move(entry));
        }
        std::sort(next.ranges.begin(), next.ranges.end(), [](const Range& a, const Range& b) { return a.address < b.address; });
        requireUnshared(shared);
        requireMemoryFor(committedBytes);
        Settle(next.ranges);
        GuestMemory::FlushGpuWrites(gds, Pm4::GdsBytes);
        auto queues = driver.Checkpoint();
        next.checkpoint.queues = std::move(queues.queues);
        next.checkpoint.resetGraphics = queues.resetGraphics;
        next.checkpoint.gds.resize(Pm4::GdsBytes);
        std::memcpy(next.checkpoint.gds.data(), reinterpret_cast<const void*>(gds), Pm4::GdsBytes);
        journal = std::move(next);
        const auto registered = journal.prelude.size();
        std::erase_if(journal.prelude, [&](const Call& call) { return !unjournaled(call).empty(); });
        if (journal.prelude.size() != registered) std::fprintf(stderr, "[journal] %zu of %zu registrations made before the journal opened refer to shader code that is no longer readable and are left out of the prelude\n", registered - journal.prelude.size(), registered);
        std::filesystem::create_directories(directory);
        BlockWriter blocks(directory);
        shadow.Load(journal.ranges, [&](std::uint64_t address, std::span<const std::byte> block) {
            if (std::any_of(block.begin(), block.end(), [](std::byte value) { return value != std::byte{0}; })) journal.image.push_back({address, blocks.Store(block), static_cast<std::uint32_t>(block.size())});
        });
        this->directory = directory;
    }

    void closeLocked() {
        require(open.load(std::memory_order_relaxed), "no journal is open");
        open.store(false, std::memory_order_release);
        std::exception_ptr failure;
        try {
            WriteJournal(directory, journal);
        } catch (...) {
            failure = std::current_exception();
        }
        release();
        journal = {};
        shadow = {};
        if (failure) std::rethrow_exception(failure);
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
        constexpr std::uint64_t Headroom = std::uint64_t{3} << 29;
        const auto needed = Shadow::BytesFor(imageBytes);
        const auto available = availableMemory();
        require(available >= needed + Headroom, "the line hashes of the " + std::to_string(imageBytes >> 20) + " MiB memory image need " + std::to_string(needed >> 20) + " MiB in this process, " + std::to_string(available >> 20) + " MiB are available and " + std::to_string(Headroom >> 20) + " MiB stay free");
    }

    bool journaled(std::uint64_t address, std::uint64_t bytes) const {
        const auto after = std::upper_bound(journal.ranges.begin(), journal.ranges.end(), address, [](std::uint64_t value, const Range& range) { return value < range.address; });
        if (after == journal.ranges.begin()) return false;
        const auto& range = *std::prev(after);
        return address - range.address <= range.bytes && bytes <= range.bytes - (address - range.address);
    }

    std::string unjournaledShader(const ShaderHeader& header) const {
        if (header.address == 0) return {};
        if (header.bytes.size() < sizeof(Shader)) return "the journal holds " + std::to_string(header.bytes.size()) + " bytes of shader header " + hex(header.address) + ", fewer than a header";
        Shader shader;
        std::memcpy(&shader, header.bytes.data(), sizeof(Shader));
        const auto code = pointer(const_cast<const void*>(shader.code));
        if (journaled(code, shader.shader_size)) return {};
        return "the code " + hex(code) + "+" + hex(shader.shader_size) + " of shader " + hex(header.address) + " lies outside the journaled guest memory";
    }

    std::string unjournaled(const Call& call) const {
        std::string reason;
        const auto check = [&](const ShaderHeader& header) {
            if (reason.empty()) reason = unjournaledShader(header);
        };
        if (const auto* item = std::get_if<RegisterShaderCall>(&call)) {
            check(item->shader);
        } else if (const auto* item = std::get_if<ResolveShaderAbiCall>(&call)) {
            check(item->shader);
        } else if (const auto* item = std::get_if<ResolveGraphicsAbiCall>(&call)) {
            check(item->vertex);
            check(item->pixel);
        } else if (const auto* item = std::get_if<ResolveGraphicsStagesAbiCall>(&call)) {
            for (const auto& stage : item->stages) check(stage);
        }
        return reason;
    }

    void requireJournaled(const Call& call) const {
        const auto reason = unjournaled(call);
        require(reason.empty(), reason);
    }

    Frame reference(const FlipInfo& flip) {
        if (flip.index < 0) return {};
        DisplayBuffer buffer;
        {
            std::lock_guard preludeLock(preludeMutex);
            const auto found = displayBuffers.find({flip.handle, flip.index});
            require(found != displayBuffers.end(), "a flip shows display buffer " + std::to_string(flip.index) + " of video output " + std::to_string(flip.handle) + ", which the journal has no display buffer for");
            buffer = found->second;
        }
        return ReadFrame(buffer);
    }

    std::mutex mutex;
    std::mutex preludeMutex;
    std::atomic<std::uint64_t> admission{0};
    std::atomic<bool> open{false};
    std::vector<Call> prelude;
    std::vector<Call> pending;
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
    DriverDetail::Driver::Get().RegisterVideoOutput(handle, journaled);
    std::lock_guard preludeLock(preludeMutex);
    outputs[handle] = {output, journaled};
    registeredLocked(VideoOutputCall{handle, true});
}

void Recording::UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    std::shared_ptr<IVideoOutput> journaled;
    {
        std::lock_guard preludeLock(preludeMutex);
        const auto found = outputs.find(handle);
        if (found != outputs.end() && found->second.first == output) journaled = found->second.second;
    }
    DriverDetail::Driver::Get().UnregisterVideoOutput(handle, journaled != nullptr ? journaled : output);
    std::lock_guard preludeLock(preludeMutex);
    if (journaled != nullptr) outputs.erase(handle);
    registeredLocked(VideoOutputCall{handle, false});
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
    Recording::Get().NoteSubmission([&] { return submitted(packet, queue); }, [&] { DriverDetail::Driver::Get().Submit(packet, queue); });
}

void SuspendPoint() {
    Recording::Get().NoteSubmission([] { return SuspendCall{}; }, [] { DriverDetail::Driver::Get().SuspendPoint(); });
}

void RegisterShader(const Shader* shader) {
    Recording::Get().NoteRegistration([&] { return RegisterShaderCall{copied(shader, shader->header_size), codeOf(shader)}; }, [&] { DriverDetail::Driver::Get().RegisterShader(shader); });
}

void ResolveShaderAbi(const Shader* shader, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
    Recording::Get().NoteRegistration([&] { return ResolveShaderAbiCall{copied(shader, sizeof(Shader)), copied(context), copied(primitive)}; }, [&] { DriverDetail::Driver::Get().ResolveShaderAbi(shader, context, primitive); });
}

void ResolveGraphicsStagesAbi(std::span<const Shader* const> stages, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
    const auto make = [&] {
        ResolveGraphicsStagesAbiCall call{{}, copied(context), copied(primitive)};
        for (const auto* stage : stages) call.stages.push_back(copied(stage, sizeof(Shader)));
        return call;
    };
    Recording::Get().NoteRegistration(make, [&] { DriverDetail::Driver::Get().ResolveGraphicsStagesAbi(stages, context, primitive); });
}

void ResolveGraphicsAbi(const Shader* vertex, const Shader* pixel, std::uint32_t primitiveType) {
    Recording::Get().NoteRegistration([&] { return ResolveGraphicsAbiCall{copied(vertex, sizeof(Shader)), copied(pixel, sizeof(Shader)), primitiveType}; }, [&] { DriverDetail::Driver::Get().ResolveGraphicsAbi(vertex, pixel, primitiveType); });
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
