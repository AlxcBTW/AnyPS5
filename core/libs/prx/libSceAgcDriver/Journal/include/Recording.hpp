#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_JOURNAL_INCLUDE_RECORDING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_JOURNAL_INCLUDE_RECORDING_HPP

#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "SceShaders.hpp"
#include "SceTypes.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver::Journal {

extern std::atomic<bool> armed;

inline bool Armed() {
    return armed.load(std::memory_order_relaxed);
}

void Submit(const Packet* packet, std::uint32_t queue);
void SuspendPoint();
void RegisterShader(const Shader* shader);
void ResolveShaderAbi(const Shader* shader, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive);
void ResolveGraphicsStagesAbi(std::span<const Shader* const> stages, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive);
void ResolveGraphicsAbi(const Shader* vertex, const Shader* pixel, std::uint32_t primitiveType);
void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);
void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);

}

#endif
