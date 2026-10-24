# SKR-X VEHICLES M2.1-B — Runtime Preflight & Audit Report

**Date:** 2026-10-24
**Milestone:** M2.1-B — Hook audit, OP215 integration, bounded diagnostics
**Verdict:** **GO for operator-controlled runtime test** (see §9)

---

## 1. Native vs Proxy Architecture — Layer Clarification

| Layer | Sees model ID | Notes |
|-------|--------------|-------|
| Pawn gamemode | 612 (via `SKRX_GetVehicleModel`) | `GetVehicleModel()` returns proxy (400) |
| open.mp vehicle pool | 400 (proxy) | `isValidVehicleModel()` rejects 612; `preloadModels[modelID-400]` OOB |
| VehicleManager (Gate) | 612 (true model) | Tracks real model, generation, proxy mapping |
| VEHICLES protocol (wire) | 612 | ModelRegistry + VehicleAssign carry the real ID |
| samp.dll CVehiclePool::Create | 400 → **612** (hook swaps) | VehicleInfo::m_nType rewritten before CAutomobile ctor |
| GTA SA CAutomobile | **612** | Vehicle is born native — never lives as proxy |
| OP215 CModelInfo array | 612 at `0xA9B0C8[612]` | Cloned from donor, patched with custom DFF/TXD |
| Vanilla client (no SKRX) | 400 (proxy) | Never receives VEHICLES protocol, safe fallback |

**Genuine native 612 in open.mp** would require patching `isValidVehicleModel()` and
extending the 212-entry `preloadModels` array.  This is NOT done in M2.1 — the proxy
pattern is the correct approach for SA-MP/open.mp compatibility.

---

## 2. Vehicle Creation Ordering — Proven Sequence

```
Server:
  1. SKRX_CreateVehicle(612, x, y, z, ...)
  2. VehicleManager selects proxy 400, creates with open.mp CreateVehicle(400, ...)
  3. VehicleManager stores: slot N → model 612, generation G
  4. On player API ready: SendVehicleRegistryToPlayer() — ModelRegistry (all models)
  5. On player API ready: SendVehicleBatchAssignToPlayer() — BatchAssign (all active)
  6. On stream-in:  BroadcastVehicleAssign (per-vehicle, per-player)

Client:
  7. OnModelRegistry → stores model defs, verifies OP215 registration, requests load
  8. OnVehicleAssign / OnBatchAssign → maps pool slot N → custom model 612
  --- BINDING EXISTS IN ASSIGNMENT MAP ---
  9. samp.dll streams in vehicle → calls CVehiclePool::Create(VehicleInfo*)
 10. HookedCreate fires BEFORE original:
     a. Reads m_nType at offset 2 (pack(1)) — sees proxy 400
     b. Looks up pool ID in assignment map → finds 612
     c. Checks OP215 model loaded (CStreaming::HasModelLoaded)
     d. If loaded: swaps m_nType 400 → 612, increments swapCount
     e. If NOT loaded: keeps proxy, requests async load, increments deferCount
 11. Original CVehiclePool::Create runs with m_nType = 612
 12. GTA SA CAutomobile ctor uses model 612 → vehicle is native from birth
```

**Ordering guarantee:** Steps 7–8 (model registry + assignment) arrive on the
gateway RPC before samp.dll can stream in the vehicle, because:
- ModelRegistry and BatchAssign are sent on API ready (player joined, before any
  vehicle streaming)
- VehicleAssign is sent on stream-in event, which fires before samp.dll's
  CVehiclePool::Create is called for that vehicle

**Edge case — race condition:** If stream-in fires before BatchAssign arrives
(theoretically possible if server is slow), the hook finds no assignment and
passes through as proxy.  The vehicle appears as model 400.  On the next
respawn or re-stream, the assignment exists and the swap succeeds.
This is safe degradation, not a crash.

---

## 3. SA-MP R5 Hook Audit

### 3.1 Binary verification
- **samp.dll R5 SHA-256:** `B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A75443253A2`
- **PE timestamp:** `0x6372C39E`
- **Image size:** `0x27E000`
- **Pinned in:** `client/samp-loader/samp_loader.cpp`

### 3.2 Hook target
- **Function:** `CVehiclePool::Create(VehicleInfo* pInfo)`
- **RVA:** `0x1F080` (confirmed in samp-api: `CVehiclePool.cpp` → `GetAddress(0x1F080)`)
- **Calling convention:** `__thiscall` (ECX = CVehiclePool*, stack arg = VehicleInfo*)

