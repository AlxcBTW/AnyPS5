#include "prx/libSceAgcDriver/Journal/include/Format.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace AgcDriver::Journal {
namespace {

static_assert(std::endian::native == std::endian::little, "the journal stores host integers as little-endian bytes");

constexpr std::array<char, 8> Magic{'A', 'G', 'C', 'J', 'R', 'N', 'L', '\0'};

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("frame journal: ") + reason);
}

class Encoder {
public:
    template <typename TValue>
    void Value(TValue value) {
        static_assert(std::is_trivially_copyable_v<TValue>);
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(TValue));
    }
    void Flag(bool value) { Value<std::uint8_t>(value ? 1 : 0); }
    void Count(std::size_t count) { Value<std::uint64_t>(count); }
    void Bytes(std::span<const std::byte> bytes) {
        Count(bytes.size());
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    void Text(std::string_view text) { Bytes(std::as_bytes(std::span(text.data(), text.size()))); }
    std::vector<std::byte> data;
};

class Decoder {
public:
    explicit Decoder(std::span<const std::byte> data) : data(data) {}
    template <typename TValue>
    TValue Value() {
        static_assert(std::is_trivially_copyable_v<TValue>);
        require(sizeof(TValue) <= data.size() - at, "the journal file is truncated");
        TValue value;
        std::memcpy(&value, data.data() + at, sizeof(TValue));
        at += sizeof(TValue);
        return value;
    }
    bool Flag() {
        const auto value = Value<std::uint8_t>();
        require(value <= 1, "a flag in the journal file is neither 0 nor 1");
        return value == 1;
    }
    std::size_t Count(std::size_t elementBytes) {
        const auto count = Value<std::uint64_t>();
        require(count <= (data.size() - at) / std::max<std::size_t>(elementBytes, 1), "a count in the journal file exceeds its remaining bytes");
        return static_cast<std::size_t>(count);
    }
    std::vector<std::byte> Bytes() {
        const auto count = Count(1);
        std::vector<std::byte> bytes(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + count));
        at += count;
        return bytes;
    }
    std::string Text() {
        const auto bytes = Bytes();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    bool Done() const { return at == data.size(); }

private:
    std::span<const std::byte> data;
    std::size_t at = 0;
};

void encode(Encoder& out, const Registers& registers) {
    out.Count(registers.size());
    for (const auto& [offset, value] : registers) {
        out.Value(offset);
        out.Value(value);
    }
}

void decode(Decoder& in, Registers& registers) {
    registers.clear();
    const auto count = in.Count(8);
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = in.Value<std::uint32_t>();
        registers.insert_or_assign(offset, in.Value<std::uint32_t>());
    }
}

void encode(Encoder& out, const QueueState& state) {
    encode(out, state.shader);
    encode(out, state.context);
    encode(out, state.userConfig);
    out.Flag(state.savedContext.has_value());
    if (state.savedContext) encode(out, *state.savedContext);
    out.Bytes(std::as_bytes(std::span(state.constantRam)));
    out.Value(state.indexBase);
    out.Value(state.drawIndirectBase);
    out.Value(state.dispatchIndirectBase);
    out.Value(state.indexBufferSize);
    out.Value(state.indexType);
    out.Value(state.instanceCount);
    out.Count(state.markers.size());
    for (const auto& marker : state.markers) out.Text(marker);
    out.Value(state.predication.address);
    out.Value(state.predication.operation);
    out.Flag(state.predication.executeWhenSet);
}

void decode(Decoder& in, QueueState& state) {
    decode(in, state.shader);
    decode(in, state.context);
    decode(in, state.userConfig);
    if (in.Flag()) decode(in, state.savedContext.emplace());
    const auto ram = in.Bytes();
    require(ram.size() == sizeof(state.constantRam), "a queue's constant RAM has the wrong size");
    std::memcpy(state.constantRam.data(), ram.data(), ram.size());
    state.indexBase = in.Value<std::uint64_t>();
    state.drawIndirectBase = in.Value<std::uint64_t>();
    state.dispatchIndirectBase = in.Value<std::uint64_t>();
    state.indexBufferSize = in.Value<std::uint32_t>();
    state.indexType = in.Value<std::uint32_t>();
    state.instanceCount = in.Value<std::uint32_t>();
    state.markers.resize(in.Count(8));
    for (auto& marker : state.markers) marker = in.Text();
    state.predication.address = in.Value<std::uint64_t>();
    state.predication.operation = in.Value<std::uint32_t>();
    state.predication.executeWhenSet = in.Flag();
}

