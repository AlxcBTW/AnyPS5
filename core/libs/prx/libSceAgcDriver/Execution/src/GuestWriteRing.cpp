#include "prx/libSceAgcDriver/Execution/include/GuestWriteRing.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace AgcDriver::GuestWriteRing {
namespace {

static_assert(sizeof(Entry) == 32);
static_assert(sizeof(Header) == 64);

struct State {
    Header* header = nullptr;
    Entry* entries = nullptr;
    std::uint64_t capacity = 0;
    std::uint64_t mappedBytes = 0;
    std::chrono::steady_clock::time_point start;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#else
    int file = -1;
#endif
};

std::atomic<State*> opened{nullptr};

std::uint64_t moduleBase() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&moduleBase), &module)) return 0;
    return reinterpret_cast<std::uintptr_t>(module);
#else
    return 0;
#endif
}

void release(State* state) {
    if (state == nullptr) return;
#ifdef _WIN32
    if (state->header != nullptr) UnmapViewOfFile(state->header);
    if (state->mapping != nullptr) CloseHandle(state->mapping);
    if (state->file != INVALID_HANDLE_VALUE) CloseHandle(state->file);
#else
    if (state->header != nullptr) munmap(state->header, static_cast<std::size_t>(state->mappedBytes));
    if (state->file >= 0) close(state->file);
#endif
    delete state;
}

State* create(const std::string& path, std::uint64_t capacity) {
    if (capacity == 0 || capacity > (std::uint64_t{1} << 40) / sizeof(Entry)) return nullptr;
    auto* state = new State();
    state->capacity = capacity;
    state->mappedBytes = sizeof(Header) + capacity * sizeof(Entry);
#ifdef _WIN32
    state->file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (state->file == INVALID_HANDLE_VALUE) {
        release(state);
        return nullptr;
    }
    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(state->mappedBytes);
    if (!SetFilePointerEx(state->file, size, nullptr, FILE_BEGIN) || !SetEndOfFile(state->file)) {
        release(state);
        return nullptr;
    }
    state->mapping = CreateFileMappingA(state->file, nullptr, PAGE_READWRITE, static_cast<DWORD>(state->mappedBytes >> 32u), static_cast<DWORD>(state->mappedBytes & 0xffffffffu), nullptr);
    if (state->mapping == nullptr) {
        release(state);
        return nullptr;
    }
    state->header = static_cast<Header*>(MapViewOfFile(state->mapping, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(state->mappedBytes)));
#else
    state->file = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (state->file < 0 || ftruncate(state->file, static_cast<off_t>(state->mappedBytes)) != 0) {
        release(state);
        return nullptr;
    }
    void* view = mmap(nullptr, static_cast<std::size_t>(state->mappedBytes), PROT_READ | PROT_WRITE, MAP_SHARED, state->file, 0);
    state->header = view == MAP_FAILED ? nullptr : static_cast<Header*>(view);
#endif
    if (state->header == nullptr) {
        release(state);
        return nullptr;
    }
    state->entries = reinterpret_cast<Entry*>(state->header + 1);
    state->start = std::chrono::steady_clock::now();
    state->header->version = 1;
    state->header->entryBytes = sizeof(Entry);
    state->header->capacity = capacity;
    state->header->moduleBase = moduleBase();
    state->header->next = 0;
    std::memcpy(state->header->magic, Magic, sizeof(Magic));
    return state;
}

State* fromEnvironment() {
    static State* const state = []() -> State* {
        const char* path = std::getenv("APS5_GUEST_WRITE_RING");
        if (path == nullptr || *path == '\0') return nullptr;
        std::uint64_t mib = 256;
        if (const char* text = std::getenv("APS5_GUEST_WRITE_RING_MIB")) {
            const auto value = std::strtoull(text, nullptr, 10);
            if (value >= 1 && value <= 16384) mib = value;
        }
        auto* created = create(path, (mib << 20u) / sizeof(Entry));
        if (created == nullptr) std::fprintf(stderr, "[guestcrash] guest write ring %s could not be mapped; no ring\n", path);
        else std::fprintf(stderr, "[guestcrash] guest write ring %s: %llu entries, module base 0x%llx\n", path, static_cast<unsigned long long>(created->capacity), static_cast<unsigned long long>(created->header->moduleBase));
        return created;
    }();
    return state;
}

State* active() {
    if (auto* state = opened.load(std::memory_order_acquire)) return state;
    return fromEnvironment();
}

std::uint16_t threadTag() {
#ifdef _WIN32
    thread_local const auto tag = static_cast<std::uint16_t>(GetCurrentThreadId() & 0xffffu);
#else
    static std::atomic<std::uint16_t> nextTag{0};
    thread_local const auto tag = static_cast<std::uint16_t>(nextTag.fetch_add(1, std::memory_order_relaxed) + 1u);
#endif
    return tag;
}

}

void Record(Kind kind, std::uint64_t address, std::uint64_t bytes, std::uint64_t tag) {
    auto* state = active();
    if (state == nullptr) return;
    const auto index = std::atomic_ref<std::uint64_t>(state->header->next).fetch_add(1, std::memory_order_relaxed);
    auto& entry = state->entries[index % state->capacity];
    entry.address = address;
    entry.bytes = static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes, 0xffffffffu));
    entry.kind = static_cast<std::uint8_t>(kind);
    entry.reserved = 0;
    entry.thread = threadTag();
    entry.microseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - state->start).count());
    entry.tag = tag;
}

void RecordMappingChange(std::uintptr_t address, std::size_t bytes, int kind) {
    Record(Kind::MappingChanged, address, bytes, static_cast<std::uint64_t>(kind));
}

bool Enabled() {
    return active() != nullptr;
}

std::uint64_t PathTag(const char* path) {
    std::uint64_t tag = 0;
    if (path == nullptr) return 0;
    for (std::size_t index = 0; index < 8 && path[index] != '\0'; ++index) tag |= static_cast<std::uint64_t>(static_cast<unsigned char>(path[index])) << (8u * index);
    return tag;
}

bool Open(const std::string& path, std::uint64_t capacity) {
    auto* created = create(path, capacity);
    if (created == nullptr) return false;
    release(opened.exchange(created, std::memory_order_acq_rel));
    return true;
}

void Close() {
    release(opened.exchange(nullptr, std::memory_order_acq_rel));
}

std::vector<Entry> ReadFile(const std::string& path, Header* header) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    Header read{};
    if (!input.read(reinterpret_cast<char*>(&read), sizeof(read)) || std::memcmp(read.magic, Magic, sizeof(Magic)) != 0 || read.entryBytes != sizeof(Entry) || read.capacity == 0) return {};
    if (header != nullptr) *header = read;
    const auto count = std::min(read.next, read.capacity);
    std::vector<Entry> slots(static_cast<std::size_t>(read.capacity));
    if (!input.read(reinterpret_cast<char*>(slots.data()), static_cast<std::streamsize>(slots.size() * sizeof(Entry)))) return {};
    std::vector<Entry> entries;
    entries.reserve(static_cast<std::size_t>(count));
    const auto first = read.next > read.capacity ? read.next % read.capacity : 0;
    for (std::uint64_t k = 0; k < count; ++k) {
        const auto& entry = slots[static_cast<std::size_t>((first + k) % read.capacity)];
        if (entry.kind != 0) entries.push_back(entry);
    }
    return entries;
}

}
