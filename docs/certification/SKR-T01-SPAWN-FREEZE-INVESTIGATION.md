# SKR-T01 — Spawn Freeze Investigation

**Date:** 2026-10-25
**Severity:** P0 CRITICAL
**Status:** Root cause analysis complete — operator isolation test required

---

## 1. Symptom

GTA San Andreas freezes during player spawning with the integrated
SKR-X VEHICLES + OP215 configuration
(`SKR_EXTERNAL_LOBBY=ON, SKR_OP215=ON, SKR_TEST_CERTIFICATION=ON`).

The freeze is a hard lock (no crash dialog), occurring at or around the
spawn transition (game state change).

---

## 2. Canonical Build Under Investigation

| Property | Value |
|----------|-------|
| Build dir | `build-test-certification-win32` |
| Preset | All three flags ON |
| DLL size | 922,624 bytes |
| DLL SHA-256 | `8d7f1f97def1c7ffd0a67a8d0b90d9f31895fd080e85386b81004743d24eef28` |
| IMG size | 20,566,016 bytes |
| IMG SHA-256 | `83c88c9af3ba80d302d4857a46aa46f36ce6989c245b0d0bbc3670f18c67c1bc` |

---

## 3. Code Path Analysis

### 3.1 OP215 Initialization (game_state.cpp:946–958)

`skr::vehicle::Initialize()` is called **once** from `Tick()` on the GTA
main thread, on the first `CGame::Process` frame after hooks are installed.
It calls:

1. `RegisterImg()` → `CStreaming::AddImageToList("SKR\\op215\\skr_vehicles.img")`
   — **non-blocking**, returns an IMG ID.
2. `RegisterVehicle()` for each descriptor (models 612, 613) — clones
   `CVehicleModelInfo` from the donor, patches fields, writes to
   `CModelInfo::ms_modelInfoPtrs[]` and `CStreamingInfo[]`.
   — **non-blocking**, pure memory writes.

**Verdict:** Initialize is safe. No file I/O, no synchronous loads, no waits.

### 3.2 CVehiclePool::Create Hook (skrx_vehicles_client.cpp:118–162)

`HookedCreate` runs on the GTA main thread whenever SA-MP's
`CVehiclePool::Create` is called (vehicle stream-in or server-created vehicle).

Execution path:
1. Reads `VehicleInfo` fields: pool ID and model ID — **register reads only**.
2. `GetAssignedModel(poolId)` — linear scan of up to 256 assignment slots.
   **O(n) but n ≤ 256, no I/O, no locks.**
3. If assigned and model differs from proxy:
   - `skr::vehicle::IsLoaded(customModel)` — calls
     `CStreaming::HasModelLoaded`, a single array lookup. **Non-blocking.**
   - If NOT loaded: `skr::vehicle::RequestLoad(customModel)` — calls
     `CStreaming::RequestModel`, queues for async load. **Non-blocking.**
     Vehicle keeps its proxy model (e.g. Landstalker 400). No swap occurs.
   - If loaded: swaps `pInfo->m_nType` to the custom model ID. **Memory write only.**
4. Calls original `CVehiclePool::Create` via trampoline. **Always.**

**Verdict: The hook CANNOT cause a freeze.** Every path is non-blocking.
The worst case is a deferred swap (vehicle appears as proxy), never a hang.

### 3.3 Model 614 Mismatch

**Critical finding:** The client registers only 2 models:
- **612** — Volvo XC60 2009 (donor: 400 Landstalker)
- **613** — BMW M4 GTS (donor: 475 Sabre)

The `kDescriptors[]` array at `custom_vehicle.cpp:144–147` has exactly two
entries. Model **614 does not exist on the client**.

If the server's `ModelRegistry` includes model 614:
- `OnModelRegistry` (line 282–303) iterates all server models and calls
  `GetDiag(614)` — scans `g_slots[]`, finds no match, returns
  `{registered=false}`. Logs:
  `"model 614 'name' NOT in OP215 (server wants it, client doesn't have it)"`
  **Non-blocking. Safe.**

