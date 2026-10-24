// SKR-X VEHICLES — Pawn API for custom vehicle models (M2)
//
// Custom models (612+) are server-authoritative. The server creates a stock
// proxy vehicle in the open.mp pool and tells enhanced clients to swap the
// visual model. Non-enhanced clients see the proxy.
//
// Stock models (400–611) pass through to standard CreateVehicle behavior.
//
// Requires: SKRGate.dll with SKR-X VEHICLES support.
//
// Usage:
//   #include "skrx_vehicles"
//
//   public OnPlayerCommandText(playerid, cmdtext[]) {
//       if (!strcmp(cmdtext, "/v 612")) {
//           new Float:x, Float:y, Float:z, Float:a;
//           GetPlayerPos(playerid, x, y, z);
//           GetPlayerFacingAngle(playerid, a);
//           new vid = SKRX_CreateVehicle(612, x + 3.0, y, z, a, -1, -1, 300);
//           if (vid != INVALID_VEHICLE_ID)
//               PutPlayerInVehicle(playerid, vid, 0);
//           return 1;
//       }
//       return 0;
//   }

#if defined _skrx_vehicles_included
    #endinput
#endif
#define _skrx_vehicles_included

// ── Error codes ─────────────────────────────────────────────────────

// SKRX_CreateVehicle returns INVALID_VEHICLE_ID on failure.
// Use the standard SA-MP INVALID_VEHICLE_ID (65535 or 0xFFFF).

// ── Natives ─────────────────────────────────────────────────────────

// Check if a model ID is a registered custom model on this server.
// Returns true for models 612+ that are in the server's SKRX vehicle registry.
// Returns false for stock models (400–611) and unregistered custom IDs.
native bool:SKRX_IsCustomModel(modelid);

// Create a vehicle.
//
// For custom models (612+):
//   - The model must be registered in the server's SKRX vehicle registry.
//   - A stock proxy vehicle is created in the open.mp pool.
//   - Enhanced clients are notified to swap the visual model.
//   - Non-enhanced clients see the proxy (stock) model.
//   - Returns the vehicle pool ID on success, INVALID_VEHICLE_ID on failure.
//
// For stock models (400–611):
//   - Delegates directly to the standard open.mp CreateVehicle.
//   - No SKRX protocol involvement.
//   - Returns the vehicle pool ID on success, INVALID_VEHICLE_ID on failure.
//
// Parameters:
//   modelid      — Vehicle model (400–611 for stock, 612+ for custom)
//   x, y, z      — Spawn position
//   rotation     — Spawn facing angle (degrees)
//   color1       — Primary color (-1 for random)
//   color2       — Secondary color (-1 for random)
//   respawnDelay — Seconds before empty vehicle respawns (-1 for no respawn)
native SKRX_CreateVehicle(
    modelid,
    Float:x, Float:y, Float:z,
    Float:rotation,
    color1, color2,
    respawnDelay
);

// Destroy a custom vehicle.
//
// Only destroys vehicles created with SKRX_CreateVehicle that have custom
// models. For stock vehicles, use the standard DestroyVehicle.
//
// Returns true if the vehicle was found and destroyed.
// Returns false if the vehicle ID is invalid or not a custom vehicle.
native bool:SKRX_DestroyVehicle(vehicleid);

// Get the true model ID for a vehicle.
//
// For custom vehicles: returns the custom model ID (612+), not the proxy.
// For stock vehicles: returns the stock model ID (400–611).
// For invalid vehicle IDs: returns 0.
native SKRX_GetVehicleModel(vehicleid);

// Check if a specific player has the custom vehicle model loaded and ready.
//
// Returns true if:
//   - The vehicle is a tracked custom vehicle.
//   - The player has confirmed the model is loaded (VehicleReady).
//   - The player's acknowledged generation matches the current generation.
//
// Returns false if:
//   - The vehicle is not custom.
//   - The player hasn't confirmed yet.
//   - The vehicle was respawned since the player last confirmed.
//   - The player's client doesn't support VEHICLES (non-enhanced).
native bool:SKRX_IsVehicleReadyForPlayer(playerid, vehicleid);
