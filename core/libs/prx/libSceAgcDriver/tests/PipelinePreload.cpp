#include "prx/libSceAgcDriver/Execution/include/PipelinePreload.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace {

using namespace AgcDriver;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Directory {
public:
    Directory() {
        path = std::filesystem::temp_directory_path() / ("anyps5-pipeline-preload-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Require(std::filesystem::create_directory(path), "could not create test directory");
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

std::vector<std::uint32_t> Spirv(std::uint32_t seed, std::size_t words) {
    std::vector<std::uint32_t> spirv{0x07230203u, 0x00010300u, 0, 100, 0};
    for (std::size_t index = 5; index < words; ++index) spirv.push_back(static_cast<std::uint32_t>(index * 2654435761u) ^ seed);
    return spirv;
}

ComputePipelineSettings Settings() {
    ComputePipelineSettings settings;
    settings.layoutKey = {0, 7, 3, 32, 1, 7, 1, 32};
    settings.bindings = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, VK_SHADER_STAGE_COMPUTE_BIT, 0}, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0}};
    settings.workgroupMemory = true;
    settings.pushConstants = true;
    settings.specialization = {{4, 9}};
    settings.subgroupSize = 32;
    settings.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    return settings;
}

PipelineManifestEntry Entry(const std::vector<std::uint32_t>& spirv, const ComputePipelineSettings& settings) {
    PipelineManifestEntry entry;
    entry.spirv = HashPipelineWords(spirv);
    entry.spirvWords = static_cast<std::uint32_t>(spirv.size());
    entry.settings = settings;
    entry.key = ComputePipelineKey(entry.spirv, spirv.size(), settings);
    return entry;
}

bool SameEntry(const PipelineManifestEntry& a, const PipelineManifestEntry& b) {
    return EncodePipelineManifestEntry(a) == EncodePipelineManifestEntry(b);
}

void CheckKeys() {
    const auto spirv = Spirv(1, 64);
    const auto base = Settings();
    const auto key = ComputePipelineKey(HashPipelineWords(spirv), spirv.size(), base);
    Require(key == ComputePipelineKey(HashPipelineWords(spirv), spirv.size(), base), "the content key is not stable");
    Require(key != ComputePipelineKey(HashPipelineWords(Spirv(2, 64)), spirv.size(), base), "the SPIR-V did not change the key");
    Require(key != ComputePipelineKey(HashPipelineWords(spirv), spirv.size() + 1, base), "the SPIR-V size did not change the key");
    std::vector<ComputePipelineSettings> changed(9, base);
    changed[0].layoutKey.push_back(0x1000);
    changed[1].bindings[0].flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    changed[2].updateAfterBind = true;
    changed[3].workgroupMemory = false;
    changed[4].pushConstants = false;
    changed[5].specialization[0].value = 10;
    changed[6].subgroupSize = 0;
    changed[7].flags = 0;
    changed[8].bindings[1].count = 2;
    for (const auto& settings : changed) Require(key != ComputePipelineKey(HashPipelineWords(spirv), spirv.size(), settings), "a setting did not change the key");
}

void CheckRecords() {
    const auto spirv = Spirv(3, 80);
    const auto entry = Entry(spirv, Settings());
    const auto record = EncodePipelineManifestEntry(entry);
    std::size_t consumed = 0;
    PipelineManifestEntry decoded;
    Require(DecodePipelineManifestEntry(record, consumed, decoded) == PipelineRecordStatus::Decoded && consumed == record.size(), "a record did not decode");
    Require(SameEntry(decoded, entry), "a record round trip changed the entry");
    Require(DecodePipelineManifestEntry(std::span(record).first(record.size() - 4), consumed, decoded) == PipelineRecordStatus::Truncated, "a truncated record was not reported");
    auto corrupted = record;
    corrupted.back() ^= std::byte{1};
    Require(DecodePipelineManifestEntry(corrupted, consumed, decoded) == PipelineRecordStatus::Skipped && consumed == record.size(), "a corrupted record was not skipped whole");
    auto forged = entry;
    forged.settings.subgroupSize = 64;
    const auto forgedRecord = EncodePipelineManifestEntry(forged);
    Require(DecodePipelineManifestEntry(forgedRecord, consumed, decoded) == PipelineRecordStatus::Skipped, "a record whose key does not match its inputs was accepted");
}

