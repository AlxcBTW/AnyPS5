#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

namespace AgcDriver::DriverDetail {

void Driver::verifyDataHit(const ShaderSnapshot& snapshot, std::size_t codeOffset, ShaderRecompiler::RecompileRequest request, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint64_t address, const DispatchVariant& variant, std::span<const std::uint32_t> liveWords, const ShaderRecompiler::RecompileResult& patched) {
    const auto fail = [&](const char* what, std::size_t position, std::size_t slot) {
        std::fprintf(stderr, "[dispatch-cache] APS5_VERIFY_DATA_HITS: %s disagrees (program 0x%llx, position %zu, slot %zu)\n", what, static_cast<unsigned long long>(address), position, slot);
        std::fflush(stderr);
        std::abort();
    };
    auto verifyMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
    const auto invocation = InvocationFor(snapshot, codeOffset, request);
    const auto capture = verifyMemory->Capture(invocation);
    const auto regions = verifyMemory->Regions();
    request.context.memory = regions;
    const auto result = invocation.Materialize(*capture);
    std::size_t position = 0, run = 0;
    for (const auto& region : regions) {
        const bool registered = std::any_of(memory.begin(), memory.end(), [&](const auto& known) { return region.guestAddress >= known.guestAddress && region.guestAddress < known.guestAddress + known.bytes.size(); });
        if (registered) continue;
        if (run >= variant.runs.size() || variant.runs[run].first != region.guestAddress || variant.runs[run].second != region.guestAddress + region.bytes.size()) fail("the read set", position, run);
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        if (position + count > liveWords.size() || std::memcmp(region.bytes.data(), liveWords.data() + position, region.bytes.size()) != 0) fail("the captured bytes", position, run);
        position += count;
        ++run;
    }
    if (run != variant.runs.size()) fail("the read set", position, run);
    if (result->variantId != patched.variantId) fail("the variant", 0, 0);
    if (result->bindings.size() != patched.bindings.size() || result->pushConstants != patched.pushConstants) fail("the bindings", 0, 0);
    for (std::size_t b = 0; b < result->bindings.size(); ++b) {
        const auto& fresh = result->bindings[b];
        const auto& reused = patched.bindings[b];
        if (fresh.kind != reused.kind || fresh.role != reused.role || fresh.binding != reused.binding || fresh.count != reused.count || fresh.guestDescriptor != reused.guestDescriptor) fail("the bindings", b, 0);
    }
    std::lock_guard cacheLock(dispatchCacheMutex);
    ++entryCounters.dataVerified;
}

void Driver::verifyRelocatedHit(const ShaderSnapshot& snapshot, std::size_t codeOffset, ShaderRecompiler::RecompileRequest request, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint64_t address, std::span<const ShaderRecompiler::MemoryRegion> captured, const ShaderRecompiler::RecompileResult& patched) {
    const auto fail = [&](const std::string& what) {
        std::fprintf(stderr, "[dispatch-content] APS5_VERIFY_CONTENT_HITS: a relocated hit of program 0x%llx disagrees with a fresh capture: %s\n", static_cast<unsigned long long>(address), what.c_str());
        std::fflush(stderr);
        std::abort();
    };
    auto verifyMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
    const auto invocation = InvocationFor(snapshot, codeOffset, request);
    const auto capture = verifyMemory->Capture(invocation);
    const auto regions = verifyMemory->Regions();
    request.context.memory = regions;
    const auto result = invocation.Materialize(*capture);
    // The words the hit stands for (its rebased runs with the live words) are the words the walk reads now.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> served, fresh;
    const auto collect = [](std::span<const ShaderRecompiler::MemoryRegion> from, std::vector<std::pair<std::uint64_t, std::uint32_t>>& into) {
        for (const auto& region : from) {
            for (std::size_t at = 0; at + sizeof(std::uint32_t) <= region.bytes.size(); at += sizeof(std::uint32_t)) {
                std::uint32_t word = 0;
                std::memcpy(&word, region.bytes.data() + at, sizeof(word));
                into.emplace_back(region.guestAddress + at, word);
            }
        }
        std::sort(into.begin(), into.end());
        into.erase(std::unique(into.begin(), into.end()), into.end());
    };
    collect(captured, served);
    collect(regions, fresh);
    if (served != fresh) {
        std::size_t first = 0;
        while (first < served.size() && first < fresh.size() && served[first] == fresh[first]) ++first;
        char text[160];
        std::snprintf(text, sizeof(text), "the read set (%zu words served, %zu read; first difference at 0x%llx)", served.size(), fresh.size(), static_cast<unsigned long long>(first < fresh.size() ? fresh[first].first : first < served.size() ? served[first].first : 0));
        fail(text);
    }
    std::string why;
    if (!SameMaterialization(*result, patched, why)) fail(why);
    std::lock_guard cacheLock(dispatchCacheMutex);
    ++entryCounters.dataVerified;
}

}
