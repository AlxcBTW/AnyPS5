#include "prx/libSceAgcDriver/Execution/include/CompileProgress.hpp"
#include <atomic>

namespace AgcDriver {
namespace {

std::atomic<std::uint64_t> completed{0};
std::atomic<std::uint32_t> active{0};
std::atomic<std::uint64_t> lastBeginMs{0};
std::atomic<std::uint64_t> lastEndMs{0};
std::atomic<std::uint64_t> lastDurationMs{0};
std::atomic<std::uint64_t> expected{0};

}

extern "C" {

void CompileBegin_nid_postfix() {
    lastBeginMs.store(CompileClockMs(), std::memory_order_relaxed);
    active.fetch_add(1, std::memory_order_relaxed);
}

void CompileEnd_nid_postfix() {
    const auto now = CompileClockMs();
    const auto began = lastBeginMs.load(std::memory_order_relaxed);
    lastDurationMs.store(now > began ? now - began : 0, std::memory_order_relaxed);
    lastEndMs.store(now, std::memory_order_relaxed);
    completed.fetch_add(1, std::memory_order_relaxed);
    active.fetch_sub(1, std::memory_order_relaxed);
}

void SetCompileExpected_nid_postfix(std::uint64_t total) {
    expected.store(total, std::memory_order_relaxed);
}

CompileProgress CompileSnapshot_nid_postfix() {
    return CompileProgress{completed.load(std::memory_order_relaxed), active.load(std::memory_order_relaxed), lastBeginMs.load(std::memory_order_relaxed), lastEndMs.load(std::memory_order_relaxed), lastDurationMs.load(std::memory_order_relaxed), expected.load(std::memory_order_relaxed)};
}

}
}
