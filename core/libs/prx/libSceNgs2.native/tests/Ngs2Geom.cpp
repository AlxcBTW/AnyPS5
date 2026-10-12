#include "Ngs2Test.hpp"

#include <cstdint>
#include <cstring>

static constexpr float SoundSpeed = 343.0f;

static void TestLayouts() {
    Require(sizeof(Ngs2GeomVector) == 12);
    Require(sizeof(Ngs2GeomCone) == 16);
    Require(sizeof(Ngs2GeomRolloff) == 16);
    Require(sizeof(Ngs2GeomListenerParam) == 60);
    Require(sizeof(Ngs2GeomListenerWork) == 96);
    Require(sizeof(Ngs2GeomSourceParam) == 108);
    Require(sizeof(Ngs2GeomA3dAttribute) == 32);
    Require(sizeof(Ngs2GeomAttribute) == 308);
}

static void TestResetListenerParam() {
    Ngs2GeomListenerParam param;
    std::memset(&param, 0x5a, sizeof(param));
    Require(sceNgs2GeomResetListenerParam(&param) == SCE_NGS2_OK);
    Require(param.position.x == 0.0f && param.position.y == 0.0f && param.position.z == 0.0f);
    Require(param.velocity.x == 0.0f && param.velocity.y == 0.0f && param.velocity.z == 0.0f);
    Require(param.orient_front.x == 0.0f && param.orient_front.y == 0.0f && param.orient_front.z == 1.0f);
    Require(param.orient_up.x == 0.0f && param.orient_up.y == 1.0f && param.orient_up.z == 0.0f);
    Require(param.sound_speed == SoundSpeed);
    Require(param.reserved[0] == 0u && param.reserved[1] == 0u);
    Require(sceNgs2GeomResetListenerParam(nullptr) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);
}

static void TestResetSourceParam() {
    Ngs2GeomSourceParam param;
    std::memset(&param, 0x5a, sizeof(param));
    Require(sceNgs2GeomResetSourceParam(&param) == SCE_NGS2_OK);
    Require(param.position.x == 0.0f && param.position.y == 0.0f && param.position.z == 0.0f);
    Require(param.velocity.x == 0.0f && param.velocity.y == 0.0f && param.velocity.z == 0.0f);
    Require(param.direction.x == 0.0f && param.direction.y == 0.0f && param.direction.z == 1.0f);
    Require(param.cone.inner_level == 1.0f && param.cone.inner_angle == 360.0f);
    Require(param.cone.outer_level == 1.0f && param.cone.outer_angle == 360.0f);
    Require(param.rolloff.model == 0u);
    Require(param.rolloff.max_distance == 1000000.0f);
    Require(param.rolloff.rolloff_factor == 1.0f);
    Require(param.rolloff.reference_distance == 1.0f);
    Require(param.doppler_factor == 1.0f && param.fbw_level == 1.0f && param.lfe_level == 1.0f);
    Require(param.max_level == 1.0f && param.min_level == 0.0f && param.radius == 0.0f);
    Require(param.num_speakers == 2u && param.matrix_format == 2u);
    Require(param.reserved[0] == 0u && param.reserved[1] == 0u);
    Require(sceNgs2GeomResetSourceParam(nullptr) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);
}

int main() {
    TestLayouts();
    TestResetListenerParam();
    TestResetSourceParam();
    return 0;
}
