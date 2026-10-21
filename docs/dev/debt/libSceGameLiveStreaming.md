# libSceGameLiveStreaming technical debt

### Functional

- [sceGameLiveStreamingEnableLiveStreaming](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - imported by PS4 titles, which do not load without it, and added without an implementation: declared without parameters, every call throws
- [libSceGameLiveStreaming](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) - native initialization requires exactly 0x4000 heap bytes before checking repeated initialization. Host lifecycle and pointer errors now match the wrapper, but service registration, allocation failures and asynchronous service readiness are not modelled. The host becomes ready immediately after initialization.
- [sceGameLiveStreamingGetCurrentStatus2](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) - the supplied module confirms the 72-byte layout and inactive status (user -1, remaining bytes zero). Live service counters, active-user changes and broadcast-resolution conversion are unimplemented.
- [sceGameLiveStreamingGetProgramInfo](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) - consumes one output pointer and uses a 4940-byte record. Field semantics remain unresolved; the protected body remains unimplemented.
- [sceGameLiveStreamingPermitLiveStreaming](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) - this and fifteen related setting, feedback and message calls (PermitServerSideRecording, SetMaxBitrate, EnableSocialFeedback, SetMetadata, Set/ClearSpoilerTag, Set/ClearPresetSocialFeedbackCommands, ...Description, SetCameraFrameSetting, SetInvitationSessionId, Get/ClearSocialFeedbackMessages, GetSocialFeedbackMessagesCount, PostSocialMessage) answer locally and store nothing. Not initialized is checked first, then null pointers, following GetCurrentStatus2. Not measured on a PS5: signatures, argument ranges, struct field semantics, a zero count with a null array. Queries report zero messages.

### Unknown function info

- [sceGameLiveStreamingScreenInitialize](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingScreenInitializeSeparateModeParameter](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingScreenTerminate](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingScreenCloseSeparateMode](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingScreenConfigureSeparateMode](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingScreenOpenSeparateMode](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingScreenSetMode](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingGetCurrentStatus](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingSetServiceProviderPermission](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingSetDefaultServiceProviderPermission](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingStartGenerateStandbyScreenResource](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingStopGenerateStandbyScreenResource](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingRegisterCallback](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingUnregisterCallback](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingCheckCallback](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingSetStandbyScreenResource](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingGetCurrentBroadcastScreenLayout](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingLaunchLiveViewer](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingLaunchLiveViewerA](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingSetGuardAreas](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingStartSocialFeedbackMessageFiltering](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingStopSocialFeedbackMessageFiltering](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingApplySocialFeedbackMessageFilter](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingSetLinkCommentPreset](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingStartDebugBroadcast](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
- [sceGameLiveStreamingStopDebugBroadcast](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - unknown signature
