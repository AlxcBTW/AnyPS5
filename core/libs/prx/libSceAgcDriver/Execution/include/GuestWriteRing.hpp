#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTWRITERING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTWRITERING_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AgcDriver::GuestWriteRing {

// Lab diagnostics: APS5_GUEST_WRITE_RING=<file> keeps the driver's own writes into guest memory in a ring mapped from
// that file (APS5_GUEST_WRITE_RING_MIB, default 256 MiB, 32 bytes per entry), so the history of the last minutes outlives
// a crash of the process. Without the variable Record does nothing. lab/scripts/guestcrash-ring.py reads the file.
enum class Kind : std::uint8_t {
    CpuStore = 1,
    GpuNoted = 2,
    Unwatch = 3,
    StorageWriteBack = 4,
    StorageCpuWriteBack = 5,
    ShadowPublish = 6,
    WriteBackWindowCpuStore = 7,
    PublishWindowCpuStore = 8,
};

struct Entry {
    std::uint64_t address;
    std::uint32_t bytes;
    std::uint8_t kind;
    std::uint8_t reserved;
    std::uint16_t thread;
    std::uint64_t microseconds;
    std::uint64_t tag;
};

struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t entryBytes;
    std::uint64_t capacity;
    std::uint64_t moduleBase;
    std::uint64_t next;
    std::uint64_t reserved[3];
};

constexpr char Magic[8] = {'A', 'P', 'S', '5', 'W', 'R', 'G', '1'};

// CpuStore and GpuNoted carry the code address that called into GuestMemory as the tag, Unwatch up to eight characters
// of the path that took the range out of the write watch (PathTag), the storage kinds the image's guest address and
// ShadowPublish the import's base. The window kinds (GuestMemory::CheckStoreWindow) name a 64 KiB block that a CPU
// store reached between a GPU store's decision and the retire of its batch, with the tag of the store. Bytes past 4 GiB
// are recorded as 0xffffffff.
void Record(Kind kind, std::uint64_t address, std::uint64_t bytes, std::uint64_t tag);
bool Enabled();
std::uint64_t PathTag(const char* path);

bool Open(const std::string& path, std::uint64_t capacity);
void Close();
// The entries of a ring file, oldest first; empty when the file is not a ring.
std::vector<Entry> ReadFile(const std::string& path, Header* header = nullptr);

}

#endif
