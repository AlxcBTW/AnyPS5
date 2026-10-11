#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_JOURNAL_INCLUDE_FORMAT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_JOURNAL_INCLUDE_FORMAT_HPP

#include "prx/libSceAgcDriver/Journal/include/Journal.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "SceShaders.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace AgcDriver::Journal {

inline constexpr std::uint32_t FormatVersion = 3;
inline constexpr std::size_t ImageBlockBytes = 65536;

struct SubmitCall {
    std::uint32_t queue = 0;
    std::uint64_t address = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint32_t> words;
};

struct SuspendCall {};

struct ShaderHeader {
    std::uint64_t address = 0;
    std::vector<std::byte> bytes;
};

struct RegisterShaderCall {
    ShaderHeader shader;
    std::vector<std::byte> code;
};

struct ResolveShaderAbiCall {
    ShaderHeader shader;
    std::vector<ShaderRegister> context;
    std::vector<ShaderRegister> primitive;
};

struct ResolveGraphicsAbiCall {
    ShaderHeader vertex;
    ShaderHeader pixel;
    std::uint32_t primitiveType = 0;
};

struct ResolveGraphicsStagesAbiCall {
    std::vector<ShaderHeader> stages;
    std::vector<ShaderRegister> context;
    std::vector<ShaderRegister> primitive;
};

struct VideoOutputCall {
    std::uint32_t handle = 0;
    bool registered = false;
};

struct DisplayBufferCall {
    std::uint32_t handle = 0;
    std::int32_t index = 0;
    DisplayBuffer buffer{};
};

using Call = std::variant<SubmitCall, SuspendCall, RegisterShaderCall, ResolveShaderAbiCall, ResolveGraphicsAbiCall, ResolveGraphicsStagesAbiCall, VideoOutputCall, DisplayBufferCall>;

struct Range {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    bool readable = false;
    bool writable = false;
    bool gpu = false;
    std::uint64_t allocationAddress = 0;
    std::uint64_t allocationBytes = 0;
    std::uint64_t backing = 0;
    std::uint64_t backingOffset = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> committed;
};

struct ImageBlock {
    std::uint64_t address = 0;
    std::uint64_t offset = 0;
    std::uint32_t bytes = 0;
};

struct TitleWrite {
    std::uint64_t address = 0;
    std::vector<std::byte> bytes;
};

struct JournaledCall {
    Call call;
    std::vector<TitleWrite> writes;
    std::vector<FlipInfo> flips;
};

struct Checkpoint {
    std::map<std::uint32_t, QueueState> queues;
    bool resetGraphics = false;
    std::vector<std::byte> gds;
};

struct JournalFile {
    std::uint64_t arenaBase = 0;
    std::uint64_t arenaBytes = 0;
    bool arenaWriteWatched = false;
    std::vector<std::pair<std::string, std::string>> environment;
    std::vector<Range> ranges;
    std::vector<ImageBlock> image;
    std::vector<Call> prelude;
    Checkpoint checkpoint;
    std::vector<JournaledCall> calls;
    std::vector<Frame> references;
};

void WriteJournal(const std::filesystem::path& directory, const JournalFile& journal);
JournalFile ReadJournal(const std::filesystem::path& directory);

class BlockWriter {
public:
    explicit BlockWriter(const std::filesystem::path& directory);
    std::uint64_t Store(std::span<const std::byte> bytes);

private:
    bool storedAt(std::uint64_t offset, std::span<const std::byte> bytes);
    std::fstream file;
    std::uint64_t size = 0;
    std::unordered_multimap<std::size_t, std::pair<std::uint64_t, std::size_t>> stored;
};

class BlockReader {
public:
    explicit BlockReader(const std::filesystem::path& directory);
    void Read(const ImageBlock& block, std::span<std::byte> destination);

private:
    std::ifstream file;
    std::uint64_t size = 0;
};

}

#endif
