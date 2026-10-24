# GPS API-003.3 — Pawn natives (DRAFT, not wired)

Status: **proposal.** The client side exists (`client/gps/gps_checkpoint.h`,
`client/gps/gps_display_policy.h`); nothing below is on the wire, in the Gate or in
`skr_gate.inc` yet. Adding it changes the Native Runtime API contract
(`docs/protocol/SKR_NATIVE_API.md`), so it needs explicit approval first.

## Design rules

- Additive to Native API v1: a new capability bit, new ops, no change to ops 1–4.
- The checkpoint ids are per player, `1..4` (client slots 1..kMaxServer). The local
  map waypoint (slot 0) is never touched by the server.
- **Client arrival is advisory.** The client reports that the player entered the
  marker; the gamemode must validate it (position, vehicle, timing) before completing
  anything. A spoofed client can always send the report.

## Proposed wire additions

Capability `8` = Gps.

| Op | Dir | Payload | Client function |
|---|---|---|---|
| 5 GpsCheckpointSet | S→C | `u8 id(1..4) f32 x f32 y f32 z u8 flags(1 hasZ, 2 visible) u32 rgba f32 size(0.5..50) f32 drawDistance(10..3000) f32 arrivalRadius(0 = size) u8 policy(0 remove,1 keepFor,2 keepUntilCleared) u16 keepMs/100` | `SetServerCheckpoint` (create or modify) |
| 6 GpsCheckpointRemove | S→C | `u8 id(0 = all)` | `RemoveServerCheckpoint` / `ClearServerCheckpoints` |
| 7 GpsHudMode | S→C | `u8 mode(0 auto,1 full,2 compact,3 hidden)` | `panel::SetDisplayOverride` |
| — GpsCheckpointEntered | C→S | new step or Reply-style event: `u32 sessionId u8 id` | `TakeServerArrival` |

Validation on both sides as for v1: exact lengths, finite floats, map bounds ±3000,
enum ranges, unknown values rejected.

## Proposed natives (`skr_gate.inc`)

```pawn
native SKRE_GPS_SetCheckpoint(playerid, cpid, Float:x, Float:y, Float:z, colour = 0xA3E65B8C,
                              Float:size = 4.0, Float:drawDistance = 250.0, Float:arrivalRadius = 0.0,
                              SKRE_GPS_ARRIVAL:policy = SKRE_GPS_ARRIVAL_REMOVE, keepMs = 0,
                              bool:hasZ = true, bool:visible = true);
native SKRE_GPS_RemoveCheckpoint(playerid, cpid);      // cpid 0 = all server checkpoints
native SKRE_GPS_SetHudMode(playerid, SKRE_GPS_HUD:mode); // auto / full / compact / hidden

forward OnPlayerEnterGpsCheckpoint(playerid, cpid);    // ADVISORY: validate server-side
```
Return values follow v1: `1` sent, `0` invalid argument / not an SKR session / capability missing.

## Open questions for review

1. Should a server destination also drive navigation (route + panel), or only the
   world marker? (Client today: marker only; the route follows the map waypoint.)
2. Server checkpoint count: 4 per player is conservative because GTA has 32 3D-marker
   slots shared with SA-MP, scripts and the game (`C3dMarkers` at 0xC7DD58).
3. Rate limits for `GpsCheckpointSet` (proposal: ≤ 10 per second per player, server side).
