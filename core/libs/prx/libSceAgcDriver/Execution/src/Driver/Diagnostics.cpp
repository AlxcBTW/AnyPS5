#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

double TraceMs() {
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count();
}

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(std::string("AGC driver: ") + reason);
    }
}

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset) {
    const auto it = registers.find(offset);
    require(it != registers.end(), "required shader register has not been written");
    return it->second;
}

void FlushStandardStreams() {
    std::fflush(stderr);
    std::fflush(stdout);
}

// stderr is unbuffered, and MinGW's printf writes such a stream one character at a time: every
// character of a direct fprintf(stderr) was a WriteFile (samp1: about 10% of the queue-0 worker).
// With the buffer one write takes up to 64 KiB. ProfileOutput's writer flushes after each batch,
// the [packets] report at each 10 s window, Driver::stop at shutdown and libc's terminate path; a
// crash or abort is reported and flushed by libc's handlers (CrashReport.cpp), and std::terminate
// ends in abort. A process killed from outside loses what is buffered since the last flush.
void BufferStandardError() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (std::getenv("APS5_STDERR_UNBUFFERED") == nullptr) std::setvbuf(stderr, nullptr, _IOFBF, 64 * 1024);
    });
}

std::uint32_t readUserData(const Registers& shader, std::uint32_t offset) {
    const auto it = shader.find(offset);
    return it == shader.end() ? 0u : it->second;
}

}
