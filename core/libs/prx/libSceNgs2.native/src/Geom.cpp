#include <cstdint>

#include "Ngs2Internal.hpp"

static constexpr float DEFAULT_SOUND_SPEED = 343.0f;

int Ngs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param) {
    if (out_listener_param == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    *out_listener_param = {};
    out_listener_param->orient_front.z = 1.0f;
    out_listener_param->orient_up.y = 1.0f;
    out_listener_param->sound_speed = DEFAULT_SOUND_SPEED;
    return SCE_NGS2_OK;
}

int Ngs2GeomResetSourceParam(Ngs2GeomSourceParam* out_source_param) {
    if (out_source_param == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    *out_source_param = {};
    out_source_param->direction.z = 1.0f;
    out_source_param->cone.inner_level = 1.0f;
    out_source_param->cone.inner_angle = 360.0f;
    out_source_param->cone.outer_level = 1.0f;
    out_source_param->cone.outer_angle = 360.0f;
    out_source_param->rolloff.max_distance = 1000000.0f;
    out_source_param->rolloff.rolloff_factor = 1.0f;
    out_source_param->rolloff.reference_distance = 1.0f;
    out_source_param->doppler_factor = 1.0f;
    out_source_param->fbw_level = 1.0f;
    out_source_param->lfe_level = 1.0f;
    out_source_param->max_level = 1.0f;
    out_source_param->num_speakers = 2;
    out_source_param->matrix_format = 2;
    return SCE_NGS2_OK;
}