If a `VehicleAssign` or `BatchAssign` references model 614:
- `GetAssignedModel` returns `614` for the pool ID.
- `HookedCreate` checks `IsLoaded(614)`:
  - `CStreamingInfo` for model 614 was never initialized by `RegisterVehicle`.
    Its `CStreamingInfo` entry contains whatever GTA left there (typically
    zeroes from `.bss`). `HasModelLoaded` checks a flag — with zero flags it
    returns `false`.
  - The hook calls `RequestLoad(614)` → `CStreaming::RequestModel(614, 2)`.
    **This is the risk:** CStreaming will look up `CStreamingInfo[614]`,
    find `imgId=0` (gta3.img), `cdPosn=0`, `cdSize=0`. Requesting a model
    with zero streaming info either:
    - (a) Silently fails (size 0 means nothing to read), or
    - (b) Causes CStreaming to enter a bad state — a zero-sector read from
      gta3.img could confuse the streaming queue.

  **However**, even in case (b), `RequestLoad` is asynchronous — it queues the
  request and returns. It does not block the main thread. The freeze would
  not be an infinite loop here.

  The vehicle keeps its proxy model (400 Landstalker) because `modelReady`
  is false. The `CAutomobile` constructor runs with model 400, which is
  always loaded. **No crash from unloaded model pointer.**

**Verdict:** Model 614 mismatch is a correctness bug (orphaned streaming
request) but NOT a freeze cause. The hook path remains non-blocking.

### 3.4 DoSpawn — F9 Test Spawn (custom_vehicle.cpp:363–437)

`DoSpawn` has a **synchronous blocking wait loop** on the GTA main thread:

```cpp
while (!hasLoaded(modelId)) {
    if (GetTickCount() - start > 5000) {
        return false;  // TIMEOUT
    }
    Sleep(10);
    loadAll(false);
}
```

This blocks the main thread for up to **5 seconds**, calling
`LoadAllRequestedModels` in a tight loop with 10ms sleeps.

**However:** `DoSpawn` is only reachable from:
- `SpawnAtPlayer(modelId)` → `DoSpawn(modelId)`
- `SpawnAtPlayer` is called from `Tick()` on **F9 keypress** only.

This path is **not triggered during normal player spawning**. It is a debug
feature for testing custom vehicle placement. The operator would need to
press F9 during spawning to hit this.

**Verdict:** Not the freeze cause during normal spawn, but a latent risk
if F9 is pressed. The 5-second synchronous load could appear as a freeze.

### 3.5 VFS Hooks (samp_vfs.cpp — TEST_CERTIFICATION only)

The certification VFS hooks intercept:
- `CreateFileW`, `CreateFileA`
- `GetFileAttributesW`, `GetFileAttributesA`
- `FindFirstFileExW`

**Reentrancy protection:** Thread-local counter (`__declspec(thread) int`),
`ReentrancyScope` RAII. If reentrant, immediately delegates to the real API.
**Safe against self-deadlock.**

**Redirect scope:** Only files matching `kSampResourceFiles[]`:
- `SAMP\SAMP.img`, `SAMP\*.ide`, `*.txd`, `bass.dll`, etc.
- **Does NOT include** `SKR\op215\skr_vehicles.img` or any GTA native
  IMG/DFF/TXD files.

**Therefore:** The VFS hooks do not intercept OP215 asset loading or GTA's
own model streaming. They add overhead to every `CreateFile` call (one
`GetFullPathNameW` + string compare per known resource), but this is
microseconds per call.

**Potential issue:** Every `CreateFile` call goes through the hook, including
calls from `fopen`/`_wfopen` inside the OP215 and VEHICLES log functions.
The log functions open and close a file on every log call. During heavy
logging (e.g., `BatchAssign` with many vehicles), this creates:
- Hook intercept → reentrancy check → `TryRedirect` → no match → passthrough.
- ~10-20 microseconds per log call × many calls.

This adds latency but should not cause a hard freeze.

**Verdict:** VFS hooks are safe. No blocking, no interference with streaming.

### 3.6 OP215 Tick (custom_vehicle.cpp:552–574)

Called every `CGame::Process` frame. Does:
1. Checks F9 (spawn) and F8 (diagnostics overlay) keypresses.
2. Both use `GetAsyncKeyState` — non-blocking.

**Verdict:** Safe. Cannot freeze.

### 3.7 VEHICLES Init / InstallVehiclePoolHook (samp_hooks.cpp:249–251)

Called once from `SampHookThread` after game state ≥ 8 and a 500ms sleep.
- `Init()` initializes the VEHICLES state structure.
- `InstallVehiclePoolHook(sampAddr)` does the inline hook: PE verify, build
  trampoline, write detour. Pure memory operations.

**Verdict:** One-time setup, non-blocking.

---

## 4. Freeze Hypothesis Matrix

