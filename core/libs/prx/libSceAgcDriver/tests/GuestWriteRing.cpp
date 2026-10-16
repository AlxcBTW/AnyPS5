#include "prx/libSceAgcDriver/Execution/include/GuestWriteRing.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver::GuestWriteRing;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string RingPath(const char* name) {
    return (std::filesystem::temp_directory_path() / (std::string("agc-guest-write-ring-") + name + ".bin")).string();
}

void CheckOffWithoutRing() {
    Close();
    Require(!Enabled(), "a ring is active without APS5_GUEST_WRITE_RING and without Open");
    Record(Kind::CpuStore, 0x1000, 16, 0);
}

void CheckOrderAndFields() {
    const auto path = RingPath("order");
    Require(Open(path, 8), "the ring file could not be mapped");
    Require(Enabled(), "an opened ring is not active");
    Record(Kind::CpuStore, 0x14c0054200, 8, 0x1234);
    Record(Kind::GpuNoted, 0x1a80014000, 4096, 0x5678);
    Record(Kind::StorageWriteBack, 0x1a80014000, std::uint64_t{1} << 40, 0x1a80014000);
    Close();
    Header header{};
    const auto entries = ReadFile(path, &header);
    Require(std::memcmp(header.magic, Magic, sizeof(Magic)) == 0 && header.entryBytes == sizeof(Entry) && header.capacity == 8 && header.next == 3, "the ring header is wrong");
    Require(entries.size() == 3, "three records do not read back as three entries");
    Require(entries[0].address == 0x14c0054200 && entries[0].bytes == 8 && entries[0].kind == static_cast<std::uint8_t>(Kind::CpuStore) && entries[0].tag == 0x1234, "the first entry is wrong");
    Require(entries[1].address == 0x1a80014000 && entries[1].bytes == 4096 && entries[1].kind == static_cast<std::uint8_t>(Kind::GpuNoted) && entries[1].tag == 0x5678, "the second entry is wrong");
    Require(entries[2].bytes == 0xffffffffu && entries[2].kind == static_cast<std::uint8_t>(Kind::StorageWriteBack) && entries[2].tag == 0x1a80014000, "a range past 4 GiB is not saturated");
    Require(entries[0].microseconds <= entries[1].microseconds && entries[1].microseconds <= entries[2].microseconds, "the entries' times go backwards");
    Require(entries[0].thread == entries[2].thread, "one thread's entries carry different thread tags");
    std::filesystem::remove(path);
}

void CheckWrapKeepsNewest() {
    const auto path = RingPath("wrap");
    Require(Open(path, 8), "the ring file could not be mapped");
    for (std::uint64_t k = 0; k < 21; ++k) Record(Kind::CpuStore, 0x10000 + k * 16, 16, k);
    Close();
    Header header{};
    const auto entries = ReadFile(path, &header);
    Require(header.next == 21, "the record counter does not count every record");
    Require(entries.size() == 8, "a wrapped ring does not read back as its capacity");
    for (std::size_t k = 0; k < entries.size(); ++k) Require(entries[k].tag == 13 + k && entries[k].address == 0x10000 + (13 + k) * 16, "a wrapped ring does not keep the newest entries oldest first");
    std::filesystem::remove(path);
}

void CheckConcurrentRecords() {
    const auto path = RingPath("threads");
    constexpr std::uint64_t Threads = 4;
    constexpr std::uint64_t PerThread = 2000;
    Require(Open(path, Threads * PerThread), "the ring file could not be mapped");
    std::vector<std::thread> workers;
    for (std::uint64_t t = 0; t < Threads; ++t) {
        workers.emplace_back([t] {
            for (std::uint64_t k = 0; k < PerThread; ++k) Record(Kind::GpuNoted, (t << 32u) + k * 8, 8, t);
        });
    }
    for (auto& worker : workers) worker.join();
    Close();
    const auto entries = ReadFile(path);
    Require(entries.size() == Threads * PerThread, "concurrent records were lost or overwritten");
    std::set<std::uint64_t> addresses;
    std::set<std::uint16_t> threads;
    for (const auto& entry : entries) {
        addresses.insert(entry.address);
        threads.insert(entry.thread);
    }
    Require(addresses.size() == Threads * PerThread, "two concurrent records share a slot");
    Require(threads.size() == Threads, "the entries of four threads do not carry four thread tags");
    std::filesystem::remove(path);
}

void CheckPathTagAndForeignFile() {
    const auto tag = PathTag("Unwatch");
    char text[9] = {};
    for (int k = 0; k < 8; ++k) text[k] = static_cast<char>((tag >> (8 * k)) & 0xffu);
    Require(std::string(text) == "Unwatch", "a path tag does not keep the path's characters");
    Require(PathTag("MarkWritten (page walk incomplete)") == PathTag("MarkWrit"), "a path tag keeps more than eight characters");
    Require(PathTag(nullptr) == 0, "a missing path has a tag");
    const auto path = RingPath("foreign");
    {
        std::ofstream file(path, std::ios::binary);
        file << "not a ring, just some bytes that are longer than a header of sixty-four bytes......................";
    }
    Require(ReadFile(path).empty(), "a file that is no ring reads as entries");
    Require(ReadFile(RingPath("missing")).empty(), "a missing file reads as entries");
    std::filesystem::remove(path);
}

}

int main() {
    try {
        CheckOffWithoutRing();
        CheckOrderAndFields();
        CheckWrapKeepsNewest();
        CheckConcurrentRecords();
        CheckPathTagAndForeignFile();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "guest write ring: all checks passed\n";
    return 0;
}
