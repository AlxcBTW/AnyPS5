#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <span>

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

struct GameLiveStreamingSpoilerTag {
    char text[65];
    std::uint8_t reserved[31];
};
static_assert(sizeof(GameLiveStreamingSpoilerTag) == 96);

struct GameLiveStreamingPreset {
    std::uint32_t commandId;
    char text[65];
    std::uint8_t reserved[31];
};

struct GameLiveStreamingCameraFrameSetting {
    std::uint8_t bytes[64];
};

struct GameLiveStreamingFeedback {
    std::uint8_t bytes[256];
};

extern "C" {
int APS5_VABI sceGameLiveStreamingInitialize(std::size_t heap_size);
int APS5_VABI sceGameLiveStreamingTerminate(void);
int APS5_VABI sceGameLiveStreamingGetCurrentStatus2(GameLiveStreamingStatus2* status);
struct GameLiveStreamingProgramInfo;
int APS5_VABI sceGameLiveStreamingGetProgramInfo(GameLiveStreamingProgramInfo*);
int APS5_VABI sceGameLiveStreamingPermitLiveStreaming(bool);
int APS5_VABI sceGameLiveStreamingPermitServerSideRecording(bool);
int APS5_VABI sceGameLiveStreamingSetMaxBitrate(std::uint32_t);
int APS5_VABI sceGameLiveStreamingEnableSocialFeedback(bool);
int APS5_VABI sceGameLiveStreamingSetMetadata(const char*);
int APS5_VABI sceGameLiveStreamingSetSpoilerTag(std::size_t, const GameLiveStreamingSpoilerTag*);
int APS5_VABI sceGameLiveStreamingClearSpoilerTag(void);
int APS5_VABI sceGameLiveStreamingSetPresetSocialFeedbackCommands(std::size_t, const GameLiveStreamingPreset*);
int APS5_VABI sceGameLiveStreamingClearPresetSocialFeedbackCommands(void);
int APS5_VABI sceGameLiveStreamingSetPresetSocialFeedbackCommandsDescription(const char*);
int APS5_VABI sceGameLiveStreamingSetCameraFrameSetting(const GameLiveStreamingCameraFrameSetting*);
int APS5_VABI sceGameLiveStreamingSetInvitationSessionId(const char*);
int APS5_VABI sceGameLiveStreamingGetSocialFeedbackMessagesCount(int, std::uint32_t*);
int APS5_VABI sceGameLiveStreamingGetSocialFeedbackMessages(int, std::size_t, GameLiveStreamingFeedback*, std::size_t*);
int APS5_VABI sceGameLiveStreamingClearSocialFeedbackMessages(int);
int APS5_VABI sceGameLiveStreamingPostSocialMessage(const char*);
}

