# SKR-T01-F — Spawn Freeze Root Cause Verification & Minimal Correction

**Date:** 2026-10-25
**Priority:** P0 — Runtime Stability

---

## ADDENDUM (09:30) — Runtime evidence, root cause, interim fix

Supersedes the sections below wherever they disagree. Those sections are
pre-evidence analysis and are kept for history.

### Evidence (operator run 09:12, pid 42600)

- Loaded DLL: `preflight.log` resolved `build-test-certification-win32\Release\SKR.Client.dll`.
  The T01-F-only markers (`OP215 init enter`, `RequestLoad(%d) queued tid`) exist only
  in `D06BF2C3…`, so that build is confirmed as the one that ran.
- `op215.log`: `RegisterImg OK: imgId=0`, then `RequestLoad(612)` and `RequestLoad(613)`
  at tick 472993531 on tid 25764, the GTA main thread (the same tid as OP215 Initialize).
- After that tick the main thread produced nothing more:
  - no Present frame;
  - no `stage 15` and no `VISUAL_READY`;
  - no SKR-X pong (run 1 got its pong 32 ms after the ping);
  - no 5-second `HudDraw` DIAG.
- The D3D9 re-chain line at 472994359 came from a different thread.
- Run 1 had OP215 compiled out, issued no `RequestLoad`, and reached gameplay.

### Confirmed defects (binary and SDK evidence)

1. **No archive was ever registered.** `AddImageToList` (stub `0x407610` → body
   `0x1567B90`) scans `ms_files[8]` at `0x8E48D8` (stride 0x30). When every slot
   is in use it runs `xor eax,eax; ret`. Under SA-MP all 8 slots are taken, so
   it returned 0, an existing archive (SAMP\CUSTOM.IMG). OP215 treated only
   `< 0` as failure.
2. **Wrong `CStreamingInfo` layout.** The plugin-sdk `VALIDATE_OFFSET` layout is
   flags@0x06, imgId@0x07, cdPosn@0x08, cdSize@0x0C, loadState@0x10. OP215
   wrote imgId@0x08, cdPosn@0x0A, cdSize@0x0E and flags@0x12. For model 612
   that produced:
   - real flags = 0xFF (all request flags);
   - real imgId = 0xFF (index 255 into an 8-entry table);
   - cdPosn = 0x00010000;
   - cdSize = 0x10700000 sectors (about 527 GB).

   `RequestLoad` then queued these entries on the main thread, and the main
   thread stopped within about 30 ms.

**Not yet proven:** the exact blocking instruction inside CStreaming/CdStream.
That needs a hang dump; none was captured. The CStreamingInfo race (H6) and
DoSpawn are **not** implicated: DoSpawn was never entered.

### Interim correction (this build)

`client/vehicle/custom_vehicle.cpp`:
- `RegisterImg` no longer calls `AddImageToList`. It logs `ms_files[0..7]` and
  returns false. OP215 stays uninitialised, so `IsRegistered` is false, no
  `RequestLoad` is issued, and assigned vehicles keep their proxy.
- The `CStreamingInfo` offsets are corrected to the SDK layout. Registration
  now writes only imgId, cdPosn and cdSize, and refuses entries whose
  loadState or size is non-zero. This path is currently unreachable; it is
  kept correct for the future loader.

Earlier T01-F guards are retained: `IsRegistered` checks in RequestLoad,
IsLoaded, DoSpawn and HookedCreate. Model 614 stays rejected.

### SKR-EXP-001 CUSTOM.IMG loader — not ported (incompatibility)

- **Verified compatible:**
  - same `gta_sa.exe` (`A559AA77…`);
  - hook site `0x5B6170` = `83 EC 3C | 8B 44 24 40`, next instruction `0x5B6177`;
  - `__cdecl(path, imageId)`, sole caller `0x5B8310`;
  - the launcher creates GTA suspended and calls `SkrRemoteInit` before
    `ResumeThread`, so a pre-resume install is possible.
