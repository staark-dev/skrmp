#include "intro/launch_signals.h"
#include "ipc/handoff_trace.h"

#include <atomic>
#include <cstdio>

namespace skr::launch_signals {
namespace {

HANDLE           g_events[ipc::kMilestoneCount] = {};
std::atomic<bool> g_signalled[ipc::kMilestoneCount];   // static storage: zero-initialised
uint64_t         g_session = 0;

HANDLE AsHandle(uint32_t value) {
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
}

} // namespace

void Configure(const ipc::LaunchParams& p) {
    g_session = p.sessionId;
    g_events[static_cast<int>(ipc::Milestone::ClientInitialized)] = AsHandle(p.clientInitializedHandle);
    g_events[static_cast<int>(ipc::Milestone::SampLoaded)]        = AsHandle(p.sampLoadedHandle);
    g_events[static_cast<int>(ipc::Milestone::SampReady)]         = AsHandle(p.sampReadyHandle);
    g_events[static_cast<int>(ipc::Milestone::GameplayConfirmed)] = AsHandle(p.gameplayConfirmedHandle);
    g_events[static_cast<int>(ipc::Milestone::VisualReady)]       = AsHandle(p.sessionReadyHandle);
    char line[96];
    snprintf(line, sizeof(line), "MILESTONE configured session=%016llx", static_cast<unsigned long long>(g_session));
    ipc::TraceHandoff(line);
}

bool Signal(ipc::Milestone m, const char* detail) {
    const int i = static_cast<int>(m);
    if (i < 0 || i >= ipc::kMilestoneCount) return false;
    if (g_signalled[i].exchange(true, std::memory_order_acq_rel)) return false;
    const HANDLE event = g_events[i];
    const bool sent = event && SetEvent(event);
    char line[200];
    snprintf(line, sizeof(line), "MILESTONE %s session=%016llx pid=%lu t=%lu launcher=%s%s%s",
             ipc::MilestoneName(m), static_cast<unsigned long long>(g_session), GetCurrentProcessId(),
             GetTickCount(), sent ? "notified" : event ? "SetEvent FAILED" : "no event",
             detail ? " " : "", detail ? detail : "");
    ipc::TraceHandoff(line);
    return true;
}

bool Signalled(ipc::Milestone m) {
    const int i = static_cast<int>(m);
    return i >= 0 && i < ipc::kMilestoneCount && g_signalled[i].load(std::memory_order_acquire);
}

uint64_t SessionId() { return g_session; }

} // namespace skr::launch_signals