### 3.3 Prolog bytes (verified)
```
55              push ebp
8B EC           mov ebp, esp
83 EC 08        sub esp, 8
```
6 bytes, no relative branches — safe to relocate into trampoline.

### 3.4 VehicleInfo layout — CRITICAL BUG FOUND AND FIXED

**Bug:** M2.1-A code read `m_nType` at offset 4 (assuming natural alignment).
But VehicleInfo uses `#pragma pack(push, 1)` — byte-packed.

**Actual pack(1) layout:**
| Offset | Field | Type | Size |
|--------|-------|------|------|
| 0 | m_nId | uint16_t (ID) | 2 |
| 2 | m_nType | int32_t | 4 |
| 6 | m_position | CVector (3×float) | 12 |
| 18 | m_fRotation | float | 4 |

**Fix applied:** All reads/writes of m_nType now use offset 2 (constant `kVI_Type`).

### 3.5 Trampoline safety
- `VirtualAlloc` 32 bytes with `PAGE_EXECUTE_READWRITE`
- Copies 6 prolog bytes (verified match against `kExpectedProlog`)
- Appends `JMP rel32` back to `original + 6`
- Detour: `E9 rel32` + `NOP` written over original 6 bytes
- `VirtualProtect` + `FlushInstructionCache` used correctly

### 3.6 Thread safety
- Hook installed on the SA-MP hook thread after game state >= 8
- All VehiclePool::Create calls happen on GTA main thread (RPC handler)
- Assignment map reads are on the same thread — no concurrent mutation
- `g_state.hookInstalled` is checked before installation (idempotent)

### 3.7 Hook verdict: **SAFE** (with the offset fix applied)

---

## 4. OP215 Integration Verification

### 4.1 Registration path
`OnModelRegistry` now:
1. Stores model definitions from server
2. If OP215 is initialized: calls `skr::vehicle::GetDiag(modelId)` to verify registration
3. If registered: calls `skr::vehicle::RequestLoad(modelId)` for pre-loading
4. Logs registration status per model

### 4.2 Hook model-loaded guard
`HookedCreate` now checks before swapping:
1. `skr::vehicle::IsInitialized()` — OP215 engine running
2. `skr::vehicle::IsLoaded(customModel)` — model DFF/TXD streamed in
3. If not loaded: keeps proxy, calls `RequestLoad()` for async loading
4. Increments `deferCount` for diagnostics

### 4.3 OP215 engine (custom_vehicle.cpp, not modified)
- Hardcoded descriptor: `{612, "skr_xc60", "Volvo XC60 2009", 400}`
- `RegisterVehicle()` clones donor `CVehicleModelInfo`, patches hashKey/texDict/objectNumber
- Registers at `CModelInfo::ms_modelInfoPtrs[612]` (address `0xA9B0C8`)
- IMG file: `SKR\op215\skr_vehicles.img` with DFF/TXD sector entries

### 4.4 Certified OP215 artifacts: **NOT MODIFIED** ✓

---

## 5. Bounded Diagnostics

### 5.1 Log file
- **Path:** `<GTA SA>\SKR\skrx_vehicles.log`
- **Format:** `[tickcount] message`
- **Direct file write** (same pattern as OP215 engine — no ring buffer)

### 5.2 Logged events
| Event | Data points |
|-------|------------|
| Init / Reset | model count, assign count, swap count, defer count |
| ModelRegistry | count, per-model OP215 status |
| VehicleAssign | pool ID, model, generation |
| VehicleRevoke | pool ID |
| BatchAssign | count, total active |
| VehicleRespawn | pool ID, generation |
| HOOK pass | pool ID, model (no swap needed) |
| HOOK swap | pool ID, proxy → model, total swaps |
| HOOK defer | pool ID, proxy, target, reason, total defers |
| HOOK install | samp base, target addr, prolog bytes, trampoline addr |
| VehicleReady | pool ID, model, generation, status (queued) |

### 5.3 Runtime test data points (spec requirements)
The log captures all required diagnostic data:
- ✅ Hook installation confirmation with addresses
- ✅ Prolog byte verification
- ✅ Every model swap with before/after model IDs
- ✅ Every deferred swap with reason
- ✅ Model registry reception and OP215 verification
- ✅ Assignment map operations
- ✅ Swap and defer counters

---

## 6. Build Artifacts

### SKR.Client.dll
- **Size:** 1,019,904 bytes
- **SHA-256:** `1516e3e61c62506b3c4584e12874278f176c0cc4081caed252b89cab278b8369`
- **New warnings:** 0 (only pre-existing: signed/unsigned in skrx_vehicle.h, unreferenced static functions)

