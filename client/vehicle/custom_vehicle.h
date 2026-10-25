#pragma once
// ─────────────────────────────────────────────────────────────────────
// OP215 — SKR Enhanced Custom Vehicle System
// ─────────────────────────────────────────────────────────────────────
// Custom vehicle models (IDs 612+) are registered before the first frame
// through SAMP\CUSTOM.IMG (operation215/native_vehicle_registration.h); no original GTA SA
// model (400–611) is modified and no extra IMG archive is added.
//
// GTA SA 1.0 US (x86).  SA-MP 0.3.7-R5.
// ─────────────────────────────────────────────────────────────────────

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

namespace skr::vehicle {

// Descriptor for a custom vehicle asset.
struct VehicleDescriptor {
    int         modelId;        // e.g. 612, 613
    const char* internalName;   // e.g. "skr_xc60" — also DFF/TXD base name in the IMG
    const char* displayName;    // e.g. "Volvo XC60 2009"
    int         donorModelId;   // metadata/handling donor (560 = Sultan, certified in EXP-001 D5)
};

// Diagnostics snapshot for F8 overlay.
struct DiagInfo {
    int  modelId;
    bool registered;    // model info entry exists
    bool txdLoaded;     // TXD slot registered
    bool modelLoaded;   // DFF streamed in
    bool imgRegistered; // CUSTOM.IMG scan mapped and verified
};

// Call once on the GTA thread (first CGame::Process tick). Adopts the models
// mapped during the CUSTOM.IMG scan. Returns the number of mapped vehicles.
int  Initialize();

// Tear down module state (engine-owned descriptors are left in place).
void Shutdown();

// Whether Initialize() completed successfully.
bool IsInitialized();

// Whether a model ID is registered in the OP215 descriptor table.
bool IsRegistered(int modelId);

// Request streaming load of a custom vehicle.  Non-blocking.
// Rejected if the model is not registered.
void RequestLoad(int modelId);

// Check if a custom vehicle model is fully streamed.
bool IsLoaded(int modelId);

// Spawn a custom vehicle in front of the player (pool-allocated CAutomobile).
// Returns true if it was created now; if the model is not streamed yet it is
// requested and the spawn happens on a later Tick (never waits).
bool SpawnAtPlayer(int modelId);

// F8 diagnostic info for the overlay.
DiagInfo GetDiag(int modelId);

// Number of registered custom vehicles.
int  GetRegisteredCount();

// Get descriptor by index (0-based).  Returns nullptr if out of range.
const VehicleDescriptor* GetDescriptor(int index);

// Process per-frame: handles F9 spawn cycling, F8 diagnostics overlay.
// Call from the game-thread tick (CGame::Process hook).
void Tick();

} // namespace skr::vehicle
