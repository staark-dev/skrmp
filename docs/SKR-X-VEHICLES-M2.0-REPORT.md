# SKR-X VEHICLES M2.0 — Foundation Deliverables Report

**Date:** 2026-10-24
**Milestone:** M2.0 — Server-side foundation (model registry, Pawn API, protocol, unit tests)
**Verdict:** **GO for M2.1**

---

## 1. Test Results

| Suite | Checks | Failures | Status |
|-------|--------|----------|--------|
| SKRXVehiclesTests | 414 | 0 | PASS |
| SKRXTests (regression) | 158 | 0 | PASS |
| SKRNativeApiTests (regression) | 70 | 0 | PASS |
| SKRClientPolicyTests (regression) | — | 0 | PASS |

No regressions introduced. All existing SKR-X M1 and native API functionality preserved.

---

## 2. Build Artifacts

### Gate DLL
- **File:** `build-gate/Release/SKRGate.dll`
- **Size:** 277,504 bytes
- **SHA256:** `acb2eb47eb742c9f659836b6e73fd09c6cca5f1783ab467b489f20414e77f17c`
- **Compiler:** MSVC 14.44.35207 (Visual Studio 2022 BuildTools)
- **Warnings:** Upstream only (omp-sdk glm, omp-network bitstream, omp-sdk anim). Zero new warnings from VEHICLES code.

### Test Executable
- **File:** `build-gate/Release/SKRXVehiclesTests.exe`
- **SHA256:** `f286c00a7cd557b227baec771c0a7f53b56e0bb20f40b60ae5acf2bba52419bf`

---

## 3. Source File Hashes (SHA256)

| File | SHA256 |
|------|--------|
| `shared/protocol/skrx_vehicle.h` | `4ce9c26d8ebb29a0f6f6bda193c1afb47bc41cc7a9cca04490e0aca2d97828dc` |
| `server/skr-gate-omp/skrx_vehicles_server.h` | `ce080482eab5079cd70a55bc9a6e73022f7e5629b25ca3907160497e2c648081` |
| `server/skr-gate-omp/skrx_vehicles_manager.cpp` | `3fb3ac1573020803bf86b18157165aac0c55d6bb91dc18d1c3364486f3323fc1` |
| `server/skr-gate-omp/skrx_vehicles_server.cpp` | `133230a2dcfaf577d098786295ab787576b3ffe4ef145ea652e33586155198bc` |
| `server/gamemode-integration/skrx_vehicles.inc` | `c99908aa216b7af87c7bba884a4ce7b201008d0d72841dcd12f63ab7d54c6aee` |
| `tests/unit/test_skrx_vehicles.cpp` | `69d05858711fbf1fd1b41117fae6973052923dacb248a5c2775c6145a4135459` |

---

## 4. Protocol Summary

**Namespace:** VEHICLES (ID 1) on SKR-X Gateway (step 10, RPC 240/241)
**Version:** 1

| Op | ID | Direction | Purpose |
|----|----|-----------|---------|
| ModelRegistry | 1 | S → C | Send registered custom model definitions |
| VehicleAssign | 2 | S → C | Bind pool-slot to custom model ID |
| VehicleRevoke | 3 | S → C | Unbind pool-slot (vehicle destroyed) |
| VehicleReady | 4 | C → S | Client confirms model swap complete |
| VehicleBatchAssign | 5 | S → C | Multiple assigns in one packet |
| VehicleRespawn | 6 | S → C | Generation bump on respawn |

---

## 5. Pawn API

```pawn
native bool:SKRX_IsCustomModel(modelid);
native SKRX_CreateVehicle(modelid, Float:x, Float:y, Float:z, Float:rotation, color1, color2, respawnDelay);
native bool:SKRX_DestroyVehicle(vehicleid);
native SKRX_GetVehicleModel(vehicleid);
native bool:SKRX_IsVehicleReadyForPlayer(playerid, vehicleid);
```

- Stock models (400–611) pass through to `vehicles->create()` unchanged.
- Custom models (612+) create a proxy vehicle using the model's registered `proxyModelId`.
- `SKRX_GetVehicleModel()` returns the custom ID for tracked vehicles, stock model otherwise.
- Global `CreateVehicle()` semantics are NOT modified (standing constraint).

---

## 6. Architecture Decisions

1. **Proxy model approach** — open.mp rejects model IDs > 611. Custom vehicles use a stock proxy for pool allocation; VEHICLES protocol tells enhanced clients to swap visuals.
2. **Manager/natives split** — `VehicleManager` (standalone, no SDK dep) in its own TU for unit testing; Pawn natives in a separate TU that depends on the Gate component.
3. **Capacity limits** — 32 custom models, 256 custom vehicles. Fixed arrays, no heap allocation in hot paths.
4. **Generation counter** — monotonic per-vehicle generation detects stale client state after respawn.

---

## 7. Remaining Wiring (M2.1 Scope)

### Server-side
- [ ] `onPlayerDisconnect` → `vehicleManager_.ForgetPlayer(playerId)`
- [ ] Gateway ready event → `SendVehicleRegistryToPlayer()` + `FillActiveVehicles()` batch
- [ ] Vehicle stream-in → per-player VehicleAssign

### Client-side
- [ ] VEHICLES namespace handler in `skrx_client.cpp`
- [ ] Model registry storage + model swap on `WorldVehicleAdd` RPC
- [ ] VehicleReady acknowledgement back to server

### Gamemode
- [ ] Test gamemode: `/v 560` (stock), `/v 612`, `/v 613`, `/v 614` (custom)
- [ ] `/dv` destroy, `/vm` query model

### Integration
- [ ] OP215 `RegisterVehicle()` refactor to separate registration from local spawn
- [ ] Feature flag for two-client-build-line constraint

---

## 8. GO/NO-GO Verdict

**GO for M2.1.**

All M2.0 acceptance criteria met:
- Wire protocol fully specified and codec-tested (encode + decode + roundtrip)
- VehicleManager lifecycle verified (registration, tracking, player bindings, capacity limits, reset)
- Pawn natives compile and link against open.mp SDK
- Gate DLL builds cleanly with VEHICLES support
- Zero regressions in existing test suites
- No modifications to global `CreateVehicle()` semantics
- No modifications to production gamemodes or deployed components
