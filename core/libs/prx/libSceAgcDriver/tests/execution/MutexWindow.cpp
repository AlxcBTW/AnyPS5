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

namespace GuestMemory = AgcDriver::GuestMemory;

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

bool OtherThreadCanTake(GuestMemory::GpuMutexType& mutex) {
    bool taken = false;
    std::thread other([&] {
        if (mutex.try_lock()) {
            taken = true;
            mutex.unlock();
        }
    });
    other.join();
    return taken;
}

}

int main() {
    SetEnvironment("APS5_LAB_MUTEX_WINDOW", "1");
    SetEnvironment("APS5_LAB_MUTEX_WINDOW_SECONDS", "1");
    const char* output = "mutex_window_test.stderr";
    try {
        Require(std::freopen(output, "w", stderr) != nullptr, "could not redirect stderr");
        Require(GuestMemory::MutexWindowEnabled() && !GuestMemory::MutexWindowVerify(), "the lab switch did not read as on without verify");
        auto& mutex = GuestMemory::GpuMutex();

        {
            GuestMemory::GpuMutexWindow window;
            Require(!window.Open() && !mutex.HeldByThisThread(), "a window opened without a hold");
        }
        {
            std::lock_guard hold(mutex);
            GuestMemory::GpuMutexWindow window;
            Require(window.Open(), "no window at depth 1");
            Require(!mutex.HeldByThisThread() && mutex.DepthOnThisThread() == 0, "the open window still holds the mutex");
            Require(OtherThreadCanTake(mutex), "another thread could not take the mutex during the window");
            window.Close();
            Require(mutex.HeldByThisThread() && mutex.DepthOnThisThread() == 1, "Close did not retake at depth 1");
            window.Close();
            Require(mutex.DepthOnThisThread() == 1, "a second Close changed the depth");
            Require(!OtherThreadCanTake(mutex), "another thread took the mutex after Close");
        }
        Require(!mutex.HeldByThisThread(), "the mutex stayed held after the hold ended");
        {
            std::lock_guard hold(mutex);
            {
                GuestMemory::GpuMutexWindow window;
                Require(window.Open(), "no second window at depth 1");
            }
            Require(mutex.HeldByThisThread() && mutex.DepthOnThisThread() == 1, "the destructor did not retake at depth 1");
        }
        {
            std::lock_guard outer(mutex);
            std::lock_guard inner(mutex);
            GuestMemory::GpuMutexWindow window;
            Require(!window.Open() && mutex.DepthOnThisThread() == 2, "a window opened at depth 2 or changed the depth");
        }
        {
            std::lock_guard hold(mutex);
            {
                const GuestMemory::MutexWindowFence fence;
                Require(GuestMemory::MutexWindowFence::Held(), "the fence is not held");
                {
                    const GuestMemory::MutexWindowFence nested;
                }
                Require(GuestMemory::MutexWindowFence::Held(), "a nested fence ended the outer one");
                GuestMemory::GpuMutexWindow window;
                Require(!window.Open() && mutex.DepthOnThisThread() == 1, "a window opened under a fence");
            }
            Require(!GuestMemory::MutexWindowFence::Held(), "the fence outlived its scope");
            GuestMemory::GpuMutexWindow window;
            Require(window.Open(), "no window after the fence ended");
        }
        {
            std::lock_guard hold(mutex);
            try {
                GuestMemory::GpuMutexWindow window;
                Require(window.Open(), "no window for the exception case");
                throw std::runtime_error("thrown inside the window");
            } catch (const std::runtime_error&) {
            }
            Require(mutex.HeldByThisThread() && mutex.DepthOnThisThread() == 1, "unwinding did not retake the mutex at depth 1");
        }

        GuestMemory::NoteMutexWindow(GuestMemory::MutexWindowEvent::VerifyDisagree);
        Require(GuestMemory::MutexWindowDisagreements() == 1, "the hard disagreement was not counted");
        GuestMemory::ReportMutexWindow();
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        GuestMemory::ReportMutexWindow();
        AgcDriver::ProfileOutput_nid_no_patch().Flush();
        std::fflush(stderr);

        std::ifstream file(output);
        std::stringstream buffer;
        buffer << file.rdbuf();
        const auto text = buffer.str();
        Require(text.find("[mutexwindow]") != std::string::npos, "no [mutexwindow] line:\n" + text);
        Require(text.find("windows opened 4,") != std::string::npos, "the opened windows are not 4:\n" + text);
        Require(text.find("refused: depth 2,") != std::string::npos, "the depth refusals are not 2:\n" + text);
        Require(text.find("leaf-lock 1,") != std::string::npos, "the fence refusal is not 1:\n" + text);
        Require(text.find("hard disagreements 1 (1 since start)") != std::string::npos, "the disagreement is not on the line:\n" + text);
    } catch (const std::exception& error) {
        std::fputs(error.what(), stdout);
        std::fputc(10, stdout);
        return 1;
    }
    std::puts("mutex window tests passed");
    return 0;
}