- **Blocking incompatibility:** the certified loader (`native_vehicle_registration.cpp`)
  prepares models 612 and 613 in descriptors 212/213 of the **operation213
  relocated CVehicleModelInfo store**. That store has capacity 215/216, 12
  patched code sites, executable-identity checks and D0/D1 gates.
  SKR Platform has no operation213 code; its OP215 uses `malloc`-cloned
  descriptors outside the store. Porting the loader therefore means porting
  operation213 first. That is a subsystem port, not a minimal change, and it
  needs a separate decision.
- **Asset staging:** the bundled `Release\samp\SAMP\custom.img` is the vanilla
  archive (8,192 bytes, `20DE16A9…`). The certified XC60/M4GTS profile needs
  a rebuilt `custom.img`. SKR-EXP-001's `prepare_custom_img` tool produces it.
  Not done.

### Build

| | |
|---|---|
| Dir | `build-test-certification-win32` (EXTERNAL_LOBBY, OP215 and TEST_CERTIFICATION all ON, Win32 Release) |
| DLL | 922,112 bytes, SHA-256 `F4D744AD2556867650C92EAB5E1B4CA1288104DE78EE485F6692A75897F98B0E` |
| Backup of previous | `Release\SKR.Client.dll.t01f-D06BF2C3-20261025-092609.bak` |
| Warnings | no new warnings |
| Tests | all 9 suites pass |

### Operator test (one run, no other changes)

Launch through the usual launcher, connect to the test server, spawn, and
let the server's model-612 assignment stream in.

**PASS requires all of these:**
- `op215.log` contains `RegisterImg DISABLED` and 8 `ms_files[...]` lines, and
  no `RequestLoad ... queued`;
- `skrx_vehicles.log` shows `ModelRegistry: OP215 not yet initialized` (or
  `NOT in OP215`), then `HOOK enter`, `HOOK defer` or `HOOK pass`, and
  `HOOK exit`;
- `handoff.log` reaches `VISUAL_READY` and `GAMEPLAY_CONFIRMED`;
- the vehicle shows as its proxy model, with no freeze.

If it still freezes, capture a hang dump: Task Manager → `gta_sa.exe` →
Create dump file.

**Verdict:** GO for this one diagnostic run. Custom-model rendering stays
NO-GO until the CUSTOM.IMG loader (with operation213) is ported.

---

## 1. Summary of Findings

### 1.1 Confirmed causes

**C1 — Unregistered model streaming (model 614):**
The server's `ModelRegistry` advertises model 614. The client's OP215 engine
registers only models 612 and 613 (`kDescriptors[]` at
`custom_vehicle.cpp:144–147`). When `HookedCreate` received a vehicle
assigned to model 614, it called `RequestLoad(614)` →
`CStreaming::RequestModel(614, 2)` against uninitialized `CStreamingInfo`.

**Evidence:** The `CStreamingInfo` entry for model 614 was never written by
`RegisterVehicle`. Its `imgId=0` (gta3.img), `cdPosn=0`, `cdSize=0` are
whatever CStreaming::Init left (zeroes). Requesting a zero-sized model from
gta3.img at offset 0 is undefined behavior in GTA's streaming subsystem.
Depending on the CStreaming implementation, this could:
- Leave model 614 permanently in `LOADSTATE_REQUESTED` (2), blocking the
  streaming queue.
- Cause a zero-byte read that returns immediately with corrupt data.
- Trigger an assertion or exception in the streaming thread.

**Fix applied:** `RequestLoad()` and `IsLoaded()` now validate via
`IsRegistered()` before touching CStreaming. `HookedCreate` checks
`IsRegistered()` before calling either. Unregistered models keep their
proxy and are logged clearly.

### 1.2 Hypothetical — requires runtime evidence

**H6 — CStreamingInfo race condition:**
`RegisterVehicle` writes to `CStreamingInfo[612]` and `[613]` on the GTA
main thread during the first `Tick()` call. The streaming thread may be
active at this point.

**Analysis (revised):** After closer inspection of GTA SA 1.0 US internals,
this hypothesis is **weaker than initially stated**:

- CStreaming::Init (0x5B8AD0) initializes ALL `CStreamingInfo` entries
  to default state: `nextOnCd=-1`, `prevOnCd=-1`, `imgId=0`, `cdPosn=0`,
  `cdSize=0`, `flags=0` (NOT_LOADED).
