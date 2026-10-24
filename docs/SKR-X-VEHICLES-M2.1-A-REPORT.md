# SKR-X VEHICLES M2.1-A — Offline Implementation Report

**Date:** 2026-10-24
**Milestone:** M2.1-A — Compiles, tests pass, no regressions
**Verdict:** **GO for M2.1-B** (two-client live test)

---

## 1. Architecture: NATIVE-FIRST

Model 612 (Volvo XC60) is **never** model 560 from the client's perspective.

### Server path
1. `SKRX_CreateVehicle(612, ...)` → creates proxy vehicle (400 Landstalker) in open.mp pool
2. VehicleManager tracks: pool slot → custom model 612, generation counter
3. On API ready: `SendVehicleRegistryToPlayer()` + `SendVehicleBatchAssignToPlayer()`
4. On stream-in: `onVehicleStreamIn()` sends per-player VehicleAssign

### Client path
1. VEHICLES handler receives ModelRegistry → stores model definitions
2. VEHICLES handler receives VehicleAssign → maps pool-slot → custom model
3. **CVehiclePool::Create hook** (samp.dll R5 RVA 0x1F080) intercepts vehicle creation
4. Hook checks assignment map: if pool-slot has custom model, swaps `VehicleInfo::m_nType` from proxy → 612
5. GTA SA `CAutomobile` constructor receives model 612 — vehicle is **native from birth**

### Vanilla client safety
- Vanilla clients never receive VEHICLES protocol messages (requires CapGateway)
- They see the proxy model (400 Landstalker) — no crash, no protocol violation

---

## 2. Test Results

| Suite | Checks | Failures | Status |
|-------|--------|----------|--------|
| SKRXVehiclesTests | 414 | 0 | PASS |
| SKRXTests (regression) | 158 | 0 | PASS |
| SKRNativeApiTests (regression) | 70 | 0 | PASS |

**Total: 642 checks, 0 failures.**

---

## 3. Build Artifacts

### Gate DLL (server)
- **File:** `build-gate/Release/SKRGate.dll`
- **Size:** 307,712 bytes
- **SHA256:** `6b99a9381f6eef8fa6d5c60db56fe87bdc7a062bc80c2aecc29e49bbc5359dbf`
- **Warnings:** Upstream only (omp-sdk, omp-network). Zero new warnings from M2.1 code.

### Client DLL
- **File:** `build/Release/SKR.Client.dll`
- **Size:** 1,008,128 bytes
- **SHA256:** `075679c5c9d26c2e68e322872cb65923a774fad936ea8db7d17afa4c2093fc09`
- **Warnings:** One pre-existing signed/unsigned mismatch in shared protocol header.

---

## 4. Files Created

| File | Purpose |
|------|---------|
| `client/skrx/skrx_vehicles_client.h` | Client VEHICLES state, hook API, assignment map |
| `client/skrx/skrx_vehicles_client.cpp` | VEHICLES handler + CVehiclePool::Create hook |
| `server/gamemode-integration/skrx_vehicles_test.pwn` | Test filterscript: `/v`, `/dv`, `/vm`, `/vcount` |
| `docs/SKR-X-VEHICLES-M2.1-A-REPORT.md` | This report |

## 5. Files Modified

| File | Change |
|------|--------|
| `server/skr-gate-omp/skrx_gateway_server.cpp` | Fixed `enhanced.confirmed` → `enhanced.mode`, added batch assign + stream-in |
| `server/skr-gate-omp/skr_gate_component.cpp` | ForgetPlayer, VehicleEventHandler registration |
| `server/skr-gate-omp/skr_gate_component.hpp` | VehicleEventHandler inheritance |
| `server/skr-gate-omp/native_api_server.cpp` | Vehicle registry + batch on API ready |
| `client/skrx/skrx_client.cpp` | VEHICLES namespace dispatch |
| `client/samp-hooks/samp_hooks.cpp` | InstallVehiclePoolHook call |
| `CMakeLists.txt` | Added skrx_vehicles_client.cpp |

---

## 6. Bug Fixed

**`enhanced.confirmed` compilation error** — The M2.0 broadcast code referenced `data.enhanced.confirmed`, but `EnhancedSession` uses `mode` (a `ClientMode` enum), not a boolean `confirmed`. Fixed all 3 occurrences to `data.enhanced.mode != protocol::ClientMode::Enhanced`. This bug was latent in M2.0 because the broadcast paths weren't triggered until VehicleEventHandler was registered.

---

## 7. Standing Constraints Verified

- [x] Global `CreateVehicle()` semantics NOT modified
- [x] No modifications to production gamemodes
- [x] No modifications to samp.dll on disk
- [x] No second RakNet instance
- [x] Certified OP215 binaries NOT modified
- [x] Gate UID 0x534B5247 preserved
- [x] SA-MP 0.3.7-R5 compatibility preserved

---

## 8. Remaining for M2.1-B (Live Two-Client Test)

1. **OP215 integration:** ModelRegistry → trigger `RegisterVehicle()` for server-supplied models
2. **VehicleReady:** Client sends status acknowledgement back to server after swap
3. **Asset staging:** Ensure `SKR\op215\skr_vehicles.img` is deployed with XC60 DFF/TXD
4. **Two-client test:** `/v 612` → both enhanced clients see Volvo XC60, synchronized
5. **Vanilla fallback:** Confirm vanilla client sees proxy model, no crash

## 9. GO/NO-GO Verdict

**GO for M2.1-B.**

All M2.1-A acceptance criteria met:
- Server-side wiring complete and compiled
- Client-side VEHICLES handler + CVehiclePool::Create hook compiled
- NATIVE-FIRST architecture implemented: proxy→612 swap in hook, before GTA SA entity creation
- Zero regressions across 642 test checks
- Test gamemode ready for live testing
