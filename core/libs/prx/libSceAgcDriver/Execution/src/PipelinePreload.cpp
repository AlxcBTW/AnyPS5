#include "prx/libSceAgcDriver/Execution/include/PipelinePreload.hpp"
#include "ShaderCacheDirectory.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace AgcDriver {
namespace {

constexpr std::uint32_t RecordMagic = 0x4c505041u;
constexpr std::uint32_t RecordVersion = 1;
constexpr std::size_t RecordHeaderBytes = 24;
constexpr std::uint64_t RecordSeed = 0x5eed0101ull;
constexpr std::uint64_t WordSeedFirst = 0x5eed0201ull;
constexpr std::uint64_t WordSeedSecond = 0x5eed0202ull;
constexpr std::uint64_t KeySeedFirst = 0x5eed0301ull;
constexpr std::uint64_t KeySeedSecond = 0x5eed0302ull;
constexpr std::uint32_t MaxListWords = 1u << 20u;

void AppendWord(std::vector<std::uint32_t>& words, std::uint64_t value) {
    words.push_back(static_cast<std::uint32_t>(value));
}

void AppendWide(std::vector<std::uint32_t>& words, std::uint64_t value) {
    words.push_back(static_cast<std::uint32_t>(value));
    words.push_back(static_cast<std::uint32_t>(value >> 32u));
}

void AppendSettings(std::vector<std::uint32_t>& words, const ComputePipelineSettings& settings) {
    AppendWord(words, settings.layoutKey.size());
    words.insert(words.end(), settings.layoutKey.begin(), settings.layoutKey.end());
    AppendWord(words, settings.bindings.size());
    for (const auto& binding : settings.bindings) {
        AppendWord(words, binding.binding);
        AppendWord(words, static_cast<std::uint32_t>(binding.type));
        AppendWord(words, binding.count);
        AppendWord(words, binding.stages);
        AppendWord(words, binding.flags);
    }
    AppendWord(words, settings.updateAfterBind ? 1u : 0u);
    AppendWord(words, settings.workgroupMemory ? 1u : 0u);
    AppendWord(words, settings.pushConstants ? 1u : 0u);
    AppendWord(words, settings.specialization.size());
    for (const auto& value : settings.specialization) {
        AppendWord(words, value.id);
        AppendWord(words, value.value);
    }
    AppendWord(words, settings.subgroupSize);
    AppendWord(words, settings.flags);
}

std::span<const std::byte> AsBytes(std::span<const std::uint32_t> words) {
    return std::as_bytes(words);
}

class Reader {
public:
    explicit Reader(std::span<const std::uint32_t> words) : words(words) {}
    bool Word(std::uint32_t& value) {
        if (cursor >= words.size()) return false;
        value = words[cursor++];
        return true;
    }
    bool Wide(std::uint64_t& value) {
        std::uint32_t low = 0, high = 0;
        if (!Word(low) || !Word(high)) return false;
        value = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32u);
        return true;
    }
    bool Count(std::uint32_t& value, std::size_t wordsEach) {
        if (!Word(value) || value > MaxListWords) return false;
        return static_cast<std::size_t>(value) * wordsEach <= words.size() - cursor;
    }
    bool Finished() const { return cursor == words.size(); }

private:
    std::span<const std::uint32_t> words;
    std::size_t cursor = 0;
};

bool ReadSettings(Reader& reader, ComputePipelineSettings& settings) {
    std::uint32_t count = 0;
    if (!reader.Count(count, 1)) return false;
    settings.layoutKey.resize(count);
    for (auto& word : settings.layoutKey) reader.Word(word);
    if (!reader.Count(count, 5)) return false;
    settings.bindings.resize(count);
    for (auto& binding : settings.bindings) {
        std::uint32_t type = 0;
        reader.Word(binding.binding);
        reader.Word(type);
        reader.Word(binding.count);
        reader.Word(binding.stages);
        reader.Word(binding.flags);
        binding.type = static_cast<VkDescriptorType>(type);
    }
    std::uint32_t updateAfterBind = 0, workgroupMemory = 0, pushConstants = 0;
    if (!reader.Word(updateAfterBind) || !reader.Word(workgroupMemory) || !reader.Word(pushConstants)) return false;
    if (updateAfterBind > 1 || workgroupMemory > 1 || pushConstants > 1) return false;
    settings.updateAfterBind = updateAfterBind != 0;
    settings.workgroupMemory = workgroupMemory != 0;
    settings.pushConstants = pushConstants != 0;
    if (!reader.Count(count, 2)) return false;
    settings.specialization.resize(count);
    for (auto& value : settings.specialization) {
        reader.Word(value.id);
        reader.Word(value.value);
    }
    std::uint32_t flags = 0;
    if (!reader.Word(settings.subgroupSize) || !reader.Word(flags)) return false;
    settings.flags = flags;
    return true;
}