void CheckManifest() {
    Directory directory;
    const auto first = Spirv(5, 300);
    const auto second = Spirv(6, 20);
    auto other = Settings();
    other.pushConstants = false;
    const auto a = Entry(first, Settings());
    const auto b = Entry(second, other);
    const auto c = Entry(first, other);
    {
        PipelineManifest manifest(directory.path, "compute-test.manifest");
        Require(manifest.Load().empty(), "an absent manifest was not empty");
        manifest.Record(a, first);
        manifest.Record(b, second);
        manifest.Record(a, first);
        manifest.Record(c, first);
        manifest.Flush();
        Require(manifest.Recorded() == 3, "a repeated key was recorded twice");
    }
    {
        std::ofstream tail(directory.path / "compute-test.manifest", std::ios::binary | std::ios::app);
        tail.write("APPL", 4);
    }
    PipelineManifest manifest(directory.path, "compute-test.manifest");
    const auto entries = manifest.Load();
    Require(entries.size() == 3 && SameEntry(entries[0], a) && SameEntry(entries[1], b) && SameEntry(entries[2], c), "the manifest did not load its records in order");
    std::vector<std::uint32_t> loaded;
    Require(manifest.LoadSpirv(entries[0], loaded) && loaded == first, "a blob did not load");
    Require(manifest.LoadSpirv(entries[1], loaded) && loaded == second, "a blob did not load");
    std::size_t blobs = 0;
    for (const auto& item : std::filesystem::directory_iterator(directory.path)) blobs += item.path().extension() == ".spv" ? 1u : 0u;
    Require(blobs == 2, "a shared SPIR-V was stored twice");
    manifest.Record(a, first);
    manifest.Flush();
    Require(manifest.Recorded() == 0, "a loaded key was recorded again");
    for (const auto& item : std::filesystem::directory_iterator(directory.path)) {
        if (item.path().extension() != ".spv") continue;
        std::ofstream blob(item.path(), std::ios::binary | std::ios::trunc);
        blob.write("garbage!", 8);
    }
    Require(!manifest.LoadSpirv(entries[0], loaded), "a changed blob was accepted");
}

void CheckClaims() {
    PipelineClaims<int> claims;
    const PipelineContentKey key{1, 2};
    const auto first = claims.Acquire(key);
    Require(first.owned && first.ready == nullptr, "the first acquire did not own the key");
    const auto second = claims.Acquire(key);
    Require(!second.owned && second.pending.valid(), "a second acquire did not wait for the first");
    Require(!claims.TryClaim(key), "a claimed key was claimed again");
    claims.Fail(key, std::make_exception_ptr(std::runtime_error("failed")));
    bool threw = false;
    try {
        static_cast<void>(second.pending.get());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw, "a waiter did not see the failure");
    const auto retry = claims.Acquire(key);
    Require(retry.owned, "a failed key could not be claimed again");
    const auto waiter = claims.Acquire(key);
    claims.Fulfill(key, std::make_shared<int>(7));
    Require(*waiter.pending.get() == 7, "a waiter did not get the value");
    const auto ready = claims.Acquire(key);
    Require(ready.ready != nullptr && *ready.ready == 7 && !ready.owned, "a fulfilled key was not ready");
    Require(claims.Ready() == 1 && *claims.Find(key) == 7, "the ready count or lookup is wrong");
}

void CheckPool() {
    std::vector<double> order;
    std::mutex mutex;
    std::vector<PipelinePreloadTask> tasks;
    for (const double cost : {3.0, 9.0, 1.0, 5.0}) tasks.push_back({cost, [&, cost] { std::lock_guard lock(mutex); order.push_back(cost); }});
    {
        PipelinePreloadPool pool(std::move(tasks), 1, false);
        pool.Wait();
        Require(pool.Done() == 4 && pool.Total() == 4, "a task did not run");
    }
    Require(order == std::vector<double>{9.0, 5.0, 3.0, 1.0}, "tasks did not run longest first");
    std::atomic<int> runs{0};
    std::vector<PipelinePreloadTask> many;
    for (int index = 0; index < 200; ++index) many.push_back({static_cast<double>(index % 7), [&] { runs.fetch_add(1); }});
    {
        PipelinePreloadPool pool(std::move(many), 8, true);
        pool.Wait();
    }
    Require(runs.load() == 200, "a task ran twice or not at all");
    std::atomic<bool> release{false};
    std::atomic<int> started{0};
    std::vector<PipelinePreloadTask> blocked;
    for (int index = 0; index < 6; ++index) blocked.push_back({1.0, [&] { started.fetch_add(1); while (!release.load()) std::this_thread::yield(); }});
    PipelinePreloadPool pool(std::move(blocked), 2, false);
    while (started.load() < 2) std::this_thread::yield();
    std::thread releaser([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); release.store(true); });
    pool.Stop();
    releaser.join();
    Require(started.load() <= 4, "a stopped pool kept starting tasks");
}

}

int main() {
    try {
        CheckKeys();
        CheckRecords();
        CheckManifest();
        CheckClaims();
        CheckPool();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pipeline preload test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