namespace {

void Require(bool value) { if (!value) std::abort(); }

constexpr std::int32_t kInvalidUserId = -1;

void CheckAll(int expected, bool nullArguments) {
    GameLiveStreamingSpoilerTag tag{};
    GameLiveStreamingPreset preset{};
    GameLiveStreamingCameraFrameSetting camera{};
    GameLiveStreamingFeedback feedback{};
    std::uint32_t count = 0x55;
    std::size_t acquired = 0x55;
    if (nullArguments) {
        Require(sceGameLiveStreamingSetMetadata(nullptr) == expected);
        Require(sceGameLiveStreamingSetSpoilerTag(1, nullptr) == expected);
        Require(sceGameLiveStreamingSetPresetSocialFeedbackCommands(1, nullptr) == expected);
        Require(sceGameLiveStreamingSetPresetSocialFeedbackCommandsDescription(nullptr) == expected);
        Require(sceGameLiveStreamingSetCameraFrameSetting(nullptr) == expected);
        Require(sceGameLiveStreamingSetInvitationSessionId(nullptr) == expected);
        Require(sceGameLiveStreamingGetSocialFeedbackMessagesCount(0, nullptr) == expected);
        Require(sceGameLiveStreamingGetSocialFeedbackMessages(0, 1, nullptr, &acquired) == expected);
        Require(sceGameLiveStreamingGetSocialFeedbackMessages(0, 1, &feedback, nullptr) == expected);
        Require(sceGameLiveStreamingPostSocialMessage(nullptr) == expected);
        Require(acquired == 0x55);
        return;
    }
    Require(sceGameLiveStreamingPermitLiveStreaming(true) == expected);
    Require(sceGameLiveStreamingPermitLiveStreaming(false) == expected);
    Require(sceGameLiveStreamingPermitServerSideRecording(true) == expected);
    Require(sceGameLiveStreamingPermitServerSideRecording(false) == expected);
    Require(sceGameLiveStreamingSetMaxBitrate(0) == expected);
    Require(sceGameLiveStreamingSetMaxBitrate(6000) == expected);
    Require(sceGameLiveStreamingSetMaxBitrate(0xffffffffu) == expected);
    Require(sceGameLiveStreamingEnableSocialFeedback(true) == expected);
    Require(sceGameLiveStreamingEnableSocialFeedback(false) == expected);
    Require(sceGameLiveStreamingSetMetadata("") == expected);
    Require(sceGameLiveStreamingSetMetadata("metadata") == expected);
    Require(sceGameLiveStreamingSetSpoilerTag(1, &tag) == expected);
    Require(sceGameLiveStreamingSetSpoilerTag(0x1000, &tag) == expected);
    Require(sceGameLiveStreamingClearSpoilerTag() == expected);
    Require(sceGameLiveStreamingClearSpoilerTag() == expected);
    Require(sceGameLiveStreamingSetPresetSocialFeedbackCommands(1, &preset) == expected);
    Require(sceGameLiveStreamingSetPresetSocialFeedbackCommands(0x1000, &preset) == expected);
    Require(sceGameLiveStreamingClearPresetSocialFeedbackCommands() == expected);
    Require(sceGameLiveStreamingClearPresetSocialFeedbackCommands() == expected);
    Require(sceGameLiveStreamingSetPresetSocialFeedbackCommandsDescription("description") == expected);
    Require(sceGameLiveStreamingSetCameraFrameSetting(&camera) == expected);
    Require(sceGameLiveStreamingSetInvitationSessionId("session") == expected);
    Require(sceGameLiveStreamingGetSocialFeedbackMessagesCount(0, &count) == expected);
    Require(count == (expected == 0 ? 0u : 0x55u));
    Require(sceGameLiveStreamingGetSocialFeedbackMessages(0, 1, &feedback, &acquired) == expected);
    Require(acquired == (expected == 0 ? 0u : 0x55u));
    Require(sceGameLiveStreamingClearSocialFeedbackMessages(0) == expected);
    Require(sceGameLiveStreamingClearSocialFeedbackMessages(1) == expected);
    Require(sceGameLiveStreamingPostSocialMessage("message") == expected);
}

void CheckMessages() {
    for (const int type : {0, 1, 2, -1}) {
        std::uint32_t count = 0xdeadbeef;
        Require(sceGameLiveStreamingGetSocialFeedbackMessagesCount(type, &count) == 0);
        Require(count == 0);
        GameLiveStreamingFeedback feedback;
        std::memset(&feedback, 0xaa, sizeof(feedback));
        std::size_t acquired = 0xdeadbeef;
        Require(sceGameLiveStreamingGetSocialFeedbackMessages(type, 1, &feedback, &acquired) == 0);
        Require(acquired == 0);
        for (const auto value : std::as_bytes(std::span(&feedback, 1))) Require(value == std::byte{0xaa});
        acquired = 0xdeadbeef;
        Require(sceGameLiveStreamingGetSocialFeedbackMessages(type, 0, nullptr, &acquired) == 0);
        Require(acquired == 0);
        Require(sceGameLiveStreamingClearSocialFeedbackMessages(type) == 0);
    }
    Require(sceGameLiveStreamingSetSpoilerTag(0, nullptr) == 0);
    Require(sceGameLiveStreamingSetPresetSocialFeedbackCommands(0, nullptr) == 0);
}

}

int main() {
    constexpr int invalidArgument = static_cast<int>(0x80A00002);
    constexpr int alreadyInitialized = static_cast<int>(0x80A00003);
    constexpr int notInitialized = static_cast<int>(0x80A00004);
    Require(sceGameLiveStreamingTerminate() == notInitialized);
    Require(sceGameLiveStreamingGetCurrentStatus2(nullptr) == notInitialized);
    CheckAll(notInitialized, true);
    CheckAll(notInitialized, false);
    for (const std::size_t size : {0u, 1u, 0x3fffu, 0x4001u, 0x100000u}) Require(sceGameLiveStreamingInitialize(size) == invalidArgument);
    Require(sceGameLiveStreamingInitialize(0x4000) == 0);
    Require(sceGameLiveStreamingInitialize(0) == invalidArgument);
    Require(sceGameLiveStreamingInitialize(0x4000) == alreadyInitialized);
    Require(sceGameLiveStreamingGetCurrentStatus2(nullptr) == invalidArgument);

    GameLiveStreamingStatus2 status{};
    std::memset(&status, 0x7f, sizeof(status));
    Require(sceGameLiveStreamingGetCurrentStatus2(&status) == 0);
    Require(status.userId == kInvalidUserId);
    Require(!status.isOnAir);
    Require(status.spectatorCounts == 0);
    Require(status.textMessageCounts == 0);
    Require(status.commandMessageCounts == 0);
    Require(status.broadcastVideoResolution == 0);
    for (std::uint8_t byte : status.align) {
        Require(byte == 0);
    }
    for (std::uint8_t byte : status.reserved) {
        Require(byte == 0);
    }

    CheckAll(invalidArgument, true);
    CheckAll(0, false);
    CheckMessages();

    bool threwProgramInfo = false;
    try {
        sceGameLiveStreamingGetProgramInfo(nullptr);
    } catch (const std::runtime_error&) {
        threwProgramInfo = true;
    }
    Require(threwProgramInfo);

    Require(sceGameLiveStreamingTerminate() == 0);
    std::memset(&status, 0x7f, sizeof(status));
    Require(sceGameLiveStreamingGetCurrentStatus2(&status) == notInitialized);
    for (const auto value : std::as_bytes(std::span(&status, 1))) Require(value == std::byte{0x7f});
    Require(sceGameLiveStreamingTerminate() == notInitialized);
    CheckAll(notInitialized, true);
    CheckAll(notInitialized, false);
}