- Model IDs 612–613 are not used by any stock GTA model or stock SA-MP
  model. Their `CStreamingInfo` entries are in default state with all
  linked-list pointers set to -1.
- `RegisterVehicle` writes -1 to the same linked-list fields, then sets
  `imgId`, `cdPosn`, `cdSize` to the OP215 IMG values, and `flags=0`.
- The streaming thread only reads a `CStreamingInfo` entry when that model
  is actively being streamed (queued via `RequestModel`). Since model
  612/613 hasn't been requested yet at init time, the streaming thread
  has no reason to touch these entries.

**Remaining risk:** If model 612's `CStreamingInfo` entry is part of
CStreaming's internal free-list or sector-position ordered list (which
would require entries for ALL model IDs, including unused ones), then
writing to the linked-list fields could corrupt that list. This would
require reverse-engineering the CStreaming linked-list management to
confirm or rule out.

**Decision:** No speculative locking added. Diagnostic markers will capture
the pre-write state of each `CStreamingInfo` entry and timestamps, allowing
the operator test to confirm whether the entries were in a list before
registration.

### 1.3 Ruled out

| Component | Why ruled out |
|-----------|--------------|
| CVehiclePool::Create hook | Every path is non-blocking; trampoline verified correct |
| VFS hooks (samp_vfs) | Only redirect SA-MP resources; reentrancy-protected; do not intercept OP215 IMG or GTA streaming |
| OP215 Tick | Only checks F9/F8 keys; no I/O |
| Hook installation race | Hook written atomically (5-byte JMP); trampoline ready before detour |
| DoSpawn main-thread block | Only reachable via F9 keypress; not triggered during normal spawn |

---

## 2. Diagnostic Instrumentation

### 2.1 Markers added to `custom_vehicle.cpp`

| Location | Marker | Data |
|----------|--------|------|
| `Initialize()` entry | `=== OP215 Initialize === tid=%lu tick=%lu` | Thread ID, monotonic tick |
| `RegisterImg` enter/exit | `Initialize: RegisterImg enter/exit tick=%lu` | Timing |
| `RegisterVehicle` enter/exit | `Initialize: RegisterVehicle enter/exit model=%d tick=%lu` | Per-model timing |
| `CStreamingInfo` pre-write | `CStreamingInfo[%d] pre-write: nextCd=%d prevCd=%d imgId=%u cdPosn=%u cdSize=%u flags=%u` | Existing entry state before overwrite |
| `RequestLoad` | `RequestLoad(%d) queued tid=%lu` or `REJECTED: model not registered` | Thread + validation |
| `DoSpawn` entry | `DoSpawn(%d) ENTER tid=%lu tick=%lu` | Thread + timing |
| `DoSpawn` load timing | `DoSpawn(%d) model loaded after %lu ms` | Elapsed wait time |

### 2.2 Markers added to `skrx_vehicles_client.cpp`

| Location | Marker | Data |
|----------|--------|------|
| `HookedCreate` entry | `HOOK enter pool=%d model=%d tid=%lu tick=%lu` | Per-call timing + thread |
| `HookedCreate` reject | `HOOK reject pool=%d model=%d: not registered in OP215` | Unregistered model |
| `HookedCreate` exit | `HOOK exit pool=%d tick=%lu` | Exit timing |

### 2.3 Markers added to `game_state.cpp`

| Location | Marker | Data |
|----------|--------|------|
| OP215 deferred init entry | `DIAG OP215Init: OP215 init enter tid=%lu tick=%lu` | Thread + timing |
| OP215 deferred init exit | `DIAG OP215Init: OP215 init done tick=%lu` | Timing |

All markers are bounded (one-shot or per-vehicle-creation). No frame-by-frame
output. Thread IDs distinguish main thread from streaming thread.

---

## 3. CStreamingInfo Ownership Analysis

### 3.1 GTA SA 1.0 US streaming architecture

