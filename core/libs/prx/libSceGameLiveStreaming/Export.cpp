#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceUserService/UserService.hpp"
#include <cstring>
#include <mutex>

static std::mutex g_mutex;
static bool g_initialized = false;
static constexpr int ErrorInvalidArgument = static_cast<int>(0x80A00002);
static constexpr int ErrorAlreadyInitialized = static_cast<int>(0x80A00003);
static constexpr int ErrorNotInitialized = static_cast<int>(0x80A00004);

struct GameLiveStreamingProgramInfo;
struct GameLiveStreamingPresetCommand;
struct GameLiveStreamingCameraFrameSetting;
struct GameLiveStreamingFeedbackMessage;

struct GameLiveStreamingSpoilerTag {
    char text[65];
    std::uint8_t reserved[31];
};
static_assert(sizeof(GameLiveStreamingSpoilerTag) == 96);

struct GameLiveStreamingStatus2 {
    std::int32_t userId;
    bool isOnAir;
    std::uint8_t align[3];
    std::uint32_t spectatorCounts;
    std::uint32_t textMessageCounts;
    std::uint32_t commandMessageCounts;
    std::uint32_t broadcastVideoResolution;
    std::uint8_t reserved[48];
};
static_assert(sizeof(GameLiveStreamingStatus2) == 72);

extern "C" {

int APS5_VABI sceGameLiveStreamingInitialize(size_t heapSize) {
    if (heapSize != 0x4000) return ErrorInvalidArgument;
    std::lock_guard lock(g_mutex);
    if (g_initialized) return ErrorAlreadyInitialized;
    g_initialized = true;
    return 0;
}

int APS5_VABI sceGameLiveStreamingTerminate(void) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    g_initialized = false;
    return 0;
}

int APS5_VABI sceGameLiveStreamingGetCurrentStatus2(GameLiveStreamingStatus2* status) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if (status == nullptr) return ErrorInvalidArgument;
    std::memset(status, 0, sizeof(*status));
    status->userId = USER_SERVICE_USER_ID_INVALID;
    return 0;
}

int APS5_VABI sceGameLiveStreamingPermitLiveStreaming(bool permit) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    return 0;
}

int APS5_VABI sceGameLiveStreamingPermitServerSideRecording(bool permit) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetMaxBitrate(std::uint32_t bitrate) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    return 0;
}

int APS5_VABI sceGameLiveStreamingEnableSocialFeedback(bool enable) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetMetadata(const char* metadata) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if (metadata == nullptr) return ErrorInvalidArgument;
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetSpoilerTag(size_t count, const GameLiveStreamingSpoilerTag* tags) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if (count != 0 && tags == nullptr) return ErrorInvalidArgument;
    return 0;
}

int APS5_VABI sceGameLiveStreamingClearSpoilerTag(void) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetPresetSocialFeedbackCommands(size_t count, const GameLiveStreamingPresetCommand* commands) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if (count != 0 && commands == nullptr) return ErrorInvalidArgument;
    return 0;
}

int APS5_VABI sceGameLiveStreamingClearPresetSocialFeedbackCommands(void) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetPresetSocialFeedbackCommandsDescription(const char* description) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if (description == nullptr) return ErrorInvalidArgument;
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetCameraFrameSetting(const GameLiveStreamingCameraFrameSetting* setting) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if (setting == nullptr) return ErrorInvalidArgument;
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetInvitationSessionId(const char* sessionId) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if (sessionId == nullptr) return ErrorInvalidArgument;
    return 0;
}

int APS5_VABI sceGameLiveStreamingGetSocialFeedbackMessagesCount(int type, std::uint32_t* count) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if (count == nullptr) return ErrorInvalidArgument;
    *count = 0;
    return 0;
}

int APS5_VABI sceGameLiveStreamingGetSocialFeedbackMessages(int type, size_t capacity, GameLiveStreamingFeedbackMessage* messages, size_t* acquired) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if ((capacity != 0 && messages == nullptr) || acquired == nullptr) return ErrorInvalidArgument;
    *acquired = 0;
    return 0;
}

int APS5_VABI sceGameLiveStreamingClearSocialFeedbackMessages(int type) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    return 0;
}

int APS5_VABI sceGameLiveStreamingPostSocialMessage(const char* message) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if (message == nullptr) return ErrorInvalidArgument;
    return 0;
}

int APS5_VABI sceGameLiveStreamingGetProgramInfo(GameLiveStreamingProgramInfo* info) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingScreenInitialize(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingScreenInitializeSeparateModeParameter(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingScreenTerminate(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingScreenCloseSeparateMode(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingScreenConfigureSeparateMode(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingScreenOpenSeparateMode(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingScreenSetMode(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingGetCurrentStatus(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetServiceProviderPermission(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetDefaultServiceProviderPermission(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingStartGenerateStandbyScreenResource(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingStopGenerateStandbyScreenResource(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingRegisterCallback(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingUnregisterCallback(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingCheckCallback(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetStandbyScreenResource(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingGetCurrentBroadcastScreenLayout(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingLaunchLiveViewer(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingLaunchLiveViewerA(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetGuardAreas(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingStartSocialFeedbackMessageFiltering(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingStopSocialFeedbackMessageFiltering(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingApplySocialFeedbackMessageFilter(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingSetLinkCommentPreset(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingStartDebugBroadcast(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingStopDebugBroadcast(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceGameLiveStreamingEnableLiveStreaming(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
