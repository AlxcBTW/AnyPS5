// The GpuMutex hold profile (APS5_LAB_HOLD_PROFILE, lab step 0 of the mutex split): spans are
// exclusive of the spans nested in them, events and holds are charged to the thread's hold site,
// the profile line reaches the profile writer, and a profiled mutex still behaves as before
// (recursion, ownership). No device needed.
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// The number after `key` in `text` (after the first `from`), or -1.
double Field(const std::string& text, const std::string& from, const std::string& key) {
    const auto start = text.find(from);
    if (start == std::string::npos) return -1;
    const auto at = text.find(key + " ", start);
    if (at == std::string::npos) return -1;
    return std::atof(text.c_str() + at + key.size() + 1);
}

}

int main() {
    SetEnvironment("APS5_PROFILE_DRAW", "1");
    SetEnvironment("APS5_LAB_HOLD_PROFILE", "1");
    SetEnvironment("APS5_LAB_HOLD_PROFILE_SECONDS", "1");
    const char* output = "hold_profile_test.stderr";
    try {
        Require(std::freopen(output, "w", stderr) != nullptr, "could not redirect stderr");
        Require(GuestMemory::HoldProfiled(), "the profile did not switch on");
        GuestMemory::TagGpuLockThread(0x20);
        auto& mutex = GuestMemory::GpuMutex();

        // Behavior with the profile on: ownership and recursion are what they were.
        Require(!mutex.HeldByThisThread() && mutex.DepthOnThisThread() == 0, "free mutex reported held");
        {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
            std::lock_guard outer(mutex);
            Require(mutex.HeldByThisThread() && mutex.DepthOnThisThread() == 1, "outermost hold not seen");
            {
                std::lock_guard inner(mutex);
                Require(mutex.DepthOnThisThread() == 2, "nested hold not seen");
            }
            Require(mutex.DepthOnThisThread() == 1, "nested unlock lost the outer hold");
        }
        Require(!mutex.HeldByThisThread(), "mutex still held after the unlocks");

        // 40 draw-like holds, each two back-to-back acquisitions; the record phase of one is
        // 5 ms of rest, 5 ms of command writes with a 30 ms GPU wait nested in the commands.
        const auto begin = std::chrono::steady_clock::now();
        int holds = 0;
        while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(2600)) {
            for (int pass = 0; pass < 2; ++pass) {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
                std::lock_guard lock(mutex);
                ++holds;
                if (pass == 0) {
                    GuestMemory::HoldSpan record(GuestMemory::HoldSpanKind::Rest);
                    SleepMs(5);
                    record.Switch(GuestMemory::HoldSpanKind::Command);
                    SleepMs(5);
                    {
                        const GuestMemory::HoldSpan wait(GuestMemory::HoldSpanKind::GpuWait);
                        SleepMs(30);
                    }
                    GuestMemory::NoteHoldEvent(GuestMemory::HoldEvent::Sync);
                    GuestMemory::NoteHoldEvent(GuestMemory::HoldEvent::SyncThrough);
                }
            }
            // Outside any hold: a wait made unlocked is charged to 'unlocked'.
            const GuestMemory::HoldSpan unlockedWait(GuestMemory::HoldSpanKind::GpuWait);
            SleepMs(1);
        }
        Require(holds >= 10, "the loop ran too few holds");
        AgcDriver::ProfileOutput_nid_no_patch().Flush();
        std::fflush(stderr);

        std::ifstream file(output);
        std::stringstream buffer;
        buffer << file.rdbuf();
        const auto text = buffer.str();
        const auto line = text.find("[holdprof] q0x20");
        Require(line != std::string::npos, "no [holdprof] line for the tagged thread:\n" + text);
        const auto rest = Field(text, "| draw", "record-rest");
        const auto command = Field(text, "| draw", "command");
        const auto wait = Field(text, "| draw", "gpu-wait");
        Require(rest > 0 && command > 0 && wait > 0, "a span kind is missing from the draw site:\n" + text);
        // The nested wait (30 ms) is not in the command span (5 ms): spans are exclusive.
        Require(wait > 2 * command, "the nested wait leaked into the outer span:\n" + text);
        Require(command > 0.8 * rest && command < 3.0 * rest, "rest and command differ more than their sleeps:\n" + text);
        Require(Field(text, "| draw", "sync") >= 1, "the sync event is missing:\n" + text);
        Require(Field(text, "| draw", "syncthrough") >= 1, "the syncthrough event is missing:\n" + text);
        Require(Field(text, "| draw", "adjacent-reacquire") >= 1, "no back-to-back reacquisition counted:\n" + text);
        Require(text.find("| unlocked") != std::string::npos, "the wait outside any hold was not charged to 'unlocked':\n" + text);
    } catch (const std::exception& error) {
        std::fputs(error.what(), stdout);
        std::fputc(10, stdout);
        return 1;
    }
    std::puts("hold profile tests passed");
    return 0;
}
