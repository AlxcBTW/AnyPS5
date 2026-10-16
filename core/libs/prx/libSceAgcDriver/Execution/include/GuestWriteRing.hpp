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
    StorageTextureCreated = 9,
    TextureCreated = 10,
    StorageTextureUploaded = 11,
    StorageTextureDestroyed = 12,
    WriteBackRetired = 13,
    PublishRetired = 14,
    MappingChanged = 15,
    StorageWriteBackDecided = 16,
    StoragePending = 17,
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
// The life of the driver's images over guest memory, so a store can be set against what the title did with those bytes
// meanwhile: StorageTextureCreated, TextureCreated (a sampled texture) and StorageTextureDestroyed name the surface's
// guest span with SurfaceTag. StorageTextureUploaded (the guest bytes read into a storage image) and
// StorageWriteBackDecided (a write-back of the image's span decided, before its StorageWriteBack or StorageCpuWriteBack
// ranges) carry the address of their reason, a string literal of the driver module (the header holds its base; 0 when
// none was named). WriteBackRetired and PublishRetired name a range of a write-back or a shadow publish whose batch
// retired (its bytes have reached guest memory), tagged as the decision's entry; the store window check takes them
// (APS5_GUEST_WRITE_RACE). MappingChanged is a guest range whose mapping or protection changed, tagged with what changed
// it (libc's GuestAllocations::ChangeKind: 1 a map, 2 a free, 3 a protection change, 4 an unmap, 5 an image unregistered,
// 6 a release outside a mutation, the guest heap's); RecordMappingChange is the change observer that records it.
// StoragePending marks when a storage image's results became pending (a write marked units that were not pending): the
// marked units' range, tagged with the image's address; a GPU write marks every unit of the image (MarkDirty).
void Record(Kind kind, std::uint64_t address, std::uint64_t bytes, std::uint64_t tag);
void RecordMappingChange(std::uintptr_t address, std::size_t bytes, int kind);

// The tag of the surface kinds: width (bits 0-14), height (15-29), depth or last array slice (30-40), guest format
// (41-49), mip count (50-53), tile mode (54-58), dimension (59-61) and the source (62-63: a sampled texture read from guest
// memory 0, viewed from a storage image 1, a block copy of one 2, a depth copy of one 3; a storage image destroyed with
// results pending 1). Each field keeps its low bits.
constexpr std::uint64_t SurfaceTag(std::uint32_t width, std::uint32_t height, std::uint32_t depthOrLastArray, std::uint32_t format, std::uint32_t mipCount, std::uint32_t tileMode, std::uint32_t dimension, std::uint32_t source) {
    return (std::uint64_t{width} & 0x7fffu) | (std::uint64_t{height} & 0x7fffu) << 15u | (std::uint64_t{depthOrLastArray} & 0x7ffu) << 30u | (std::uint64_t{format} & 0x1ffu) << 41u |
           (std::uint64_t{mipCount} & 0xfu) << 50u | (std::uint64_t{tileMode} & 0x1fu) << 54u | (std::uint64_t{dimension} & 0x7u) << 59u | (std::uint64_t{source} & 0x3u) << 62u;
}
bool Enabled();
std::uint64_t PathTag(const char* path);

bool Open(const std::string& path, std::uint64_t capacity);
void Close();
// The entries of a ring file, oldest first; empty when the file is not a ring.
std::vector<Entry> ReadFile(const std::string& path, Header* header = nullptr);

}

#endif
