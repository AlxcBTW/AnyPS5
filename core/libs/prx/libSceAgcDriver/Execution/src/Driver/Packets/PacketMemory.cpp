#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "HotLever.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace AgcDriver::DriverDetail {

namespace {

// exp (APS5_EXP_DMA_BEHIND=1, hot, never push; port of perf/dma-copy-behind-writers 59a4e2d6a): a
// DMA_DATA memory copy of up to 64 KiB whose source has an unsignaled GPU writer, or storage results
// pending over it, goes through VulkanDevice::CopyBuffer's GPU path, the one copies over 64 KiB
// already take: its barrier orders the copy behind those writes and the destination is noted as a
// pending write, instead of the CPU read that waits for the writers (the flush hook) and stores the
// bytes as a label. A copy CopyBuffer refuses (not imported) resolves on the CPU as before. Read once
// per packet. APS5_EXP_DMA_BEHIND_VERIFY=N: every Nth such copy syncs the recorder right after it and
// aborts unless the destination holds the source's bytes (what the CPU path would have stored).
std::int64_t leverInteger(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? static_cast<std::int64_t>(std::strtoll(value, nullptr, 10)) : 0;
}

bool dmaBehindWriters() {
    static const Aps5Hot::Lever lever("APS5_EXP_DMA_BEHIND", leverInteger("APS5_EXP_DMA_BEHIND"));
    return lever.On();
}

std::int64_t dmaBehindVerify() {
    static const Aps5Hot::Lever lever("APS5_EXP_DMA_BEHIND_VERIFY", leverInteger("APS5_EXP_DMA_BEHIND_VERIFY"));
    return lever.Value();
}

bool sourceWaitsForGpu(std::uint64_t source, std::size_t bytes) {
    if (auto* recorder = Graphics::Recorder::Active(); recorder != nullptr && recorder->PendingWriteOverlaps(source, bytes) && !recorder->PendingWriteSettled(source, bytes)) return true;
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{source, source + bytes}}};
    return Graphics::StorageTexture::AnyPendingOverlaps(range);
}

}

