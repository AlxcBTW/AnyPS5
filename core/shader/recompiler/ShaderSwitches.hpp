#ifndef CORE_SHADER_RECOMPILIER_SHADERSWITCHES_HPP
#define CORE_SHADER_RECOMPILIER_SHADERSWITCHES_HPP

#include <cstdlib>
#include <string_view>

namespace ShaderRecompiler {

// Environment switches that change the SPIR-V a recompile produces. They are read from the
// environment, not from the request: the shader disk cache keys every APS5_* literal in the
// recompiler sources together with its value (ShaderCacheVersion.cmake, ShaderDiskCache switchKey),
// so a module built with a switch off is never reused with it on. All are off when unset.

inline bool SwitchEnabled(const char* name) {
    const char* text = std::getenv(name);
    return text != nullptr && *text != '\0' && std::string_view(text) != "0";
}

// APS5_FLUSH_ELIM=1: DenormalFlushEliminator recognises more denormal-free f32 values and drops
// the flushes of them.
inline bool FlushEliminationEnabled() {
    static const bool enabled = SwitchEnabled("APS5_FLUSH_ELIM");
    return enabled;
}

}

#endif