std::string Hex(const PipelineContentKey& key) {
    char text[33];
    std::snprintf(text, sizeof(text), "%016llx%016llx", static_cast<unsigned long long>(key.first), static_cast<unsigned long long>(key.second));
    return text;
}

std::atomic<std::uint64_t> progressDone{0};
std::atomic<std::uint64_t> progressTotal{0};
std::atomic<bool> progressActive{false};

}

PipelineContentKey HashPipelineWords(std::span<const std::uint32_t> words) {
    const auto bytes = AsBytes(words);
    return {ShaderRecompiler::HashBytes(bytes, WordSeedFirst), ShaderRecompiler::HashBytes(bytes, WordSeedSecond)};
}

PipelineContentKey ComputePipelineKey(const PipelineContentKey& spirv, std::size_t spirvWords, const ComputePipelineSettings& settings) {
    std::vector<std::uint32_t> words;
    AppendWide(words, spirv.first);
    AppendWide(words, spirv.second);
    AppendWide(words, spirvWords);
    AppendSettings(words, settings);
    const auto bytes = AsBytes(words);
    return {ShaderRecompiler::HashBytes(bytes, KeySeedFirst), ShaderRecompiler::HashBytes(bytes, KeySeedSecond)};
}

std::vector<std::byte> EncodePipelineManifestEntry(const PipelineManifestEntry& entry) {
    std::vector<std::uint32_t> payload;
    AppendWide(payload, entry.key.first);
    AppendWide(payload, entry.key.second);
    AppendWide(payload, entry.spirv.first);
    AppendWide(payload, entry.spirv.second);
    AppendWord(payload, entry.spirvWords);
    AppendSettings(payload, entry.settings);
    const auto payloadBytes = AsBytes(std::span<const std::uint32_t>(payload));
    std::vector<std::uint32_t> header;
    AppendWord(header, RecordMagic);
    AppendWord(header, RecordVersion);
    AppendWord(header, payloadBytes.size());
    AppendWord(header, 0);
    AppendWide(header, ShaderRecompiler::HashBytes(payloadBytes, RecordSeed));
    std::vector<std::byte> record(RecordHeaderBytes + payloadBytes.size());
    std::memcpy(record.data(), header.data(), RecordHeaderBytes);
    std::memcpy(record.data() + RecordHeaderBytes, payloadBytes.data(), payloadBytes.size());
    return record;
}

PipelineRecordStatus DecodePipelineManifestEntry(std::span<const std::byte> bytes, std::size_t& consumed, PipelineManifestEntry& entry) {
    consumed = 0;
    if (bytes.size() < RecordHeaderBytes) return PipelineRecordStatus::Truncated;
    std::uint32_t header[6];
    std::memcpy(header, bytes.data(), RecordHeaderBytes);
    if (header[0] != RecordMagic || header[1] != RecordVersion || header[3] != 0 || header[2] % 4 != 0) return PipelineRecordStatus::Truncated;
    if (header[2] > bytes.size() - RecordHeaderBytes) return PipelineRecordStatus::Truncated;
    consumed = RecordHeaderBytes + header[2];
    const auto payloadBytes = bytes.subspan(RecordHeaderBytes, header[2]);
    const auto hash = static_cast<std::uint64_t>(header[4]) | (static_cast<std::uint64_t>(header[5]) << 32u);
    if (ShaderRecompiler::HashBytes(payloadBytes, RecordSeed) != hash) return PipelineRecordStatus::Skipped;
    std::vector<std::uint32_t> payload(header[2] / 4);
    std::memcpy(payload.data(), payloadBytes.data(), payloadBytes.size());
    Reader reader(payload);
    PipelineManifestEntry decoded;
    std::uint32_t spirvWords = 0;
    if (!reader.Wide(decoded.key.first) || !reader.Wide(decoded.key.second) || !reader.Wide(decoded.spirv.first) || !reader.Wide(decoded.spirv.second) || !reader.Word(spirvWords)) return PipelineRecordStatus::Skipped;
    decoded.spirvWords = spirvWords;
    if (!ReadSettings(reader, decoded.settings) || !reader.Finished()) return PipelineRecordStatus::Skipped;
    if (decoded.spirvWords < 5 || decoded.spirvWords > ComputePipelineCapture::MaxSpirvWords) return PipelineRecordStatus::Skipped;
    if (ComputePipelineKey(decoded.spirv, decoded.spirvWords, decoded.settings) != decoded.key) return PipelineRecordStatus::Skipped;
    entry = std::move(decoded);
    return PipelineRecordStatus::Decoded;
}