| Component | Address | Thread |
|-----------|---------|--------|
| `CStreamingInfo` array | 0x8E4CC0 | Shared (main + streaming) |
| `CStreaming::RequestModel` | 0x4087E0 | Main thread |
| `CStreaming::LoadAllRequestedModels` | 0x40EA10 | Main thread |
| `CStreaming::HasModelLoaded` | 0x4044C0 | Main thread (read-only) |
| `CStreaming::AddImageToList` | 0x407610 | Main thread |
| CdStream thread | Internal | Streaming thread |

### 3.2 CStreamingInfo entry layout (20 bytes)

```
0x00: nextOnCd  (int16)  — linked-list: next entry on CD queue
0x02: prevOnCd  (int16)  — linked-list: previous entry on CD queue
0x04: nextInImg (int16)  — linked-list: next entry in IMG
0x06: prevInImg (int16)  — linked-list: previous entry in IMG
0x08: imgId     (uint8)  — which IMG archive this model is in
0x0A: cdPosn    (uint32) — sector offset within the IMG
0x0E: cdSize    (uint32) — size in sectors
0x12: flags     (uint8)  — load state (0=not loaded, 1=loaded, 2=requested, 3=reading)
```

### 3.3 Pre-write state of model 612/613 entries

Unknown until diagnostic run. The diagnostic markers capture the existing
values before overwrite. Expected states:

| Scenario | nextOnCd | prevOnCd | imgId | cdPosn | cdSize | flags | Implication |
|----------|----------|----------|-------|--------|--------|-------|-------------|
| Default (CStreaming::Init) | -1 | -1 | 0 | 0 | 0 | 0 | Safe to overwrite |
| In a free-list | ≥0 | ≥0 | 0 | 0 | 0 | 0 | **Dangerous** — overwriting corrupts list |
| Residual from prior session | varies | varies | varies | varies | varies | varies | Need to inspect |

**The diagnostic run will answer this definitively.**

### 3.4 Synchronization mechanisms

No verified GTA SA streaming critical section was identified at a known
address. The proposed `CStreaming::ms_csStreamingLock` from the previous
investigation was speculative and is NOT used.

**Decision:** No speculative locking. If the diagnostic reveals that entries
are in a linked list, the fix would be to defer `RegisterVehicle` until
`CStreaming` reports those entries as idle, or to call `CStreaming::RemoveModel`
first to safely detach them.

---

## 4. DoSpawn Synchronous Wait Analysis

### 4.1 Purpose

`DoSpawn` (F9 debug spawn) requests a model via `CStreaming::RequestModel`,
then busy-waits (with `Sleep(10)` + `LoadAllRequestedModels`) until
`HasModelLoaded` returns true, or 5 seconds elapse.

### 4.2 Risk assessment

- **Blocks main thread** for up to 5 seconds — appears as a freeze.
- **Cannot cause infinite freeze** — 5-second timeout guarantees return.
- **Only reachable via F9 keypress** — not triggered during normal spawn.
- `LoadAllRequestedModels(false)` processes the streaming queue from the
  main thread. This is the standard GTA pattern used by mission scripts
  (`CStreaming::LoadScene`, etc.).

### 4.3 Can the wait become infinite?

No. `GetTickCount()` is monotonic; the 5-second guard is unconditional.
Even if the model never loads, DoSpawn returns false after 5 seconds.

### 4.4 Added safety

`DoSpawn` now validates `IsRegistered(modelId)` before requesting. An
unregistered model is rejected immediately without touching CStreaming.

### 4.5 Decision

No changes to the wait mechanism. It is bounded, debugger-only (F9),
and follows standard GTA patterns. The 5-second timeout is adequate
for development testing.

---

## 5. Minimal Corrections Applied

### 5.1 Unregistered model guard (Phase 4)

**File:** `client/vehicle/custom_vehicle.cpp`

Added `IsRegistered(int modelId)`:
- Linear scan of `g_slots[]` (O(n), n ≤ 8).
- Returns true only if model ID matches a registered descriptor.

Modified `RequestLoad(int modelId)`:
- Rejects with log if `!IsRegistered(modelId)`.
- No CStreaming call for unregistered models.

Modified `IsLoaded(int modelId)`:
- Returns false if `!IsRegistered(modelId)`.
- No CStreaming call for unregistered models.