| # | Hypothesis | Evidence | Likelihood |
|---|-----------|----------|------------|
| H1 | OP215 DoSpawn blocks main thread | Synchronous 5s wait loop exists, but only on F9 keypress — not triggered during normal spawn | **LOW** (unless F9 was held) |
| H2 | Model 614 orphaned streaming request corrupts CStreaming | RequestModel(614) with uninitialized CStreamingInfo → possible streaming queue corruption | **MEDIUM** — could cause delayed freeze during stream-in |
| H3 | VFS hooks block CStreaming file I/O | VFS only redirects SA-MP resources, not GTA/OP215 files | **NONE** — ruled out |
| H4 | OP215 Initialize runs during spawn transition | Initialize is called once on first Tick, which is first CGame::Process after hooks install — could overlap with first spawn | **MEDIUM** — if Initialize's CStreaming writes overlap with a concurrent stream-in request |
| H5 | CVehiclePool::Create hook causes infinite loop | Hook always falls through to trampoline, trampoline JMPs back to original+5 | **NONE** — ruled out |
| H6 | Collision between OP215 CStreamingInfo writes and GTA streaming thread | RegisterVehicle writes to CStreamingInfo[612..613] without synchronization, while CStreaming may be actively using those entries | **HIGH** — GTA's streaming runs on a separate thread |
| H7 | Log I/O during hook execution blocks main thread | fopen/fclose on every hook call, inside VFS hook scope | **LOW** — adds latency, not a freeze |

---

## 5. Primary Suspect: H6 — CStreamingInfo Race Condition

**This is the most likely freeze cause.**

### Evidence

`skr::vehicle::Initialize()` is called from `Tick()` on the first
`CGame::Process` frame. At this point:
- GTA's streaming thread is already running.
- `CStreaming` is actively loading models for the initial scene.
- The player may be spawning (game state ≥ 8 triggers spawn processing).

`RegisterVehicle()` writes directly to `CStreamingInfo[612]` and
`CStreamingInfo[613]`:

```cpp
auto* dffSi = GetStreamingInfo(desc->modelId);  // e.g., 612
*reinterpret_cast<int16_t*>(dffSi + kSI_NextOnCd)  = -1;
*reinterpret_cast<int16_t*>(dffSi + kSI_PrevOnCd)  = -1;
// ... 6 more field writes
```

These writes are **not atomic** and **not synchronized** with the streaming
thread. If GTA's streaming thread is reading `CStreamingInfo[612]` (e.g.,
because model 612 is in the same linked list as another model being streamed),
it could read partially-written data:
- Corrupted `cdPosn` → streaming thread seeks to wrong offset in gta3.img.
- Corrupted `imgId` → streaming thread opens wrong IMG archive.
- Corrupted `nextOnCd`/`prevOnCd` → streaming linked-list corruption →
  infinite loop in CStreaming's linked-list traversal.

**A linked-list corruption in CStreaming would cause an infinite loop on
the streaming thread**, which would never complete loading. The main thread,
waiting for streaming to finish (e.g., `LoadAllRequestedModels` during spawn),
would block indefinitely — **a freeze with no crash**.

### Why model IDs 612–613 are dangerous

GTA SA has model info slots 0–19999. Stock vehicles use IDs 400–611.
**Model ID 612 is the first slot after the last stock vehicle (611, Utility Van).**

GTA initializes `CStreamingInfo` for all stock models (0–611 + their TXDs).
Model 612's `CStreamingInfo` entry may be:
- Zeroed (unused) — writing to it is relatively safe.
- Part of a free-list or sentinel — writing to it corrupts the list.

The linked-list fields (`nextOnCd`, `prevOnCd`, `nextInImg`, `prevInImg`)
are particularly sensitive. Setting them to `-1` while the streaming thread
reads them could cause list traversal to skip entries or loop.

---

## 6. Secondary Suspect: H2 — Model 614 Orphaned Request

If the server assigns model 614 to a vehicle pool slot, `HookedCreate` calls
`RequestLoad(614)`. Since model 614 was never registered:
- `CStreamingInfo[614]` is uninitialized (likely zeroes).
- `imgId=0` points to `gta3.img`, `cdPosn=0`, `cdSize=0`.
- CStreaming may queue a zero-size read, which could confuse the streaming
  thread or leave the model permanently in "loading" state.

This alone won't cause a freeze, but combined with H6 (corrupted streaming
state), it could contribute to streaming thread deadlock.

---

## 7. Ruled Out

| Component | Why |
|-----------|-----|
| CVehiclePool::Create hook | Every path is non-blocking; always calls original |
| VFS hooks (samp_vfs) | Only redirect SA-MP resources; reentrancy-protected |
| OP215 Tick | Only checks F9/F8 keys; no I/O |
| Hook installation | One-time, before game fully starts |
| Trampoline/detour | Verified correct prolog, instruction-aligned, no relative branches |

