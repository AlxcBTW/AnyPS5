#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_DRIVER_DRAW_DRAWREGISTERKEY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_DRIVER_DRAW_DRAWREGISTERKEY_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <unordered_map>

namespace AgcDriver::DriverDetail {

std::uint64_t RegisterStateKey(const QueueState& queue, bool allUserWords = false, bool incremental = true);

enum class RegisterKeyPart : std::size_t { Shader, Context, ShaderRegisters, UserData, UserConfig, Count };

using RegisterKeyParts = std::array<std::uint64_t, static_cast<std::size_t>(RegisterKeyPart::Count)>;

struct RegisterKeyMissCounts {
    std::uint64_t misses = 0;
    std::uint64_t noCandidate = 0;
    std::uint64_t identical = 0;
    RegisterKeyParts differing{};
};

RegisterKeyParts RegisterStateParts(const QueueState& queue, bool allUserWords = false);

class RegisterKeyMissTracker {
public:
    void Note(std::uint64_t identity, const RegisterKeyParts& parts, bool miss);
    RegisterKeyMissCounts Take();

private:
    std::unordered_map<std::uint64_t, RegisterKeyParts> last;
    RegisterKeyMissCounts counts;
};

}

#endif