Modified `DoSpawn(int modelId)`:
- Rejects with log if `!IsRegistered(modelId)`.

**File:** `client/vehicle/custom_vehicle.h`

Added `bool IsRegistered(int modelId)` declaration.

**File:** `client/skrx/skrx_vehicles_client.cpp`

Modified `HookedCreate`:
- Checks `IsRegistered(customModel)` before `IsLoaded` or `RequestLoad`.
- Unregistered models keep their proxy, logged as `HOOK reject`.
- Existing `OnModelRegistry` already logs the mismatch.

### 5.2 Diagnostic markers (Phase 1)

**Files modified:**
- `client/vehicle/custom_vehicle.cpp` — Initialize, RegisterImg,
  RegisterVehicle, CStreamingInfo pre-write, RequestLoad, DoSpawn
- `client/skrx/skrx_vehicles_client.cpp` — HookedCreate entry/exit/reject
- `client/game-state/game_state.cpp` — OP215 deferred init timing

All markers use `GetTickCount()` (monotonic) and `GetCurrentThreadId()`.
No frame-by-frame output.

---

## 6. Modified Files

| File | Changes |
|------|---------|
| `client/vehicle/custom_vehicle.h` | Added `IsRegistered` declaration |
| `client/vehicle/custom_vehicle.cpp` | Added `IsRegistered`; guarded `RequestLoad`, `IsLoaded`, `DoSpawn`; added diagnostic markers |
| `client/skrx/skrx_vehicles_client.cpp` | Guarded `HookedCreate` with `IsRegistered`; added entry/exit/reject markers |
| `client/game-state/game_state.cpp` | Added OP215 init timing markers via `LogDiag` |

---

## 7. Canonical Build

| Property | Value |
|----------|-------|
| Build dir | `build-test-certification-win32` |
| Generator | Visual Studio 18 2026 |
| Architecture | Win32 |
| SKR_EXTERNAL_LOBBY | ON |
| SKR_OP215 | ON |
| SKR_TEST_CERTIFICATION | ON |
| Config | Release |
| DLL | `SKR.Client.dll` |
| DLL size | 924,160 bytes |
| DLL SHA-256 | `D06BF2C3EA5A39C0700BF705E480CBB993D9877762227BFBF2B475574C9D1C75` |
| IMG | `skr_vehicles.img` (unchanged) |
| IMG size | 20,566,016 bytes |
| IMG SHA-256 | `83c88c9af3ba80d302d4857a46aa46f36ce6989c245b0d0bbc3670f18c67c1bc` |

---

## 8. Automated Regression Results

| Suite | Result |
|-------|--------|
| SKRTests | 26/26 passed |
| SKROpsTests | 17/17 passed |
| SKRIntroTests | 98/98 passed |
| SKRLobbyTests | 43/43 passed |
| SKRNativeInitTests | 0 failures |
| SKRPreflightTests | PASS |
| SKRHandoffTests | 85/85 passed |
| SKRLaunchIpcTests | 18/18 passed |
| SKRPauseControlsTests | PASS |

**Compiler warnings:** Same pre-existing warnings as prior builds (C4389 ×1,
C4505 ×2 in skrx_vehicles_client.cpp). No new warnings.

---

## 9. Operator-Controlled Runtime Test Instructions

### 9.1 Staging

Copy the built DLL to the launcher search path:

```
Source: build-test-certification-win32\Release\SKR.Client.dll
Dest:   <launcher-dir>\SKR.Client.dll
  and/or
        <Steam GTA>\SKR.Client.dll
```

Ensure `<Steam GTA>\SKR\op215\skr_vehicles.img` is present (20,566,016 bytes).

### 9.2 Scenario A — Baseline (OP215=OFF)

Build with `SKR_OP215=OFF`, `SKR_TEST_CERTIFICATION=ON`, `SKR_EXTERNAL_LOBBY=ON`.
Stage. Launch GTA, connect, spawn.

**Expected:** No freeze. Vehicles appear as stock models. No OP215 log.
VEHICLES hook installs (if `SKR_EXTERNAL_LOBBY=ON`) but no model swap occurs.

