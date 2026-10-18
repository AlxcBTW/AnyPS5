#include "SceTypes.hpp"
#include "prx/libkernel/Pthread/include/LabThreadPriority.hpp"
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
int APS5_VABI scePthreadSetprio(Pthread thread, int prio);
int APS5_VABI scePthreadGetprio(Pthread thread, int* prio);
int APS5_VABI scePthreadAttrInit(PthreadAttr* attr);
int APS5_VABI scePthreadAttrDestroy(PthreadAttr* attr);
int APS5_VABI scePthreadAttrSetinheritsched(PthreadAttr* attr, int inheritSched);
int APS5_VABI scePthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param);
}

namespace {

using namespace LabThreadPriority;

constexpr int SCE_OK = 0;
constexpr int EXPLICIT_SCHED = 0;

void Require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << "\n";
    std::exit(1);
}

static_assert(HostPriority(256) == HostNormal && HostPriority(257) == HostNormal && HostPriority(511) == HostNormal);
static_assert(HostPriority(512) == HostBelowNormal && HostPriority(700) == HostBelowNormal && HostPriority(701) == HostBelowNormal && HostPriority(703) == HostBelowNormal);
static_assert(HostPriority(704) == HostLowest && HostPriority(766) == HostLowest && HostPriority(767) == HostLowest);
static_assert(HostPriority(896) == HostLowest && HostPriority(959) == HostLowest);
static_assert(HostLowest < HostBelowNormal && HostBelowNormal < HostNormal);

void CheckMapping() {
    for (int guest = 256; guest <= 959; ++guest) {
        const int host = HostPriority(guest);
        Require(host <= HostNormal, "guest priority " + std::to_string(guest) + " maps above normal");
        Require(host >= HostLowest, "guest priority " + std::to_string(guest) + " maps below lowest");
        if (guest > 256) Require(host <= HostPriority(guest - 1), "guest priority " + std::to_string(guest) + " ranks above " + std::to_string(guest - 1));
    }
    Require(std::strcmp(HostPriorityName(HostNormal), "normal") == 0 && std::strcmp(HostPriorityName(HostBelowNormal), "below normal") == 0 && std::strcmp(HostPriorityName(HostLowest), "lowest") == 0, "the level names are wrong");
}

#ifdef _WIN32
struct Probe {
    std::promise<int> created;
    std::promise<void> changed;
    std::promise<int> afterChange;
};

void* APS5_VABI Report(void* argument) {
    auto* probe = static_cast<Probe*>(argument);
    probe->created.set_value(GetThreadPriority(GetCurrentThread()));
    probe->changed.get_future().get();
    probe->afterChange.set_value(GetThreadPriority(GetCurrentThread()));
    return nullptr;
}

int Created(int explicitPriority, Pthread& thread, Probe& probe) {
    PthreadAttr attr = nullptr;
    Require(scePthreadAttrInit(&attr) == SCE_OK, "scePthreadAttrInit failed");
    if (explicitPriority != 0) {
        Require(scePthreadAttrSetinheritsched(&attr, EXPLICIT_SCHED) == SCE_OK, "scePthreadAttrSetinheritsched failed");
        const KernelSchedParam param{explicitPriority};
        Require(scePthreadAttrSetschedparam(&attr, &param) == SCE_OK, "scePthreadAttrSetschedparam failed");
    }
    Require(scePthreadCreate(&thread, &attr, Report, &probe, "priority probe") == SCE_OK, "scePthreadCreate failed");
    Require(scePthreadAttrDestroy(&attr) == SCE_OK, "scePthreadAttrDestroy failed");
    return probe.created.get_future().get();
}

void CheckThreads(bool enabled) {
    const int callerBefore = GetThreadPriority(GetCurrentThread());
    {
        Probe probe;
        Pthread thread = nullptr;
        const int created = Created(766, thread, probe);
        Require(created == (enabled ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_NORMAL), "a thread created at guest 766 runs at " + std::to_string(created));
        Require(scePthreadSetprio(thread, 257) == SCE_OK, "scePthreadSetprio failed");
        int stored = 0;
        Require(scePthreadGetprio(thread, &stored) == SCE_OK && stored == 257, "scePthreadSetprio did not store the guest priority");
        probe.changed.set_value();
        const int after = probe.afterChange.get_future().get();
        Require(after == THREAD_PRIORITY_NORMAL, "a thread set to guest 257 runs at " + std::to_string(after));
        Require(scePthreadJoin(thread, nullptr) == SCE_OK, "scePthreadJoin failed");
    }
    {
        Probe probe;
        Pthread thread = nullptr;
        const int created = Created(0, thread, probe);
        Require(created == (enabled ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL), "a thread that inherits the default guest 700 runs at " + std::to_string(created));
        Require(scePthreadSetprio(thread, 896) == SCE_OK, "scePthreadSetprio(896) failed");
        probe.changed.set_value();
        const int after = probe.afterChange.get_future().get();
        Require(after == (enabled ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_NORMAL), "a thread set to guest 896 runs at " + std::to_string(after));
        Require(scePthreadJoin(thread, nullptr) == SCE_OK, "scePthreadJoin failed");
    }
    Require(GetThreadPriority(GetCurrentThread()) == callerBefore, "creating guest threads changed the creating thread's own priority");
}
#endif

}

int main() {
    CheckMapping();
#ifdef _WIN32
    const char* value = std::getenv("APS5_LAB_THREAD_PRIORITIES");
    const bool enabled = value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    CheckThreads(enabled);
    std::cout << "guest thread priorities: mapping and threads checked, switch " << (enabled ? "on" : "off") << "\n";
#else
    std::cout << "guest thread priorities: mapping checked (the switch applies on Windows only)\n";
#endif
    return 0;
}
