#include "prx/libSceAgcDriver/Execution/include/AsyncLog.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string ReadAll(std::FILE* file) {
    std::fflush(file);
    std::rewind(file);
    std::string text;
    char buffer[4096];
    for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) != 0;) text.append(buffer, read);
    return text;
}

void LockStream(std::FILE* file) {
#ifdef _WIN32
    _lock_file(file);
#else
    flockfile(file);
#endif
}

void UnlockStream(std::FILE* file) {
#ifdef _WIN32
    _unlock_file(file);
#else
    funlockfile(file);
#endif
}

class HeldStream {
public:
    explicit HeldStream(std::FILE* file) : file(file) {
        holder = std::thread([this] {
            LockStream(this->file);
            held.store(true);
            while (!releaseRequested.load()) std::this_thread::sleep_for(1ms);
            released.store(true);
            UnlockStream(this->file);
        });
        while (!held.load()) std::this_thread::sleep_for(1ms);
    }

    HeldStream(const HeldStream&) = delete;
    HeldStream& operator=(const HeldStream&) = delete;

    ~HeldStream() {
        Release();
        holder.join();
    }

    void Release() {
        releaseRequested.store(true);
    }

    bool Released() const {
        return released.load();
    }

private:
    std::FILE* file;
    std::atomic<bool> held{false};
    std::atomic<bool> releaseRequested{false};
    std::atomic<bool> released{false};
    std::thread holder;
};

std::string LargeLine(std::size_t index, std::size_t bytes) {
    std::string line = "large " + std::to_string(index) + " ";
    line.resize(bytes - 1, 'x');
    line += '\n';
    return line;
}

void OrderPerThread() {
    std::FILE* file = std::tmpfile();
    Check(file != nullptr, "temporary file");
    auto& log = AgcDriver::AsyncLog::Get();
    std::string expected;
    for (int i = 0; i < 2000; ++i) {
        log.Defer(file, "deferred %d %s %llu\n", i, AgcDriver::LogLiteral{"literal"}, static_cast<unsigned long long>(i) * 3ull);
        expected += "deferred " + std::to_string(i) + " literal " + std::to_string(static_cast<unsigned long long>(i) * 3ull) + "\n";
        const std::string plain = "plain " + std::to_string(i) + "\n";
        log.Write(file, plain.data(), plain.size());
        expected += plain;
    }
    log.Flush();
    Check(ReadAll(file) == expected, "deferred and written lines changed order or content");
    std::fclose(file);
}

void LongLine() {
    std::FILE* file = std::tmpfile();
    Check(file != nullptr, "temporary file");
    const std::string longText(3000, 'x');
    AgcDriver::AsyncLog::Get().Defer(file, "%s|%d\n", AgcDriver::LogLiteral{longText.c_str()}, 7);
    AgcDriver::AsyncLog::Get().Flush();
    Check(ReadAll(file) == longText + "|7\n", "a line longer than the stack buffer changed");
    std::fclose(file);
}

void Coalescing() {
    auto& log = AgcDriver::AsyncLog::Get();
    const std::string line = "[gpu] coalescing test line\n";
    int printed = 0;
    for (int i = 0; i < 50; ++i) printed += log.Admit(line) ? 1 : 0;
    Check(printed == 3, "identical lines in one window were not limited to the first three");
    Check(log.Admit("[gpu] another line\n"), "a different line was suppressed");
}

void FlushWaitsForHeldStream() {
    std::FILE* file = std::tmpfile();
    Check(file != nullptr, "temporary file");
    auto& log = AgcDriver::AsyncLog::Get();
    {
        HeldStream held(file);
        log.Defer(file, "held %d %s\n", 7, AgcDriver::LogLiteral{"line"});
        std::thread releaser([&] {
            std::this_thread::sleep_for(2400ms);
            held.Release();
        });
        log.Flush();
        const bool waited = held.Released();
        releaser.join();
        Check(waited, "Flush returned while the logger still waited for the stream");
    }
    Check(ReadAll(file) == "held 7 line\n", "the line queued behind a held stream changed");
    std::fclose(file);
}

void FullQueueBlocksCaller() {
    std::FILE* file = std::tmpfile();
    Check(file != nullptr, "temporary file");
    auto& log = AgcDriver::AsyncLog::Get();
    constexpr std::size_t lineBytes = 1024 * 1024;
    constexpr std::size_t lines = 2 * AgcDriver::AsyncLog::Capacity / lineBytes + 4;
    constexpr std::size_t filled = AgcDriver::AsyncLog::Capacity / lineBytes / 2;
    std::atomic<std::size_t> queued{0};
    {
        HeldStream held(file);
        std::thread producer([&] {
            for (std::size_t i = 0; i < lines; ++i) {
                const std::string line = LargeLine(i, lineBytes);
                log.Write(file, line.data(), line.size());
                queued.store(i + 1);
            }
        });
        const auto deadline = AgcDriver::AsyncLog::Clock::now() + 10s;
        while (queued.load() < filled && AgcDriver::AsyncLog::Clock::now() < deadline) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(300ms);
        const auto blockedAt = queued.load();
        std::this_thread::sleep_for(100ms);
        const bool blocked = blockedAt >= filled && blockedAt < lines && queued.load() == blockedAt;
        held.Release();
        producer.join();
        Check(blocked, "the caller was not blocked by a full queue");
    }
    log.Flush();
    std::fflush(file);
    std::rewind(file);
    std::string line;
    for (std::size_t i = 0; i < lines; ++i) {
        const std::string expected = LargeLine(i, lineBytes);
        line.resize(expected.size());
        Check(std::fread(line.data(), 1, line.size(), file) == line.size() && line == expected, "a line written through a full queue changed or went missing");
    }
    Check(std::fgetc(file) == EOF, "a full queue wrote extra output");
    std::fclose(file);
}

}

int main() {
    try {
        OrderPerThread();
        LongLine();
        Coalescing();
        FlushWaitsForHeldStream();
        FullQueueBlocksCaller();
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
    return 0;
}