void encode(Encoder& out, std::span<const ShaderRegister> registers) {
    out.Count(registers.size());
    for (const auto& item : registers) {
        out.Value(item.offset);
        out.Value(item.value);
    }
}

void decode(Decoder& in, std::vector<ShaderRegister>& registers) {
    registers.resize(in.Count(8));
    for (auto& item : registers) {
        item.offset = in.Value<std::uint32_t>();
        item.value = in.Value<std::uint32_t>();
    }
}

void encode(Encoder& out, const DisplayBuffer& buffer) {
    out.Value(buffer.address);
    out.Value(buffer.pixelFormat);
    out.Value(buffer.width);
    out.Value(buffer.height);
    out.Value(buffer.tilingMode);
    out.Value(buffer.pitchInPixel);
    out.Value(buffer.dccAddress);
    out.Value(buffer.dccClearColor);
}

void decode(Decoder& in, DisplayBuffer& buffer) {
    buffer.address = in.Value<std::uint64_t>();
    buffer.pixelFormat = in.Value<std::uint64_t>();
    buffer.width = in.Value<std::uint32_t>();
    buffer.height = in.Value<std::uint32_t>();
    buffer.tilingMode = in.Value<std::uint32_t>();
    buffer.pitchInPixel = in.Value<std::uint32_t>();
    buffer.dccAddress = in.Value<std::uint64_t>();
    buffer.dccClearColor = in.Value<std::uint64_t>();
}

void encode(Encoder& out, const SubmitCall& call) {
    out.Value(call.queue);
    out.Value(call.address);
    out.Value(call.flags);
    out.Bytes(std::as_bytes(std::span(call.words)));
}

void encode(Encoder&, const SuspendCall&) {}

void encode(Encoder& out, const ShaderHeader& header) {
    out.Value(header.address);
    out.Bytes(header.bytes);
}

ShaderHeader decodeHeader(Decoder& in) {
    ShaderHeader header;
    header.address = in.Value<std::uint64_t>();
    const auto bytes = in.Bytes();
    header.bytes.assign(bytes.begin(), bytes.end());
    return header;
}

void encode(Encoder& out, const RegisterShaderCall& call) {
    encode(out, call.shader);
}

void encode(Encoder& out, const ResolveShaderAbiCall& call) {
    encode(out, call.shader);
    encode(out, call.context);
    encode(out, call.primitive);
}

void encode(Encoder& out, const ResolveGraphicsAbiCall& call) {
    encode(out, call.vertex);
    encode(out, call.pixel);
    out.Value(call.primitiveType);
}

void encode(Encoder& out, const ResolveGraphicsStagesAbiCall& call) {
    out.Count(call.stages.size());
    for (const auto& stage : call.stages) encode(out, stage);
    encode(out, call.context);
    encode(out, call.primitive);
}

void encode(Encoder& out, const VideoOutputCall& call) {
    out.Value(call.handle);
    out.Flag(call.registered);
}

void encode(Encoder& out, const DisplayBufferCall& call) {
    out.Value(call.handle);
    out.Value(call.index);
    encode(out, call.buffer);
}

void encode(Encoder& out, const Call& call) {
    out.Value(static_cast<std::uint8_t>(call.index()));
    std::visit([&](const auto& item) { encode(out, item); }, call);
}

Call decodeCall(Decoder& in) {
    switch (in.Value<std::uint8_t>()) {
        case 0: {
            SubmitCall call;
            call.queue = in.Value<std::uint32_t>();
            call.address = in.Value<std::uint64_t>();
            call.flags = in.Value<std::uint8_t>();
            const auto words = in.Bytes();
            require(words.size() % sizeof(std::uint32_t) == 0, "a submission's command bytes are not whole words");
            call.words.resize(words.size() / sizeof(std::uint32_t));
            std::memcpy(call.words.data(), words.data(), words.size());
            return call;
        }
        case 1: return SuspendCall{};
        case 2: return RegisterShaderCall{decodeHeader(in)};
        case 3: {
            ResolveShaderAbiCall call;
            call.shader = decodeHeader(in);
            decode(in, call.context);
            decode(in, call.primitive);
            return call;
        }
        case 4: {
            ResolveGraphicsAbiCall call;
            call.vertex = decodeHeader(in);
            call.pixel = decodeHeader(in);
            call.primitiveType = in.Value<std::uint32_t>();
            return call;
        }
        case 5: {
            ResolveGraphicsStagesAbiCall call;
            call.stages.resize(in.Count(8));
            for (auto& stage : call.stages) stage = decodeHeader(in);
            decode(in, call.context);
            decode(in, call.primitive);
            return call;
        }
        case 6: {
            VideoOutputCall call;
            call.handle = in.Value<std::uint32_t>();
            call.registered = in.Flag();
            return call;
        }
        case 7: {
            DisplayBufferCall call;
            call.handle = in.Value<std::uint32_t>();
            call.index = in.Value<std::int32_t>();
            decode(in, call.buffer);
            return call;
        }
        default: throw std::runtime_error("frame journal: unknown call kind in the journal file");
    }
}

