#include "prx/libSceAgcDriver/Journal/include/Journal.hpp"
#include "prx/libSceAgcDriver/Journal/include/Format.hpp"
#include "prx/libSceAgcDriver/Journal/include/Readback.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Journal {
namespace {

constexpr std::uint32_t ReadOnly = 0x02;
constexpr std::uint32_t ReadWrite = 0x04;

constexpr std::array<std::pair<const char*, const char*>, 6> StrictPins{{
    {"APS5_NO_WRITE_EVIDENCE", "1"},
    {"APS5_NO_PIPELINE_LTO", "1"},
    {"APS5_GPU_WAIT_TIMEOUT_MS", "600000"},
    {"APS5_THREAD_PRIORITY", "off"},
    {"APS5_NO_WORKER_AFFINITY", "1"},
    {"ANYPS5_NO_SHADER_CACHE", "1"},
}};

void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error("frame journal replay: " + reason);
}

std::string hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::vector<EnvironmentVariable> environmentOf(const JournalFile& journal) {
    std::vector<EnvironmentVariable> result;
    const auto set = [&](const std::string& name, std::optional<std::string> value) {
        const auto existing = std::find_if(result.begin(), result.end(), [&](const EnvironmentVariable& variable) { return variable.name == name; });
        if (existing != result.end()) existing->value = std::move(value);
        else result.push_back({name, std::move(value)});
    };
    set("ANYPS5_GPU", std::nullopt);
    for (const auto& [name, value] : journal.environment) set(name, value);
    for (const auto& [name, value] : StrictPins) set(name, value);
    return result;
}

void requireEnvironment(const JournalFile& journal) {
    for (const auto& variable : environmentOf(journal)) {
        const char* current = std::getenv(variable.name.c_str());
        const bool matches = variable.value ? current != nullptr && *variable.value == current : current == nullptr;
        require(matches, variable.name + (variable.value ? " is not " + *variable.value : " is set") + " in this process; apply ReplayEnvironment before the replay");
    }
    require(GuestArena::GuestArenaWriteWatched_nid_postfix() == journal.arenaWriteWatched, std::string("the guest arena is ") + (journal.arenaWriteWatched ? "not " : "") + "write-watched in this process but was in the recording; start the replay with APS5_NO_WRITE_WATCH set as it was recorded");
}

class FlipLog {
public:
    void Add(const FlipInfo& info) {
        std::lock_guard lock(mutex);
        flips.push_back(info);
    }
    std::vector<FlipInfo> Take() {
        std::lock_guard lock(mutex);
        return std::exchange(flips, {});
    }

private:
    std::mutex mutex;
    std::vector<FlipInfo> flips;
};

class ReadbackRequest final : public IFlipRequest {
public:
    void GpuReady(const std::shared_ptr<FrameTiming>&) override {}
    void Fail(std::exception_ptr) noexcept override {}
};

class NoRenderingWait final : public IRenderingWait {
public:
    void Wait() override {}
};

class ReadbackOutput final : public IVideoOutput {
public:
    explicit ReadbackOutput(std::shared_ptr<FlipLog> log) : log(std::move(log)) {}
    std::shared_ptr<IFlipRequest> Reserve(const FlipInfo& info) override {
        log->Add(info);
        return std::make_shared<ReadbackRequest>();
    }
    std::shared_ptr<IRenderingWait> CaptureRenderingWait(std::uint32_t) override { return std::make_shared<NoRenderingWait>(); }
    void Fail(std::exception_ptr) noexcept override {}

private:
    std::shared_ptr<FlipLog> log;
};

class Host {
public:
    explicit Host(const JournalFile& journal) : journal(journal) {}

    void Place(BlockReader& blocks) {
        std::vector<const Range*> outside;
        for (const auto& range : journal.ranges) {
            require(range.backing == 0, "range " + hex(range.address) + " belongs to an alias group; alias groups are not implemented");
            if (inArena(range)) placeInArena(range);
            else outside.push_back(&range);
        }
        placeOutsideArena(outside);
        for (const auto& block : journal.image) {
            require(placed(block.address, block.bytes), "image block " + hex(block.address) + " lies outside the journaled ranges");
            blocks.Read(block, {reinterpret_cast<std::byte*>(block.address), block.bytes});
        }
        GuestAllocations::Mutation mutation;
        for (const auto& range : journal.ranges) {
            for (const auto& [begin, end] : range.committed) {
                if (!inArena(range)) {
                    if (!range.writable) protectOutsideArena(begin, end - begin);
                    continue;
                }
                if (!range.writable) GuestArena::GuestArenaSetProtection_nid_postfix(begin, end - begin, ReadOnly);
#ifndef _WIN32
                GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(reinterpret_cast<const void*>(begin), end - begin);
#endif
            }
            mutation.Add(reinterpret_cast<void*>(range.address), range.bytes, range.readable, range.writable, range.gpu);
        }
    }

