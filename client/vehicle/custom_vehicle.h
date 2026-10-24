#pragma once
// ─────────────────────────────────────────────────────────────────────
// OP215 — SKR Enhanced Custom Vehicle System
// ─────────────────────────────────────────────────────────────────────
// Registers custom vehicle models (IDs 612+) at runtime without
// modifying any original GTA SA model (400–611).
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
    int         donorModelId;   // existing vehicle to clone model info from (e.g. 400 = Landstalker)
};

// Diagnostics snapshot for F8 overlay.
struct DiagInfo {
    int  modelId;
    bool registered;    // model info entry exists
    bool txdLoaded;     // TXD slot registered
    bool modelLoaded;   // DFF streamed in
    bool imgRegistered; // IMG added to streaming list
};

// Call once after SA-MP has finished initialization (samp.dll state == 5).
// Registers the custom IMG and all vehicle descriptors.
// Returns the number of vehicles successfully registered.
int  Initialize();

// Tear down — unload custom models, free allocations.
void Shutdown();

// Whether Initialize() completed successfully.
bool IsInitialized();

// Request streaming load of a custom vehicle.  Non-blocking.
void RequestLoad(int modelId);

// Check if a custom vehicle model is fully streamed.
bool IsLoaded(int modelId);

// Spawn a custom vehicle at the player's position.
// Returns true if the vehicle was created and added to the world.
// Requires the model to be loaded (calls RequestLoad + sync load internally).
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