static_assert(std::variant_size_v<Call> == 8, "decodeCall handles every call kind");

void encode(Encoder& out, const FlipInfo& flip) {
    out.Value(flip.handle);
    out.Value(flip.index);
    out.Value(flip.mode);
    out.Value(flip.argument);
}

FlipInfo decodeFlip(Decoder& in) {
    FlipInfo flip{};
    flip.handle = in.Value<std::uint32_t>();
    flip.index = in.Value<std::int32_t>();
    flip.mode = in.Value<std::uint32_t>();
    flip.argument = in.Value<std::int64_t>();
    return flip;
}

void encode(Encoder& out, const Frame& frame) {
    out.Value(frame.width);
    out.Value(frame.height);
    out.Bytes(frame.bgra);
}

Frame decodeFrame(Decoder& in) {
    Frame frame;
    frame.width = in.Value<std::uint32_t>();
    frame.height = in.Value<std::uint32_t>();
    frame.bgra = in.Bytes();
    require(frame.bgra.size() == static_cast<std::size_t>(frame.width) * frame.height * 4, "a reference frame's size does not match its dimensions");
    return frame;
}

std::filesystem::path journalPath(const std::filesystem::path& directory) {
    return directory / "journal.bin";
}

std::filesystem::path blocksPath(const std::filesystem::path& directory) {
    return directory / "blocks.bin";
}

}

void WriteJournal(const std::filesystem::path& directory, const JournalFile& journal) {
    Encoder out;
    out.data.insert(out.data.end(), reinterpret_cast<const std::byte*>(Magic.data()), reinterpret_cast<const std::byte*>(Magic.data() + Magic.size()));
    out.Value(FormatVersion);
    out.Value(journal.arenaBase);
    out.Value(journal.arenaBytes);
    out.Flag(journal.arenaWriteWatched);
    out.Count(journal.environment.size());
    for (const auto& [name, value] : journal.environment) {
        out.Text(name);
        out.Text(value);
    }
    out.Count(journal.ranges.size());
    for (const auto& range : journal.ranges) {
        out.Value(range.address);
        out.Value(range.bytes);
        out.Flag(range.readable);
        out.Flag(range.writable);
        out.Flag(range.gpu);
        out.Value(range.allocationAddress);
        out.Value(range.allocationBytes);
        out.Value(range.backing);
        out.Value(range.backingOffset);
        out.Count(range.committed.size());
        for (const auto& [begin, end] : range.committed) {
            out.Value(begin);
            out.Value(end);
        }
    }
    out.Count(journal.image.size());
    for (const auto& block : journal.image) {
        out.Value(block.address);
        out.Value(block.offset);
        out.Value(block.bytes);
    }
    out.Count(journal.prelude.size());
    for (const auto& call : journal.prelude) encode(out, call);
    out.Count(journal.checkpoint.queues.size());
    for (const auto& [queue, state] : journal.checkpoint.queues) {
        out.Value(queue);
        encode(out, state);
    }
    out.Flag(journal.checkpoint.resetGraphics);
    out.Bytes(journal.checkpoint.gds);
    out.Count(journal.calls.size());
    for (const auto& call : journal.calls) {
        encode(out, call.call);
        out.Count(call.writes.size());
        for (const auto& write : call.writes) {
            out.Value(write.address);
            out.Bytes(write.bytes);
        }
        out.Count(call.flips.size());
        for (const auto& flip : call.flips) encode(out, flip);
    }
    out.Count(journal.references.size());
    for (const auto& frame : journal.references) encode(out, frame);
    std::ofstream file(journalPath(directory), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(out.data.data()), static_cast<std::streamsize>(out.data.size()));
    file.close();
    require(file.good(), "cannot write journal.bin");
}

