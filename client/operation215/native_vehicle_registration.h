#pragma once
// ─────────────────────────────────────────────────────────────────────
// Operation 215 — native custom vehicle registration via SAMP\CUSTOM.IMG
// ─────────────────────────────────────────────────────────────────────
// Ported from SKR-EXP-001 client/operation215/native_vehicle_registration.cpp,
// asset profile DUAL (D5 certified: 612 = skr_xc60, 613 = skr_m4gts, Sultan
// 560 metadata). Depends on operation213 (relocated vehicle store, capacity
// 215) for descriptors 212..214.
//
// Kept from EXP-001: pre-resume archive identity + scanner hook, D0
// snapshot, D1 AddVehicleModel 612/613 with pointer-table isolation,
// metadata, D2B1 association allowlist, D2B2 one-at-a-time requests,
// D2B3 loaded-model validation and PLAYER.IMG drift classification.
// Removed (single-player harness only): live checkpoint, session nonce,
// D3 spawn harness, and TerminateProcess on failure. Here a failure leaves
// the model unmapped, it is never requested, and SKR-X keeps the proxy.
// ─────────────────────────────────────────────────────────────────────

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace skr::operation215 {

enum class Stage : uint32_t {
    NotStarted = 0,
    Inactive,     // nothing patched (archive not the certified one, unsupported exe, no suspended thread)
    Armed,        // store relocated, scanner hook installed, waiting for the image-0 scan
    Mapped,       // D0, D1, metadata and D2B1 passed
    Failed,       // scan done but a gate failed; no model is mapped
};
const char* StageName(Stage stage) noexcept;

// Per-model lifecycle (the separate states the integration must report).
enum class Resource : uint32_t {
    Unknown = 0,
    Missing,      // not in the active CUSTOM.IMG
    InArchive,    // archive identity verified (resource present in the archive GTA scans)
    Registered,   // descriptor + D2B1 mapping verified, not requested
    Requested,    // queued or in flight in CStreaming
    Ready,        // loaded and D2B3-validated
    Rejected,     // failed load validation or protected association drift
};
const char* ResourceName(Resource resource) noexcept;

// Inside SkrInitialize before samp.dll loads and before the main thread resumes.
// Returns false only if code was partially patched (the process must not run).
bool PrepareBeforeResume() noexcept;

Stage GetStage() noexcept;
Resource GetResource(int modelId) noexcept;
bool IsMapped(int modelId) noexcept;   // Registered, Requested or Ready
bool IsReady(int modelId) noexcept;
int GetTxdStreamId(int modelId) noexcept;

// Main thread. Re-reads the mapped associations after all archives are
// scanned. Returns the number still intact.
size_t RecheckMappings() noexcept;

// Main thread, non-blocking. Queues a mapped model; Tick issues the
// CStreaming request when no other custom model is in flight.
void Request(int modelId) noexcept;
void Tick() noexcept;

} // namespace skr::operation215