bool Driver::preparePacketMemory(const Submission& submission, QueueState& queue, std::span<const std::uint32_t> packet, std::uint32_t header, std::uint32_t opcode, bool& wroteOnGpu, bool& endOfPipeInterrupt, bool& interruptDeferred, bool& drawPacket, bool& sampleDump) {
    static const bool drainAll = std::getenv("APS5_DRAIN_ALL") != nullptr;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    wroteOnGpu = false;

    bool orderedAlready = false;

    const auto interruptSelect = opcode == 0x49 ? (packet[2] >> 24u) & 7u : 0u;
    endOfPipeInterrupt = interruptSelect != 0 && interruptSelect != 3;
    interruptDeferred = false;
    if (!drainAll && endOfPipeInterrupt) {
        const auto label = Pm4::DecodeLabelWrite(packet);
        const bool storesNothing = (packet[2] >> 29u) == 0 || (packet[3] | (static_cast<std::uint64_t>(packet[4]) << 32u)) == 0;
        if (label.has_value() || storesNothing) {
            bumpEpoch(&EpochBumps::drains);
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            const auto localDevice = device.Load();
            recordDeferredLabels(localDevice.get(), submission.queue);
            const bool workOpen = Graphics::Recorder::RecordedWorkSinceSubmit() != 0;
            int reason = localDevice != nullptr ? 0 : 1;
            if (label.has_value()) {
                const auto bytes = label->Bytes();
                const auto stamp = ++eventSerial;
                reason = localDevice != nullptr ? localDevice->WriteLabelOnGpu(label->address, bytes, stamp, submission.queue) : 4;
                if (reason == 1) GuestMemory::Write(label->address, bytes, 4);
                if (reason == 0 || reason == 1 || reason == 5) {
                    noteLabelStore(label->address, bytes, stamp, submission.queue);
                    Graphics::Recorder::CloseLabelGroup(GuestMemory::TrackerGeneration());
                }
                countLabelOutcome(reason);
                ++immediateLabels;
            } else {
                ++noOpLabels;
            }
            if (reason == 0 || reason == 5) {
                const auto queueId = submission.queue;
                interruptDeferred = localDevice->AfterRecordedWork([queueId] { AgcDriverDeliverEopInterrupt(queueId); }, submission.queue == 0);
                if (interruptDeferred && workOpen) localDevice->SubmitRecorded(submission.queue == 0);
                wroteOnGpu = true;
            } else if (reason == 1) {
                wroteOnGpu = true;
            }
        }
    }
    if (opcode == 0x50 && packet.size() >= 7) {
        if (Pm4::DmaSource(packet) == 0 || Pm4::DmaSource(packet) == 3) Graphics::RequireNoMultisampleTarget(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u), packet[6] & 0x3ffffffu, "a DMA copy");
        if (Pm4::DmaSource(packet) == 2 && Pm4::DmaDestination(packet) != 1) {
            const auto address = packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u);
            Graphics::NoteDepthMetadataFill(address, packet[6] & 0x3ffffffu, packet[2]);
            Graphics::NoteColorMetadataFill(address, packet[6] & 0x3ffffffu, packet[2]);
        }
    }
    if (!drainAll && !endOfPipeInterrupt && (opcode == 0x49 || opcode == 0x37)) {
        if (const auto label = Pm4::DecodeLabelWrite(packet)) {
            const auto bytes = label->Bytes();
            if (DeferLabels() && bytes.size() <= DeferredLabel::Capacity && bytes.size() % 4 == 0 && label->address % 4 == 0) {

                auto& deferred = deferredLabels();
                if (deferred.labels.empty()) deferred.since = std::chrono::steady_clock::now();
                auto& entry = deferred.labels.emplace_back();
                entry.address = label->address;
                entry.size = bytes.size();
                std::memcpy(entry.bytes.data(), bytes.data(), bytes.size());
                ++queuedLabels;
                wroteOnGpu = true;
            } else {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                const auto localDevice = device.Load();

                recordDeferredLabels(localDevice.get(), submission.queue);
                const auto stamp = ++eventSerial;
                const auto reason = localDevice != nullptr ? localDevice->WriteLabelOnGpu(label->address, bytes, stamp, submission.queue) : 4;
                wroteOnGpu = reason == 0 || reason == 5;
                if (reason == 1) {

                    GuestMemory::Write(label->address, bytes, 4);
                    wroteOnGpu = true;
                }
                if (wroteOnGpu) noteLabelStore(label->address, bytes, stamp, submission.queue);
                Graphics::Recorder::CloseLabelGroup(GuestMemory::TrackerGeneration());
                countLabelOutcome(reason);
                ++immediateLabels;
            }
        } else if (opcode == 0x49 ? ((packet[2] >> 29u) == 0 || (packet[3] | (static_cast<std::uint64_t>(packet[4]) << 32u)) == 0) : (packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u)) == 0) {
            orderedAlready = true;
            ++noOpLabels;
        } else {
            ++labelFallbacks[4];
        }
    }

    static const bool cpuStores = std::getenv("APS5_CPU_STORES") != nullptr;
    if (!drainAll && !cpuStores && (opcode == 0x40 || opcode == 0x50 || opcode == 0x83)) {
        constexpr std::size_t gpuStoreLimit = 65536;
        bool drained = true;
        bool copiedOnGpu = false;
        if (opcode == 0x50 && dmaBehindWriters()) {
            const auto copy = Pm4::DecodeMemoryCopy(packet);
            if (copy.has_value() && copy->bytes <= gpuStoreLimit && copy->destination % 4 == 0 && copy->bytes % 4 == 0 && GuestMemory::Accessible(reinterpret_cast<const void*>(copy->source), copy->bytes) && GuestMemory::Accessible(reinterpret_cast<const void*>(copy->destination), copy->bytes, true)) {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Copy);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                if (const auto localDevice = device.Load()) {
                    recordDeferredLabels(localDevice.get(), submission.queue);
                    if (sourceWaitsForGpu(copy->source, copy->bytes)) {
                        const auto outcome = localDevice->CopyBuffer(copy->destination, copy->source, copy->bytes, 0, std::numeric_limits<std::size_t>::max(), 0, 0, submission.queue, [](std::span<const std::byte>, std::uint64_t) {});
                        if (outcome.path == 1 || outcome.path == 3) {
                            const auto copied = ++copiesBehindWriters;
                            wroteOnGpu = true;
                            drained = false;
                            copiedOnGpu = true;
                            const auto verify = dmaBehindVerify();
                            const bool apart = copy->source + copy->bytes <= copy->destination || copy->destination + copy->bytes <= copy->source;
                            if (verify > 0 && apart && copied % static_cast<std::uint64_t>(verify) == 0) {
                                if (auto* recorder = Graphics::Recorder::Active()) recorder->Sync();
                                if (std::memcmp(reinterpret_cast<const void*>(copy->destination), reinterpret_cast<const void*>(copy->source), copy->bytes) != 0) {
                                    std::fprintf(stderr, "FATAL: [sync] DMA_BEHIND verify: the GPU copy 0x%llx -> 0x%llx (+0x%zx) left other bytes than the source holds\n", static_cast<unsigned long long>(copy->source), static_cast<unsigned long long>(copy->destination), copy->bytes);
                                    std::fflush(stderr);
                                    std::abort();
                                }
                                ++copiesBehindVerified;
                            }
                        }
                    }
                }
            }
        }
        if (const auto store = copiedOnGpu ? std::optional<Pm4::StoreWrite>{} : Pm4::ResolveStore(packet, queue, gpuStoreLimit)) {
            const auto bytes = store->Bytes();
            if (bytes.empty()) {

                orderedAlready = true;
                drained = false;
            } else {

                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                const auto localDevice = device.Load();

                Graphics::StorageTexture::FlushPending(store->address, bytes.size(), nullptr, "packet store", Graphics::PublishScope::PartialUnits);

                recordDeferredLabels(localDevice.get(), submission.queue);
                const auto stamp = ++eventSerial;
                const auto reason = localDevice != nullptr ? localDevice->WriteLabelOnGpu(store->address, bytes, stamp, submission.queue) : 4;
                if (reason == 0 || reason == 1 || reason == 5) noteLabelStore(store->address, bytes, stamp, submission.queue);
                if (reason == 0 || reason == 5) {
                    if (reason == 0) ++storesOnGpu;
                    else ++storesBehindCompletions;
                    wroteOnGpu = true;
                    drained = false;
                } else if (reason == 1) {
                    GuestMemory::Write(store->address, bytes, 1);
                    ++storesOnCpu;
                    wroteOnGpu = true;
                    drained = false;
                }
                Graphics::Recorder::CloseLabelGroup(GuestMemory::TrackerGeneration());
            }
        }
        if (drained && opcode == 0x50) {
            const auto copy = Pm4::DecodeMemoryCopy(packet);
            if (copy.has_value() && copy->bytes > gpuStoreLimit && GuestMemory::Accessible(reinterpret_cast<const void*>(copy->source), copy->bytes) && GuestMemory::Accessible(reinterpret_cast<const void*>(copy->destination), copy->bytes, true)) {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Copy);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                if (const auto localDevice = device.Load()) {
                    recordDeferredLabels(localDevice.get(), submission.queue);
                    const auto outcome = localDevice->CopyBuffer(copy->destination, copy->source, copy->bytes, 0, std::numeric_limits<std::size_t>::max(), 0, 0, submission.queue, [](std::span<const std::byte>, std::uint64_t) {});
                    if (outcome.path == 1 || outcome.path == 3) {
                        wroteOnGpu = true;
                        drained = false;
                    } else if (outcome.path == 2 && !Graphics::RegisteredReadableCovers(copy->destination, copy->bytes) && !localDevice->StoresPendingOver(copy->destination, copy->bytes)) {
                        Graphics::StorageTexture::FlushPending(copy->source, copy->bytes, nullptr, "buffer copy source", Graphics::PublishScope::Whole);
                        if (auto* recorder = Graphics::Recorder::Active()) recorder->SyncThrough(copy->source, copy->bytes);
                        ++copiesToHostMemory;
                        orderedAlready = true;
                        drained = false;
                    }
                }
            }
        }
        if (drained) ++storesDrained;
    }

    static const bool drawDrain = std::getenv("APS5_DRAW_DRAIN") != nullptr;
    drawPacket = Pm4::DrawOpcode(opcode);
    sampleDump = opcode == 0x46 && (packet[1] & 0x3fu) == 0x39u;
    if (sampleDump && !drainAll) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        if (const auto localDevice = device.Load()) {
            recordDeferredLabels(localDevice.get(), submission.queue);
            wroteOnGpu = localDevice->DumpSamplesOnGpu(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u));
        }
    }

    static const bool syncFlip = std::getenv("APS5_SYNC_FLIP") != nullptr;
    const bool drains = drainAll ? ((Pm4::AccessesMemory(header) && opcode != 0x16) || opcode == 0x42 || opcode == 0x46 || opcode == 0x58 || header == FlipPacketHeader)
                                 : (!wroteOnGpu && !orderedAlready && (opcode == 0x49 || opcode == 0x37 || opcode == 0x40 || opcode == 0x45 || opcode == 0x50 || opcode == 0x83 || sampleDump || (drawPacket && drawDrain) || (header == FlipPacketHeader && syncFlip)));
    if (drains) {

        static const bool unlockedDrain = std::getenv("APS5_NO_UNLOCKED_DRAIN") == nullptr && !drainAll;

        bumpEpoch(&EpochBumps::drains);
        std::shared_ptr<VulkanDevice> draining;
        std::uint64_t epoch = 0;
        {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            if (profile) {
                ++drainCounts[header == FlipPacketHeader ? 0xffffu : opcode];
                ++drainTotal;
                if (std::chrono::steady_clock::now() - lastSyncReport > std::chrono::seconds(10)) {
                    lastSyncReport = std::chrono::steady_clock::now();
                    reportSync();
                }
            }
            draining = device.Load();

            recordDeferredLabels(draining.get(), submission.queue);
            if (draining != nullptr) {
                if (unlockedDrain && draining->CanWaitUnlocked()) epoch = draining->SubmitAndEpoch();
                else draining->WaitIdle();
            }
            if (epoch == 0) draining.reset();
        }
        if (epoch != 0) {
            ++unlockedDrains;
            try {
                draining->WaitRecorded(epoch);
            } catch (...) {

                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                draining.reset();
                throw;
            }
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            draining->ReapRecorded(epoch);
            draining.reset();
        }
    }
    return drains;
}

void Driver::dumpSampleCounters(std::uint64_t address) {
    std::uint64_t samples = 0;
    {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        auto* recorder = Graphics::Recorder::Active();
        require(recorder != nullptr, "occlusion counters without the command recorder are not implemented");
        recorder->CountSamples();
        samples = recorder->SamplesTotal();
    }
    constexpr std::uint64_t ready = 1ull << 63u;
    for (std::uint64_t db = 0; db < 16; ++db) {
        const std::uint64_t value = ready | (db == 0 ? samples : 0u);
        GuestMemory::Write(address + db * 16u, std::as_bytes(std::span(&value, 1)), 8);
    }
}

}
