#ifndef CORE_LIBS_PRX_LIBKERNEL_PTHREAD_INCLUDE_LABTHREADPRIORITY_HPP
#define CORE_LIBS_PRX_LIBKERNEL_PTHREAD_INCLUDE_LABTHREADPRIORITY_HPP

namespace LabThreadPriority {

// Windows thread priority levels (THREAD_PRIORITY_LOWEST, _BELOW_NORMAL, _NORMAL), repeated here so the mapping can be
// tested on every host.
inline constexpr int HostLowest = -2;
inline constexpr int HostBelowNormal = -1;
inline constexpr int HostNormal = 0;

// Lab switch APS5_LAB_THREAD_PRIORITIES=1: the guest thread priorities (256 highest .. 767 lowest, 700 the default;
// 896 .. 959 the idle class) set at thread creation and by the scheduling calls become Windows priorities inside the
// normal band, never above normal. The layer's own threads (the AGC driver's queue workers, draw committer and
// presenter, every host thread the libraries start) stay at normal, so no guest thread is ever ranked above them:
// the console's high classes (256 .. 511: audio, render, RHI and IO threads ask for 256 and 257) share normal with
// them, the default class (512 .. 703: the default 700 and the foreground workers' 700 and 701) runs below normal, and
// the low classes (704 .. 767: background workers lower themselves to 766) and anything past them run at lowest.
constexpr int HostPriority(int guestPriority) {
    if (guestPriority < 512) return HostNormal;
    if (guestPriority < 704) return HostBelowNormal;
    return HostLowest;
}

constexpr const char* HostPriorityName(int hostPriority) {
    return hostPriority == HostNormal ? "normal" : hostPriority == HostBelowNormal ? "below normal" : hostPriority == HostLowest ? "lowest" : "other";
}

}

#endif