    void Restore() {
        auto& driver = DriverDetail::Driver::Get();
        driver.Restore({journal.checkpoint.queues, journal.checkpoint.resetGraphics});
        require(journal.checkpoint.gds.size() == Pm4::GdsBytes, "the checkpoint's GDS has the wrong size");
        std::memcpy(reinterpret_cast<void*>(Pm4::GdsAddress()), journal.checkpoint.gds.data(), Pm4::GdsBytes);
    }

    void Perform(const Call& call) {
        std::visit([&](const auto& item) { perform(item); }, call);
    }

    void Apply(const std::vector<TitleWrite>& writes) {
        for (const auto& write : writes) {
            require(placed(write.address, write.bytes.size()), "title write " + hex(write.address) + " lies outside the journaled ranges");
            std::memcpy(reinterpret_cast<void*>(write.address), write.bytes.data(), write.bytes.size());
        }
    }

    std::vector<Frame> Frames(std::size_t index, const std::vector<FlipInfo>& recorded) {
        const auto flips = log->Take();
        const auto same = [](const FlipInfo& a, const FlipInfo& b) { return a.handle == b.handle && a.index == b.index && a.mode == b.mode && a.argument == b.argument; };
        require(std::equal(flips.begin(), flips.end(), recorded.begin(), recorded.end(), same), "call " + std::to_string(index) + " reserved " + std::to_string(flips.size()) + " flips, the recording " + std::to_string(recorded.size()) + " (or different ones)");
        std::vector<Frame> frames;
        for (const auto& flip : flips) {
            if (flip.index < 0) {
                frames.emplace_back();
                continue;
            }
            const auto buffer = displayBuffers.find({flip.handle, flip.index});
            require(buffer != displayBuffers.end(), "a flip shows display buffer " + std::to_string(flip.index) + " of video output " + std::to_string(flip.handle) + ", which the journal has no display buffer for");
            frames.push_back(ReadFrame(buffer->second));
        }
        return frames;
    }

private:
    static bool inArena(const Range& range) {
        return GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(range.address), range.bytes);
    }

    static void placeInArena(const Range& range) {
        auto cursor = range.address;
        for (const auto& [begin, end] : range.committed) {
            if (cursor < begin) GuestArena::GuestArenaMarkUsed_nid_postfix(reinterpret_cast<void*>(cursor), begin - cursor);
            GuestArena::GuestArenaMarkUsed_nid_postfix(reinterpret_cast<void*>(begin), end - begin);
            GuestArena::GuestArenaCommit_nid_postfix(reinterpret_cast<void*>(begin), end - begin, ReadWrite, end - begin);
            cursor = end;
        }
        if (cursor < range.address + range.bytes) GuestArena::GuestArenaMarkUsed_nid_postfix(reinterpret_cast<void*>(cursor), range.address + range.bytes - cursor);
    }

    static void placeOutsideArena(const std::vector<const Range*>& ranges) {
#ifdef _WIN32
        constexpr std::uint64_t Granule = 0x10000;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
        for (const auto* range : ranges) spans.emplace_back(range->address & ~(Granule - 1), (range->address + range->bytes + Granule - 1) & ~(Granule - 1));
        std::sort(spans.begin(), spans.end());
        std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
        for (const auto& span : spans) {
            if (!merged.empty() && span.first <= merged.back().second) merged.back().second = std::max(merged.back().second, span.second);
            else merged.push_back(span);
        }
        for (const auto& [begin, end] : merged) {
            require(VirtualAlloc(reinterpret_cast<void*>(begin), end - begin, MEM_RESERVE, PAGE_NOACCESS) == reinterpret_cast<void*>(begin), "cannot reserve " + hex(begin) + "+" + hex(end - begin) + " outside the guest arena at its recorded address (error " + std::to_string(GetLastError()) + ")");
        }
        for (const auto* range : ranges) {
            for (const auto& [begin, end] : range->committed) {
                require(VirtualAlloc(reinterpret_cast<void*>(begin), end - begin, MEM_COMMIT, PAGE_READWRITE) == reinterpret_cast<void*>(begin), "cannot commit " + hex(begin) + "+" + hex(end - begin) + " outside the guest arena (error " + std::to_string(GetLastError()) + ")");
            }
        }
#else
        for (const auto* range : ranges) {
            for (const auto& [begin, end] : range->committed) {
                void* const wanted = reinterpret_cast<void*>(begin);
                void* const mapped = mmap(wanted, end - begin, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                if (mapped != MAP_FAILED && mapped != wanted) munmap(mapped, end - begin);
                require(mapped == wanted, "cannot map " + hex(begin) + "+" + hex(end - begin) + " outside the guest arena at its recorded address");
            }
        }
#endif
    }

    static void protectOutsideArena(std::uint64_t address, std::uint64_t bytes) {
#ifdef _WIN32
        DWORD previous = 0;
        require(VirtualProtect(reinterpret_cast<void*>(address), bytes, PAGE_READONLY, &previous) != 0, "cannot protect " + hex(address) + " read-only");
#else
        require(mprotect(reinterpret_cast<void*>(address), bytes, PROT_READ) == 0, "cannot protect " + hex(address) + " read-only");
#endif
    }

    bool placed(std::uint64_t address, std::uint64_t bytes) const {
        for (const auto& range : journal.ranges) {
            for (const auto& [begin, end] : range.committed) {
                if (address >= begin && bytes <= end - begin && address - begin <= end - begin - bytes) return true;
            }
        }
        return false;
    }

    void perform(const SubmitCall& call) {
        const auto bytes = call.words.size() * sizeof(std::uint32_t);
        const std::uint32_t* words = reinterpret_cast<const std::uint32_t*>(call.address);
        if (!call.words.empty() && placed(call.address, bytes)) {
            require(std::memcmp(words, call.words.data(), bytes) == 0, "the command words at " + hex(call.address) + " differ from the recording before their submission");
        } else if (!call.words.empty()) {
            words = commandCopies.emplace_back(call.words).data();
        }
        const Packet packet{const_cast<std::uint32_t*>(words), static_cast<std::uint32_t>(call.words.size()), call.flags, {}};
        AgcDriver::Submit(&packet, call.queue);
    }

    void perform(const SuspendCall&) {
        AgcDriverSuspendPoint_nid_postfix();
    }

    void perform(const RegisterShaderCall& call) {
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(call.shader));
    }

    void perform(const ResolveShaderAbiCall& call) {
        AgcDriverResolveShaderAbi_nid_postfix(reinterpret_cast<const Shader*>(call.shader), call.context, call.primitive);
    }

    void perform(const ResolveGraphicsAbiCall& call) {
        AgcDriverResolveGraphicsAbi_nid_postfix(reinterpret_cast<const Shader*>(call.vertex), reinterpret_cast<const Shader*>(call.pixel), call.primitiveType);
    }

    void perform(const ResolveGraphicsStagesAbiCall& call) {
        std::vector<const Shader*> stages;
        for (const auto stage : call.stages) stages.push_back(reinterpret_cast<const Shader*>(stage));
        AgcDriverResolveGraphicsStagesAbi_nid_postfix(stages, call.context, call.primitive);
    }

    void perform(const VideoOutputCall& call) {
        if (call.registered) {
            const auto output = std::make_shared<ReadbackOutput>(log);
            AgcDriverRegisterVideoOutput_nid_postfix(call.handle, output);
            outputs[call.handle] = output;
            return;
        }
        const auto output = outputs.find(call.handle);
        require(output != outputs.end(), "the recording unregisters video output " + std::to_string(call.handle) + ", which it never registered");
        AgcDriverUnregisterVideoOutput_nid_postfix(call.handle, output->second);
        outputs.erase(output);
    }

    void perform(const DisplayBufferCall& call) {
        displayBuffers[{call.handle, call.index}] = call.buffer;
    }

    const JournalFile& journal;
    std::shared_ptr<FlipLog> log = std::make_shared<FlipLog>();
    std::map<std::uint32_t, std::shared_ptr<IVideoOutput>> outputs;
    std::map<std::pair<std::uint32_t, std::int32_t>, DisplayBuffer> displayBuffers;
    std::deque<std::vector<std::uint32_t>> commandCopies;
};

}

std::vector<EnvironmentVariable> ReplayEnvironment(const std::filesystem::path& directory) {
    return environmentOf(ReadJournal(directory));
}

Report Replay(const std::filesystem::path& directory) {
    const auto journal = ReadJournal(directory);
    requireEnvironment(journal);
    BlockReader blocks(directory);
    Host host(journal);
    host.Place(blocks);
    for (const auto& call : journal.prelude) host.Perform(call);
    host.Restore();
    Report report;
    report.references = journal.references;
    report.calls = journal.calls.size();
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < journal.calls.size(); ++index) {
        const auto& call = journal.calls[index];
        host.Apply(call.writes);
        host.Perform(call.call);
        Settle(journal.ranges);
        for (auto& frame : host.Frames(index, call.flips)) report.frames.push_back(std::move(frame));
    }
    report.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return report;
}

}