---

## 8. Recommended Fix (pending operator confirmation of hypothesis)

### 8.1 Immediate: Guard OP215 Initialize with streaming lock

```cpp
// Wrap RegisterVehicle's CStreamingInfo writes with CStreaming's own lock.
// GTA SA uses CStreaming::ms_bLoadingBigModel and a critical section at
// 0x8E4C58 (CStreaming::ms_csStreamingLock).
//
// Before writing:
//   EnterCriticalSection(&CStreaming::ms_csStreamingLock);
// After writing:
//   LeaveCriticalSection(&CStreaming::ms_csStreamingLock);
```

Or, simpler and safer: **defer Initialize() until the streaming queue is
idle** — check `CStreaming::ms_numModelsRequested == 0` before running
RegisterVehicle, and retry on the next Tick() frame if not idle.

### 8.2 Immediate: Guard model 614 mismatch

In `HookedCreate`, before calling `RequestLoad`:
```cpp
// Only request load if the model is actually registered in OP215.
if (skr::vehicle::IsInitialized()) {
    auto diag = skr::vehicle::GetDiag(customModel);
    if (diag.registered) {
        modelReady = skr::vehicle::IsLoaded(customModel);
        if (!modelReady) skr::vehicle::RequestLoad(customModel);
    }
    // else: model not in OP215 — keep proxy, don't request an unknown model
}
```

### 8.3 Defensive: Move DoSpawn off the main thread

The F9 synchronous load loop should be deferred to avoid any possibility
of an operator accidentally triggering a 5-second block during spawn.

---

## 9. Isolation Test Matrix

To confirm the hypothesis, the operator should run these tests:

| Test | OP215 | TEST_CERT | Expected Result | Purpose |
|------|-------|-----------|-----------------|---------|
| A: Control OFF | OFF | ON | No freeze | Baseline: confirms VFS/VEHICLES hook alone is safe |
| B: Control OFF | OFF | OFF | No freeze | Baseline: confirms stock client is safe |
| C: OP215 ON, no custom vehicles on server | ON | ON | No freeze | Tests OP215 init without hook swaps |
| D: OP215 ON, custom vehicles on server (612, 613 only) | ON | ON | May freeze | Tests the full path with registered models |
| E: OP215 ON, custom vehicles including 614 | ON | ON | May freeze | Tests unregistered model handling |

### Test A procedure (highest diagnostic value)
1. Build with `SKR_OP215=OFF`, `SKR_TEST_CERTIFICATION=ON`, `SKR_EXTERNAL_LOBBY=ON`.
2. Stage DLL to launcher/GTA paths.
3. Launch GTA, connect to server, spawn.
4. **If no freeze:** OP215 is the cause. Proceed to Test C/D.
5. **If freeze:** Something else is responsible. Check VFS/VEHICLES logs.

### Test D procedure (confirms H6)
1. Build with all three flags ON (current canonical build).
2. Ensure server creates vehicles with models 612 and 613.
3. Launch GTA, connect, spawn.
4. **If freeze on first spawn:** Confirms H6 — race between Initialize and streaming.
5. Collect `SKR\op215.log` and `SKR\skrx_vehicles.log` — timestamps will
   show whether Initialize overlaps with the first spawn.

---

## 10. Artifacts Preserved

No source files were modified during this investigation. All analysis was
read-only.

| File | Status |
|------|--------|
| `client/vehicle/custom_vehicle.cpp` | Read, not modified |
| `client/skrx/skrx_vehicles_client.cpp` | Read, not modified |
| `client/vfs/samp_vfs.cpp` | Read, not modified |
| `client/samp-hooks/samp_hooks.cpp` | Read, not modified |
| `client/game-state/game_state.cpp` | Read, not modified |

---

## 11. Next Steps

1. **Operator runs Test A** (OP215 OFF) to confirm or rule out OP215 as cause.
2. If OP215 confirmed:
   - Apply fix 8.1 (streaming lock or deferred init) to `custom_vehicle.cpp`.
   - Apply fix 8.2 (model registration check) to `skrx_vehicles_client.cpp`.
   - Rebuild, stage, re-test (Test D).
3. If OP215 NOT the cause:
   - Expand investigation to VEHICLES hook timing, SA-MP game state
     transitions, and CNetGame initialization sequence.
4. **Do not proceed to T03** until freeze is resolved.
