#pragma once
// Client side of the launcher readiness protocol (shared/ipc/launch_milestones.h).
// Owns the launcher-duplicated event handles and signals each milestone at
// most once per process, tracing "MILESTONE <name> session=… pid=… t=…" to
// <gta>\SKR\handoff.log. Thread-safe.

#include "ipc/launch_milestones.h"
#include "ipc/launch_params.h"

namespace skr::launch_signals {

// Called once from SkrInitialize after the launch params were read.
void Configure(const ipc::LaunchParams& params);

// Signals `m` (once). Returns true only for the call that signalled it.
bool Signal(ipc::Milestone m, const char* detail = nullptr);

bool     Signalled(ipc::Milestone m);
uint64_t SessionId();

} // namespace skr::launch_signals