JournalFile ReadJournal(const std::filesystem::path& directory) {
    std::ifstream file(journalPath(directory), std::ios::binary);
    require(file.is_open(), "cannot open journal.bin");
    const std::vector<char> text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Decoder in(std::as_bytes(std::span(text)));
    std::array<char, Magic.size()> magic{};
    for (auto& character : magic) character = static_cast<char>(in.Value<std::uint8_t>());
    require(magic == Magic, "journal.bin is not a frame journal");
    const auto version = in.Value<std::uint32_t>();
    if (version != FormatVersion) throw std::runtime_error("frame journal: journal.bin has format version " + std::to_string(version) + ", this build reads version " + std::to_string(FormatVersion));
    JournalFile journal;
    journal.arenaBase = in.Value<std::uint64_t>();
    journal.arenaBytes = in.Value<std::uint64_t>();
    journal.arenaWriteWatched = in.Flag();
    journal.environment.resize(in.Count(16));
    for (auto& [name, value] : journal.environment) {
        name = in.Text();
        value = in.Text();
    }
    journal.ranges.resize(in.Count(59));
    for (auto& range : journal.ranges) {
        range.address = in.Value<std::uint64_t>();
        range.bytes = in.Value<std::uint64_t>();
        range.readable = in.Flag();
        range.writable = in.Flag();
        range.gpu = in.Flag();
        range.allocationAddress = in.Value<std::uint64_t>();
        range.allocationBytes = in.Value<std::uint64_t>();
        range.backing = in.Value<std::uint64_t>();
        range.backingOffset = in.Value<std::uint64_t>();
        range.committed.resize(in.Count(16));
        for (auto& [begin, end] : range.committed) {
            begin = in.Value<std::uint64_t>();
            end = in.Value<std::uint64_t>();
            require(range.address <= begin && begin < end && end - range.address <= range.bytes, "a committed piece lies outside its range");
        }
    }
    journal.image.resize(in.Count(20));
    for (auto& block : journal.image) {
        block.address = in.Value<std::uint64_t>();
        block.offset = in.Value<std::uint64_t>();
        block.bytes = in.Value<std::uint32_t>();
    }
    journal.prelude.resize(in.Count(1));
    for (auto& call : journal.prelude) call = decodeCall(in);
    const auto queues = in.Count(4);
    for (std::size_t i = 0; i < queues; ++i) {
        const auto queue = in.Value<std::uint32_t>();
        decode(in, journal.checkpoint.queues[queue]);
    }
    journal.checkpoint.resetGraphics = in.Flag();
    journal.checkpoint.gds = in.Bytes();
    journal.calls.resize(in.Count(1));
    for (auto& call : journal.calls) {
        call.call = decodeCall(in);
        call.writes.resize(in.Count(16));
        for (auto& write : call.writes) {
            write.address = in.Value<std::uint64_t>();
            write.bytes = in.Bytes();
        }
        call.flips.resize(in.Count(20));
        for (auto& flip : call.flips) flip = decodeFlip(in);
    }
    journal.references.resize(in.Count(16));
    for (auto& frame : journal.references) frame = decodeFrame(in);
    require(in.Done(), "journal.bin has bytes after its last section");
    return journal;
}

BlockWriter::BlockWriter(const std::filesystem::path& directory) : file(blocksPath(directory), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc) {
    require(file.is_open(), "cannot create blocks.bin");
}

bool BlockWriter::storedAt(std::uint64_t offset, std::span<const std::byte> bytes) {
    std::vector<std::byte> existing(bytes.size());
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char*>(existing.data()), static_cast<std::streamsize>(existing.size()));
    require(file.good(), "cannot read back blocks.bin");
    return std::equal(existing.begin(), existing.end(), bytes.begin());
}

std::uint64_t BlockWriter::Store(std::span<const std::byte> bytes) {
    const auto hash = std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    const auto [first, last] = stored.equal_range(hash);
    for (auto candidate = first; candidate != last; ++candidate) {
        if (candidate->second.second == bytes.size() && storedAt(candidate->second.first, bytes)) return candidate->second.first;
    }
    const auto offset = size;
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    require(file.good(), "cannot write blocks.bin");
    size += bytes.size();
    stored.emplace(hash, std::make_pair(offset, bytes.size()));
    return offset;
}

BlockReader::BlockReader(const std::filesystem::path& directory) : file(blocksPath(directory), std::ios::binary) {
    require(file.is_open(), "cannot open blocks.bin");
    file.seekg(0, std::ios::end);
    size = static_cast<std::uint64_t>(file.tellg());
}

void BlockReader::Read(const ImageBlock& block, std::span<std::byte> destination) {
    require(destination.size() == block.bytes && block.offset <= size && block.bytes <= size - block.offset, "an image block lies outside blocks.bin");
    file.seekg(static_cast<std::streamoff>(block.offset));
    file.read(reinterpret_cast<char*>(destination.data()), static_cast<std::streamsize>(destination.size()));
    require(file.good(), "cannot read blocks.bin");
}

}
