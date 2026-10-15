#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINEPRELOAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINEPRELOAD_HPP

#include "prx/libSceAgcDriver/Execution/include/ComputePipelineCapture.hpp"
#include <atomic>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver {

struct PipelineContentKey {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    auto operator<=>(const PipelineContentKey&) const = default;
};

PipelineContentKey HashPipelineWords(std::span<const std::uint32_t> words);

struct ComputePipelineSettings {
    std::vector<std::uint32_t> layoutKey;
    std::vector<ComputeDescriptorBinding> bindings;
    bool updateAfterBind = false;
    bool workgroupMemory = false;
    bool pushConstants = false;
    std::vector<ComputeSpecialization> specialization;
    std::uint32_t subgroupSize = 0;
    VkPipelineCreateFlags flags = 0;
};

PipelineContentKey ComputePipelineKey(const PipelineContentKey& spirv, std::size_t spirvWords, const ComputePipelineSettings& settings);

struct PipelineManifestEntry {
    PipelineContentKey key;
    PipelineContentKey spirv;
    std::uint32_t spirvWords = 0;
    ComputePipelineSettings settings;
};

std::vector<std::byte> EncodePipelineManifestEntry(const PipelineManifestEntry& entry);

enum class PipelineRecordStatus { Decoded, Skipped, Truncated };

PipelineRecordStatus DecodePipelineManifestEntry(std::span<const std::byte> bytes, std::size_t& consumed, PipelineManifestEntry& entry);

class PipelineManifest {
public:
    PipelineManifest(std::filesystem::path directory, std::string name);
    ~PipelineManifest();
    PipelineManifest(const PipelineManifest&) = delete;
    PipelineManifest& operator=(const PipelineManifest&) = delete;

    std::vector<PipelineManifestEntry> Load();
    bool LoadSpirv(const PipelineManifestEntry& entry, std::vector<std::uint32_t>& spirv) const;
    void Record(const PipelineManifestEntry& entry, std::span<const std::uint32_t> spirv);
    void Flush();
    std::filesystem::path File() const { return file; }
    std::size_t Recorded() const;

private:
    struct Pending {
        PipelineManifestEntry entry;
        std::vector<std::uint32_t> spirv;
    };
    void run();
    std::filesystem::path blobPath(const PipelineContentKey& spirv) const;

    std::filesystem::path directory;
    std::filesystem::path file;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::condition_variable drained;
    std::deque<Pending> queue;
    std::set<PipelineContentKey> keys;
    std::set<PipelineContentKey> blobs;
    std::size_t recorded = 0;
    bool busy = false;
    bool stopping = false;
    std::thread writer;
    std::ofstream output;
};

template<typename TValue>
class PipelineClaims {
public:
    struct Claim {
        std::shared_ptr<TValue> ready;
        std::shared_future<std::shared_ptr<TValue>> pending;
        bool owned = false;
    };

    Claim Acquire(const PipelineContentKey& key) {
        std::lock_guard lock(mutex);
        auto& slot = slots[key];
        if (slot.value != nullptr) return {slot.value, {}, false};
        if (slot.future.valid()) return {nullptr, slot.future, false};
        slot.promise = std::make_shared<std::promise<std::shared_ptr<TValue>>>();
        slot.future = slot.promise->get_future().share();
        return {nullptr, {}, true};
    }

    bool TryClaim(const PipelineContentKey& key) {
        return Acquire(key).owned;
    }

    void Fulfill(const PipelineContentKey& key, std::shared_ptr<TValue> value) {
        std::shared_ptr<std::promise<std::shared_ptr<TValue>>> promise;
        {
            std::lock_guard lock(mutex);
            auto& slot = slots[key];
            slot.value = value;
            promise = std::move(slot.promise);
            slot.future = {};
        }
        if (promise != nullptr) promise->set_value(std::move(value));
    }

    void Fail(const PipelineContentKey& key, std::exception_ptr error) {
        std::shared_ptr<std::promise<std::shared_ptr<TValue>>> promise;
        {
            std::lock_guard lock(mutex);
            const auto found = slots.find(key);
            if (found == slots.end()) return;
            promise = std::move(found->second.promise);
            slots.erase(found);
        }
        if (promise != nullptr) promise->set_exception(error);
    }

    std::shared_ptr<TValue> Find(const PipelineContentKey& key) const {
        std::lock_guard lock(mutex);
        const auto found = slots.find(key);
        return found == slots.end() ? nullptr : found->second.value;
    }

    std::size_t Ready() const {
        std::lock_guard lock(mutex);
        std::size_t count = 0;
        for (const auto& [key, slot] : slots) count += slot.value != nullptr ? 1u : 0u;
        return count;
    }

    void Clear() {
        std::lock_guard lock(mutex);
        slots.clear();
    }

private:
    struct Slot {
        std::shared_ptr<TValue> value;
        std::shared_ptr<std::promise<std::shared_ptr<TValue>>> promise;
        std::shared_future<std::shared_ptr<TValue>> future;
    };
    mutable std::mutex mutex;
    std::map<PipelineContentKey, Slot> slots;
};

struct PipelinePreloadTask {
    double cost = 0;
    std::function<void()> run;
};

class PipelinePreloadPool {
public:
    PipelinePreloadPool(std::vector<PipelinePreloadTask> tasks, unsigned threads, bool lowPriority);
    ~PipelinePreloadPool();
    PipelinePreloadPool(const PipelinePreloadPool&) = delete;
    PipelinePreloadPool& operator=(const PipelinePreloadPool&) = delete;

    void Stop();
    void Wait();
    std::size_t Done() const { return done.load(std::memory_order_acquire); }
    std::size_t Total() const { return tasks.size(); }
    double ElapsedMs() const;

private:
    void work(bool lowPriority);

    std::vector<PipelinePreloadTask> tasks;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable finished;
    std::size_t running = 0;
    std::chrono::steady_clock::time_point started;
    std::atomic<double> elapsedMs{0};
    std::vector<std::thread> threads;
};

struct PipelinePreloadProgress {
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    bool active = false;
};

double ProcessSeconds();

std::uint32_t PipelineSpirvNonce();

std::span<const std::uint32_t> NoncedSpirv(std::span<const std::uint32_t> spirv, std::vector<std::uint32_t>& storage);

PipelinePreloadProgress CurrentPipelinePreload();
void PublishPipelinePreload(const PipelinePreloadProgress& progress);

}

#endif
