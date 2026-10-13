#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPILEPROGRESS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPILEPROGRESS_HPP

#include <chrono>
#include <cstdint>

namespace AgcDriver {

struct CompileProgress {
    std::uint64_t completed;
    std::uint32_t active;
    std::uint64_t lastBeginMs;
    std::uint64_t lastEndMs;
    std::uint64_t lastDurationMs;
};

inline std::uint64_t CompileClockMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

extern "C" {

void CompileBegin_nid_postfix();
void CompileEnd_nid_postfix();
CompileProgress CompileSnapshot_nid_postfix();

}

class CompileScope {
public:
    CompileScope() {
        CompileBegin_nid_postfix();
    }
    ~CompileScope() {
        CompileEnd_nid_postfix();
    }
    CompileScope(const CompileScope&) = delete;
    CompileScope& operator=(const CompileScope&) = delete;
};

}

#endif