PipelineManifest::PipelineManifest(std::filesystem::path directory, std::string name) : directory(std::move(directory)), file(this->directory / name) {}

PipelineManifest::~PipelineManifest() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    changed.notify_all();
    if (writer.joinable()) writer.join();
}

std::filesystem::path PipelineManifest::blobPath(const PipelineContentKey& spirv) const {
    return directory / (Hex(spirv) + ".spv");
}

std::vector<PipelineManifestEntry> PipelineManifest::Load() {
    std::vector<std::byte> bytes;
    std::vector<PipelineManifestEntry> entries;
    if (!ShaderRecompiler::ReadWholeFile(file, bytes)) return entries;
    std::span<const std::byte> rest(bytes);
    std::set<PipelineContentKey> seen;
    while (!rest.empty()) {
        std::size_t consumed = 0;
        PipelineManifestEntry entry;
        const auto status = DecodePipelineManifestEntry(rest, consumed, entry);
        if (status == PipelineRecordStatus::Truncated) break;
        rest = rest.subspan(consumed);
        if (status == PipelineRecordStatus::Decoded && seen.insert(entry.key).second) entries.push_back(std::move(entry));
    }
    std::lock_guard lock(mutex);
    for (const auto& entry : entries) {
        keys.insert(entry.key);
        blobs.insert(entry.spirv);
    }
    return entries;
}

bool PipelineManifest::LoadSpirv(const PipelineManifestEntry& entry, std::vector<std::uint32_t>& spirv) const {
    std::vector<std::byte> bytes;
    if (!ShaderRecompiler::ReadWholeFile(blobPath(entry.spirv), bytes)) return false;
    if (bytes.size() != static_cast<std::size_t>(entry.spirvWords) * sizeof(std::uint32_t)) return false;
    spirv.resize(entry.spirvWords);
    std::memcpy(spirv.data(), bytes.data(), bytes.size());
    return HashPipelineWords(spirv) == entry.spirv;
}

void PipelineManifest::Record(const PipelineManifestEntry& entry, std::span<const std::uint32_t> spirv) {
    {
        std::lock_guard lock(mutex);
        if (stopping || !keys.insert(entry.key).second) return;
        queue.push_back({entry, std::vector<std::uint32_t>(spirv.begin(), spirv.end())});
        if (!writer.joinable()) writer = std::thread([this] { run(); });
    }
    changed.notify_one();
}

void PipelineManifest::Flush() {
    std::unique_lock lock(mutex);
    drained.wait(lock, [&] { return queue.empty() && !busy; });
}

std::size_t PipelineManifest::Recorded() const {
    std::lock_guard lock(mutex);
    return recorded;
}

