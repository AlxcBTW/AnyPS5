#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DIAGNOSTICS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DIAGNOSTICS_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <cstdint>
#include <span>

namespace AgcDriver::DriverDetail {

double TraceMs();

std::uint64_t SkippedShader();

// Lab only. APS5_LAB_TRACE_CODE=1 prints one [labcode] line per program address and code hash; APS5_LAB_SKIP_CODE=<hash>[,<hash>...]
// (the hashes of those lines, hex) makes the draws and dispatches of these programs record nothing. Returns true to skip.
bool LabCode(const char* kind, unsigned stage, std::uint64_t address, std::span<const std::uint32_t> code);

void require(bool condition, const char* reason);

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset);

std::uint32_t readUserData(const Registers& shader, std::uint32_t offset);

}

#endif