### SKRGate.dll
- **Size:** 310,272 bytes
- **SHA-256:** `f5c7376a61ba72cc7287c19e9e5e355ea4779a6e17353dc4d30ea496b6b081e2`
- **New warnings:** 0 (only upstream omp-sdk/omp-network)

---

## 7. Test Results

| Suite | Checks | Failures | Status |
|-------|--------|----------|--------|
| SKRXVehiclesTests | 414 | 0 | **PASS** |
| SKRXTests (regression) | 158 | 0 | **PASS** |
| SKRNativeApiTests (regression) | 70 | 0 | **PASS** |

**Total: 642 checks, 0 failures.**

---

## 8. Bugs Found and Fixed

### 8.1 VehicleInfo m_nType offset (CRITICAL)
- **What:** Hook read `m_nType` at byte offset 4 (natural alignment assumption)
- **Root cause:** VehicleInfo uses `#pragma pack(push, 1)` — m_nType is at offset 2
- **Impact:** Would read garbage model ID at runtime, corrupting wrong field on write
- **Fix:** Changed offsets to 2 (`kVI_Type = 2`), added named constants

### 8.2 Missing model-loaded guard
- **What:** Hook swapped model ID unconditionally
- **Impact:** If OP215 model not yet streamed in, CAutomobile ctor would crash (null RwObject)
- **Fix:** Added `IsLoaded()` check; if not loaded, keeps proxy and calls `RequestLoad()`

### 8.3 No OP215 integration in OnModelRegistry
- **What:** ModelRegistry handler stored definitions but did not verify/load via OP215
- **Impact:** Models might not be pre-loaded when the hook fires
- **Fix:** Added `GetDiag()` verification and `RequestLoad()` call per model

---

## 9. Standing Constraints Verified

- [x] Global `CreateVehicle()` semantics NOT modified
- [x] No modifications to production gamemodes
- [x] No modifications to samp.dll on disk
- [x] No second RakNet instance
- [x] Certified OP215 binaries NOT modified (custom_vehicle.cpp/h unchanged)
- [x] Gate UID 0x534B5247 preserved
- [x] SA-MP 0.3.7-R5 compatibility preserved
- [x] SA-MP 0.3.DL rejected
- [x] BMW, Audi, Dacia NOT added
- [x] GTA not launched automatically
- [x] No deployed servers modified

---

## 10. Known Limitations (M2.1-B)

1. **VehicleReady not transmitted:** The client→server acknowledgement is logged and counted but not sent on the wire. Requires exposing `SendOnRpc240` or adding a public gateway send API. Not blocking for the first test.

2. **Deferred swaps require re-stream:** If the model isn't loaded when the hook fires, the vehicle appears as the proxy until the next stream-in/respawn cycle.

3. **`IsModelStreamedIn` and `SendVehicleReady` are unreferenced:** C4505 warnings. Both exist for the runtime path but aren't called yet in test builds. `IsModelStreamedIn` is called indirectly through the `#ifdef SKR_OP215` path; `SendVehicleReady` awaits the send-path wiring.

---

## 11. GO/NO-GO Verdict

### **GO** — Cleared for operator-controlled runtime test.

**Conditions met:**
- ✅ All 5 M2.1-B audit sections completed
- ✅ Critical VehicleInfo offset bug found and fixed
- ✅ Model-loaded guard prevents crash on unloaded models
- ✅ OP215 integration wired (ModelRegistry → GetDiag + RequestLoad)
- ✅ Comprehensive file-based diagnostics for every hook event
- ✅ 642/642 test checks pass, zero regressions
- ✅ Build artifacts produced with complete SHA-256 hashes
- ✅ All standing constraints verified

**Runtime test sequence (operator-controlled):**
1. Deploy `SKRGate.dll` to test server with `skrx_vehicles_test.pwn`
2. Deploy `SKR.Client.dll` to test client with OP215 assets
3. Connect two enhanced clients
4. `/v 560` — stock Sultan, verify vanilla creation (hook passes through)
5. `/v 612` — Volvo XC60, verify:
   - `skrx_vehicles.log`: HOOK swap pool=N proxy=400 -> model=612
   - Vehicle visually renders as XC60, not Landstalker
   - `/vm N` shows true=612 stock=400 custom=yes
6. `/dv` — destroy, verify cleanup
7. Repeat with second client — both see same model
8. Vanilla client fallback: connect without SKR → sees proxy 400

**Blocking for GO:** None. All audit items resolved.