void PipelineManifest::run() {
    std::unique_lock lock(mutex);
    for (;;) {
        changed.wait(lock, [&] { return stopping || !queue.empty(); });
        if (queue.empty()) return;
        auto pending = std::move(queue.front());
        queue.pop_front();
        busy = true;
        const bool needsBlob = blobs.insert(pending.entry.spirv).second;
        lock.unlock();
        bool stored = true;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (needsBlob) stored = ShaderRecompiler::WriteFileAtomically(blobPath(pending.entry.spirv), AsBytes(std::span<const std::uint32_t>(pending.spirv)));
        if (stored) {
            if (!output.is_open()) output.open(file, std::ios::binary | std::ios::app);
            const auto record = EncodePipelineManifestEntry(pending.entry);
            output.write(reinterpret_cast<const char*>(record.data()), static_cast<std::streamsize>(record.size()));
            output.flush();
            stored = output.good();
        }
        lock.lock();
        if (!stored) {
            blobs.erase(pending.entry.spirv);
            std::fprintf(stderr, "[preload] could not record pipeline %s in %s\n", Hex(pending.entry.key).c_str(), file.string().c_str());
        } else {
            ++recorded;
        }
        busy = false;
        if (queue.empty()) drained.notify_all();
    }
}

PipelinePreloadPool::PipelinePreloadPool(std::vector<PipelinePreloadTask> tasks, unsigned threadCount, bool lowPriority) : tasks(std::move(tasks)), started(std::chrono::steady_clock::now()) {
    std::stable_sort(this->tasks.begin(), this->tasks.end(), [](const PipelinePreloadTask& a, const PipelinePreloadTask& b) { return a.cost > b.cost; });
    const auto count = std::max(1u, std::min<unsigned>(threadCount, static_cast<unsigned>(std::max<std::size_t>(this->tasks.size(), 1))));
    running = this->tasks.empty() ? 0 : count;
    if (this->tasks.empty()) return;
    for (unsigned index = 0; index < count; ++index) threads.emplace_back([this, lowPriority] { work(lowPriority); });
}

PipelinePreloadPool::~PipelinePreloadPool() {
    Stop();
}

void PipelinePreloadPool::Stop() {
    stopping.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
}

void PipelinePreloadPool::Wait() {
    std::unique_lock lock(mutex);
    finished.wait(lock, [&] { return running == 0; });
}

double PipelinePreloadPool::ElapsedMs() const {
    return elapsedMs.load(std::memory_order_acquire);
}

void PipelinePreloadPool::work(bool lowPriority) {
#ifdef _WIN32
    if (lowPriority) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    static_cast<void>(lowPriority);
#endif
    while (!stopping.load(std::memory_order_acquire)) {
        const auto index = next.fetch_add(1, std::memory_order_acq_rel);
        if (index >= tasks.size()) break;
        try {
            tasks[index].run();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[preload] task %zu failed: %s\n", index, error.what());
        }
        done.fetch_add(1, std::memory_order_acq_rel);
    }
    std::lock_guard lock(mutex);
    if (--running == 0) {
        elapsedMs.store(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), std::memory_order_release);
        finished.notify_all();
    }
}

std::uint32_t PipelineSpirvNonce() {
    static const std::uint32_t nonce = [] {
        const char* text = std::getenv("APS5_EXP_PIPELINE_NONCE");
        return text == nullptr ? 0u : static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16));
    }();
    return nonce;
}

std::span<const std::uint32_t> NoncedSpirv(std::span<const std::uint32_t> spirv, std::vector<std::uint32_t>& storage) {
    const auto nonce = PipelineSpirvNonce();
    if (nonce == 0 || spirv.size() < 5) return spirv;
    storage.assign(spirv.begin(), spirv.end());
    storage[2] ^= nonce;
    return storage;
}

double ProcessSeconds() {
#ifdef _WIN32
    static const std::uint64_t created = [] {
        FILETIME creation{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user);
        return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32u) | creation.dwLowDateTime;
    }();
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    const auto current = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32u) | now.dwLowDateTime;
    return current > created ? static_cast<double>(current - created) / 1e7 : 0.0;
#else
    static const auto created = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - created).count();
#endif
}

PipelinePreloadProgress CurrentPipelinePreload() {
    return {progressDone.load(std::memory_order_relaxed), progressTotal.load(std::memory_order_relaxed), progressActive.load(std::memory_order_relaxed)};
}

void PublishPipelinePreload(const PipelinePreloadProgress& progress) {
    progressDone.store(progress.done, std::memory_order_relaxed);
    progressTotal.store(progress.total, std::memory_order_relaxed);
    progressActive.store(progress.active, std::memory_order_relaxed);
}

}
