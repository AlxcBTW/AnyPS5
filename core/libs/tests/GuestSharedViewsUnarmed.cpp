#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {
int APS5_VABI sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
int APS5_VABI sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
int APS5_VABI sceKernelMunmap(void*, std::size_t);
int APS5_VABI sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
}

static void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Unarmed shared view check failed at %s:%u\n", location.file_name(), static_cast<unsigned>(location.line()));
        std::abort();
    }
}

// APS5_EXP_NO_VIEW_ARMING=1 (set by the test's environment): shared views of direct memory are never armed,
// so a guest write through either view takes no write fault (the views stay writable after a clearing
// collect), every collect reports every page of a view as written, and the views still share their bytes.
static void CheckUnarmedSharedViews() {
#ifdef _WIN32
    constexpr std::size_t page = 0x4000;
    std::int64_t phys = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 3, 0, 0, &phys) == 0);
    void* first = nullptr;
    void* second = nullptr;
    Require(sceKernelMapDirectMemory(&first, page * 3, 3, 0, phys, 0) == 0);
    Require(sceKernelMapDirectMemory(&second, page * 3, 3, 0, phys, 0) == 0);
    const auto collect = [](void* address, std::size_t bytes, bool clear = true) {
        std::array<void*, 32> pages{};
        std::size_t count = pages.size();
        Require(GuestArena::GuestArenaCollectWrites_nid_postfix(reinterpret_cast<std::uintptr_t>(address), bytes, pages.data(), &count, clear));
        return count;
    };
    const auto writable = [](void* address) {
        MEMORY_BASIC_INFORMATION info{};
        return VirtualQuery(address, &info, sizeof(info)) != 0 && info.Protect == PAGE_READWRITE;
    };
    for (int round = 0; round < 3; ++round) {
        Require(collect(first, page * 3) == 12);
        Require(collect(second, page * 3) == 12);
    }
    Require(writable(first) && writable(static_cast<std::byte*>(first) + page * 2) && writable(second) && writable(static_cast<std::byte*>(second) + page));
    auto* left = static_cast<volatile unsigned char*>(first);
    auto* right = static_cast<volatile unsigned char*>(second);
    left[page + 5] = 21;
    Require(right[page + 5] == 21);
    right[page * 2] = 42;
    Require(left[page * 2] == 42);
    Require(collect(first, page * 3, false) == 12 && collect(first, page * 3) == 12 && collect(second, page * 3) == 12);
    Require(writable(first) && writable(second));
    Require(sceKernelMunmap(first, page * 3) == 0);
    Require(sceKernelMunmap(second, page * 3) == 0);
    Require(sceKernelReleaseDirectMemory(phys, page * 3) == 0);
#endif
}

int main() {
    CheckUnarmedSharedViews();
    std::printf("unarmed shared view checks passed\n");
    return 0;
}