### 9.3 Scenario B — OP215 ON, no server vehicles

Build with all three flags ON (current canonical build). Server gamemode
creates NO custom vehicles (no VehicleAssign/BatchAssign sent).

**Expected:** No freeze. OP215 initializes and registers models 612/613.
Hook installs. All vehicles pass through as stock. Logs:
- `op215.log`: Initialize, RegisterImg, RegisterVehicle ×2, CStreamingInfo pre-write dumps.
- `skrx_vehicles.log`: HOOK pass for each vehicle stream-in.

**Diagnostic value:** If freeze occurs here, OP215 initialization itself
is the cause (isolates H6). Check `op215.log` for the last successful marker.

### 9.4 Scenario C — Full integrated path (612, 613 only)

Same build. Server creates vehicles with models 612 and 613 only.

**Expected:** No freeze. Hook swaps or defers vehicles. Logs show
`HOOK swap` or `HOOK defer` entries with timing.

### 9.5 Scenario D — Unregistered model (614)

Same build. Server creates a vehicle with model 614.

**Expected:** No freeze (after fix). Log shows:
```
HOOK enter pool=<N> model=<proxy> tid=<main> tick=<T>
HOOK reject pool=<N> model=614: not registered in OP215
HOOK defer pool=<N> proxy=<proxy> target=614 (model not loaded) defers=<N>
HOOK exit pool=<N> tick=<T>
```

Vehicle appears as proxy. No `RequestLoad(614)` call to CStreaming.

### 9.6 Evidence collection

After each scenario, collect:
- `<Steam GTA>\SKR\op215.log`
- `<Steam GTA>\SKR\skrx_vehicles.log`
- `<Steam GTA>\SKR\skr_game_state.log` (for OP215Init DIAG entries)

If the freeze reproduces, the last logged diagnostic marker identifies the
exact point of failure. A freeze without ANY op215.log entries means OP215
Initialize itself was not reached — the cause is elsewhere.

### 9.7 Hang dump (if freeze reproduces)

With operator approval:
1. Open Task Manager → Details → right-click `gta_sa.exe` → Create dump file.
2. The dump captures all thread call stacks, identifying the blocked thread.
3. The instruction pointer and call stack will confirm or refute the
   streaming thread deadlock hypothesis.

---

## 10. Rollback Procedure

If the diagnostic build causes issues beyond the known freeze:

1. Restore the previous DLL from backup:
   - `SKR.Client.dll.pre-m21c-*.bak` (pre-M2.1-C binary) or
   - The deployment backup in `docs/ai-memory/checkpoints/`.
2. The source changes are in 4 files (§6). Revert with:
   ```
   git checkout HEAD -- client/vehicle/custom_vehicle.h
   git checkout HEAD -- client/vehicle/custom_vehicle.cpp
   git checkout HEAD -- client/skrx/skrx_vehicles_client.cpp
   git checkout HEAD -- client/game-state/game_state.cpp
   ```
3. Rebuild with the canonical preset.

---

## 11. GO / NO-GO Verdict

**GO for operator-controlled diagnostic test.**

Rationale:
- The unregistered model guard (C1) is a verified correctness fix with no
  speculative elements. It prevents undefined CStreaming behavior for any
  model ID not in the OP215 descriptor table.
- The diagnostic markers are bounded, one-shot or per-event. They add
  negligible overhead and do not change behavior.
- No speculative locking or delay was added to CStreaming internals.
- All automated test suites pass with no regressions.
- The fix preserves all existing behavior: stock vehicles pass through,
  registered custom vehicles swap or defer as before, and the proxy
  fallback remains intact.

**Runtime PASS is NOT declared.** The freeze mechanism is diagnosed based
on code analysis but cannot be confirmed without operator-controlled GTA
evidence. The diagnostic build will confirm:

1. Whether C1 (unregistered model) was the actual trigger.
2. Whether H6 (CStreamingInfo race) is real — the pre-write dumps will
   show if entries 612/613 were in a linked list.
3. The exact point of failure if the freeze persists.

**Primary rule followed: diagnosed first, patched only what the evidence supports.**
