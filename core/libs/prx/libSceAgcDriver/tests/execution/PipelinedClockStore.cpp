#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("pipelined clock store: " + what);
}

std::uint64_t ClockNow() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10);
}

std::array<std::uint32_t, 6> ClockCopy(const volatile void* destination, bool wide) {
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    return {0xc0044000u, wide ? 0x06016209u : 0x06006209u, 0u, 0u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(static_cast<std::uint64_t>(address) >> 32u)};
}

std::array<std::uint32_t, 5> WriteData(const volatile void* destination, std::uint32_t value) {
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    return {0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(static_cast<std::uint64_t>(address) >> 32u), value};
}

void Submit(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&packet) == 0, "submission failed");
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
#endif
    try {
        alignas(256) static volatile std::uint64_t stamps[8];
        alignas(256) static volatile std::uint32_t narrow[2];
        alignas(256) static volatile std::uint32_t labels[8];
        for (auto& value : stamps) value = 0;
        narrow[0] = 0;
        narrow[1] = 0xdeadbeefu;
        for (auto& value : labels) value = 0;
        using namespace AgcDriver::DriverDetail;
        const auto storesBefore = storesOnCpu.load() + storesOnGpu.load() + storesBehindCompletions.load();
        const auto queuedBefore = clockStoresQueued.load();
        std::vector<std::uint32_t> words;
        const auto append = [&](const auto& packet) { words.insert(words.end(), packet.begin(), packet.end()); };
        const auto before = ClockNow();
        append(WriteData(&labels[0], 1));
        append(ClockCopy(&stamps[0], true));
        append(WriteData(&labels[1], 2));
        append(ClockCopy(&stamps[1], true));
        append(ClockCopy(&narrow[0], false));
        append(WriteData(&labels[2], 3));
        append(ClockCopy(&stamps[2], true));
        Submit(words);
        AgcDriverWaitIdle_nid_postfix();
        const auto after = ClockNow();
        check(labels[0] == 1 && labels[1] == 2 && labels[2] == 3, "a label before or between the clock stores did not land");
        check(stamps[0] >= before && stamps[1] >= stamps[0] && stamps[2] >= stamps[1] && stamps[2] <= after, "the clock stamps are not ordered inside the submission interval");
        check(narrow[0] != 0 && narrow[1] == 0xdeadbeefu, "the 32-bit clock store did not write only its low half");
        const auto stores = storesOnCpu.load() + storesOnGpu.load() + storesBehindCompletions.load() - storesBefore;
        check(stores == 4, "the clock stores were not committed (" + std::to_string(stores) + " committed stores)");
        check(clockStoresQueued.load() - queuedBefore == 4, "the clock stores were not queued on the draw committer (" + std::to_string(clockStoresQueued.load() - queuedBefore) + " queued)");
        LibcRunShutdown_nid_postfix();
        std::puts("pipelined clock store tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
