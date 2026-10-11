#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_JOURNAL_INCLUDE_JOURNAL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_JOURNAL_INCLUDE_JOURNAL_HPP

#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace AgcDriver::Journal {

struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> bgra;
    bool operator==(const Frame&) const = default;
};

struct GpuRange {
    std::string kind;
    std::uint32_t queue = 0;
    std::uint64_t program = 0;
    std::uint64_t vertexProgram = 0;
    std::array<std::uint32_t, 3> shape{};
    double beginNs = 0;
    double endNs = 0;
};

struct Iteration {
    std::vector<Frame> frames;
    double milliseconds = 0;
    std::vector<GpuRange> gpuRanges;
    std::uint64_t droppedRanges = 0;
};

struct ReplayOptions {
    std::size_t iterations = 1;
    bool gpuTimes = false;
};

struct Report {
    std::vector<Frame> references;
    std::size_t calls = 0;
    std::vector<Iteration> iterations;
};

struct EnvironmentVariable {
    std::string name;
    std::optional<std::string> value;
};

void Arm();
void Open(const std::filesystem::path& directory);
void Close();
void NoteDisplayBuffer(std::uint32_t handle, std::int32_t index, const DisplayBuffer& buffer);
std::vector<EnvironmentVariable> ReplayEnvironment(const std::filesystem::path& directory);
Report Replay(const std::filesystem::path& directory, const ReplayOptions& options = {});

}

extern "C" void AgcDriverJournalDisplayBuffer_nid_postfix(std::uint32_t handle, std::int32_t index, const AgcDriver::DisplayBuffer& buffer);

#endif
