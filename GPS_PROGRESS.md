# GPS Navigation System — API-002.9 + API-003.0

Status: **First runtime test crashed (2026-10-23 20:20). Fix built + unit-tested
in API-003.0.3; all GPS features ship DISABLED pending staged runtime testing.**

## Architecture

| Module | File | Responsibility |
|---|---|---|
| GPS Types | `client/gps/gps_types.h` | Shared types: Vec3, NavStatus, RouteNode |
| GPS Destination | `client/gps/gps_destination.h/.cpp` | Waypoint tracking, arrival detection |
| GPS Route | `client/gps/gps_route.h/.cpp` | CPathFind pathfinding, route cache, navigation state |
| GPS Engine | `client/gps/gps_engine.h/.cpp` | Integration layer: per-frame tick, deviation, recalc |
| GPS Renderer | `client/gps/gps_renderer.h/.cpp` | D3D9 minimap route line rendering |

## Verified GTA SA 1.0 US APIs

| Symbol | Address | Calling Conv | Purpose |
|---|---|---|---|
| `CPathFind::DoPathSearch` | `0x4515D0` | `__thiscall` | Road-network A* search |
| `CPathFind::GetPathNode` | `0x420AC0` | `__thiscall` | Resolve CNodeAddress → CPathNode* |
| `CPathFind::SetPathsNeededAtPosition` | `0x44DCD0` | `__thiscall` | Stream path data for position |
| `CPathNode::GetNodeCoors` | `0x420A10` | `__thiscall` | Compressed → world coords |
| `ThePaths` (CPathFind global) | `0x96F050` | — | Global pathfind instance |
| `CRadar::ms_RadarTrace` | `0xBA86F0` | — | Blip array (175 × 0x28 bytes) |
| `RADAR_SPRITE_WAYPOINT` | `41` | — | Player map marker sprite ID |
| `CRadar::TransformRealWorldPointToRadarSpace` | `0x583530` | `__cdecl` | World XY → radar [-1,1] |
| `CRadar::TransformRadarPointToScreenSpace` | `0x583480` | `__cdecl` | Radar [-1,1] → screen pixels |
| IDirect3DDevice9* (RenderWare) | `0xC97C28` | — | D3D9 device pointer |

## Integration Points

- `game_state.cpp` → `#include "gps/gps_engine.h"`
- `Tick()` calls `skr::gps::Tick(pos.x, pos.y, pos.z)` after ReadPlayer succeeds
- `TickNegotiation()` calls `skr::gps::Reset()` on disconnect

## Features Implemented

- [x] Map waypoint detection (CRadar::ms_RadarTrace scan)
- [x] Destination set/clear/has with position deduplication
- [x] Configurable arrival radius (default 15.0 world units)
- [x] CPathFind::DoPathSearch vehicle route calculation
- [x] Path streaming ensured (SetPathsNeededAtPosition) before search
- [x] SEH-guarded pathfinding (no crash on bad nodes)
- [x] Route node resolution via GetPathNode + GetNodeCoors
- [x] Cached route with ordered RouteNode geometry
- [x] Remaining distance calculation (sum of segments)
- [x] Navigation progress tracking (node checkpoint advancement)
- [x] Deviation detection (point-to-segment distance, 40m threshold)
- [x] Bounded route recalculation (max 10 recalcs per destination, 2s cooldown)
- [x] Rate-limited waypoint polling (200ms), deviation check (500ms)
- [x] Read-only RouteSnapshot for future renderers
- [x] Full reset on disconnect
- [x] No straight-line fallback when pathfinding fails

## Safety

- All GTA memory access is SEH-guarded
- No worker threads — all game-state access on GTA thread
- Rate-limited checks prevent per-frame overhead
- Bounded recalculation prevents infinite loops
- No modification of GTA vehicle AI or navigation behavior
- GPS operates client-side only — no server dependency
- Graceful failure: NavStatus::Unreachable / Failed when pathfind fails

## Build Results (2026-10-23)

- **Client DLL:** `build-test-certification-win32/Release/SKR.Client.dll` — 0 errors, 0 warnings
- **Gate tests:** `SKRNativeApiTests.exe` — 70/70 pass (no regression)
- **GPS tests:** `SKRGpsTests.exe` — 23/23 pass

## Known Limitations

1. **No minimap rendering** — route geometry is computed but not drawn (deferred to API-003.0)
2. **Path streaming latency** — SetPathsNeededAtPosition is called synchronously; if path data is not yet streamed, DoPathSearch may return partial/no results on the first attempt
3. ~~64-node route limit~~ — resolved in API-003.0.4 (road graph, no cap; native truncation detected)
4. **Z-coordinate from waypoint** — GTA map waypoint Z is always 0.0; DoPathSearch uses 2D proximity to find nearest road node, so elevation is approximate
5. **No pedestrian navigation** — pathType 0 (vehicle) only

## API-003.0 — Route Renderer (D3D9)

| Feature | Status |
|---|---|
| World → radar → screen coordinate transform chain | Implemented |
| Triangle-strip thick-line rendering | Implemented |
| Dark outline (5.5px half-width) + lime fill (3.5px half-width) | Implemented |
| D3D9 render state save/restore | Implemented |
| Scissor clipping for Round mode (bounding-box) | Implemented |
| Square/Custom mode uses existing HookedDrawRadar scissor | Implemented |
| SEH-guarded transform calls | Implemented |
| Active-segment rendering (from currentIndex onward) | Implemented |
| Rate-limited diagnostic logging via OutputDebugString | Implemented |
| ResetRenderer() on disconnect | Implemented |
| Integration: called from HookedDrawRadar after trampoline | Implemented |

### Render Pipeline

```
HookedDrawRadar()
  ├── Scale radar geometry
  ├── Set scissor (Square/Custom)
  ├── Call GTA trampoline (draws map tiles + blips)
  ├── Measure radar rect via TransformRadarPointToScreenSpace
  ├── OnRadarDrawn() → compass, vitals
  ├── RenderRoute() → GPS line  ← NEW
  └── Restore scissor + scale
```

### Colors

- Route line: `0xCC77FF44` (ARGB) — lime green, 80% opacity
- Outline: `0xCC0A1A0A` (ARGB) — dark, 80% opacity

## API-003.0.1 — GPS Test Enablement (2026-10-23)

Status: **IMPLEMENTED + COMPILED. NOT runtime-tested in GTA.**

### ESC MAP waypoints (`client/native-pause/pause_map.h/.cpp`, `pause_renderer.cpp` RenderTabMap)
- Right-click on map → `map::SetWaypoint()`; right-click within 25 px (scaled) of the
  waypoint → `map::ClearWaypoint()`. `CLEAR WAYPOINT` button appears in the control strip
  while a waypoint exists. Right-clicks over the control strip are ignored.
- Screen→world: `map::ScreenToWorld()` = exact inverse of `WorldToScreen` (same cx/cy/zoom).
- Pulsing red destination marker drawn over tiles; legend gets "Right-click: waypoint".
- GPS engine unchanged: it picks up the blip via its existing `ms_RadarTrace` scan.

Native APIs (verified against plugin-sdk `game_sa/CRadar.h/.cpp`, `CMenuManager.h`):

| Symbol | Value |
|---|---|
| `CRadar::SetCoordBlip(type, CVector, uint, display, char*)` cdecl | `0x583820` |
| `CRadar::SetBlipSprite(handle, sprite)` cdecl | `0x583D70` |
| `CRadar::ClearBlip(handle)` cdecl | `0x587CE0` |
| `CMenuManager::m_nTargetBlipIndex` | `0xBA6748 + 0x2C` |
| `BLIP_COORD` / `BLIP_DISPLAY_BLIP_ONLY` | `4` / `2` |
| blip handle | `arrayIndex | (m_nCounter@0x14 << 16)` |

No direct writes to `ms_RadarTrace`; the only raw write is GTA's own `m_nTargetBlipIndex` slot.

### `/gpscar` (`server-runtime-branded-test/gamemodes/skr_runtime_test.pwn` only)
Sultan (560) 4 units ahead, interior/VW linked, player put in as driver; requires spawned
state; one vehicle per player (replaced on reuse, destroyed on disconnect).

### Builds
- `SKR.Client.dll` (build-test-certification-win32, Release): 0 errors, 0 warnings.
- `SKRGpsTests.exe`: 23/23 (no regression).
- `skr_runtime_test.amx`: pawncc 3.10.10, 0 errors / 0 warnings; deployed to test runtime.

### Known issue (pre-existing, not changed)
`RenderTabMap` blip loop reads the sprite at `trace+0x22`; the SDK puts `m_nRadarSprite` at
`0x24`. Effect: blips with zero colour get the wrong fallback colour. The new waypoint
marker does not depend on it.

## API-003.0.3 — Crash & D3D9 State Stabilization (2026-10-23)

### Evidence
Source: `<gta>\SKR\crash_trace.log` (SKR's own handler; SA-MP's dialog swallowed the
exception, so the Windows event log has no record of this run).
- `0x6398C040` = **`nvd3dum.dll`+0x44C040** (NVIDIA user-mode D3D9 driver), read AV.
  `samp.dll` base `0x044C0000` matches the reported base. Reproduced twice (2 runs).
- Stack (heuristic scan): driver ← d3d9 ← `gta_sa+0x3FBCED` (RW D3D9 Im2D draw) ←
  `0x727B60` `CSprite2d::DrawRect` ← `SKR.Client+0x35571` (fn `0x10035510`, `mov edi,727B60h`)
  ← `+0x34360` ← `+0x32172` = return from `radar_hud::OnRadarDrawn` inside HookedDrawRadar
  (`+0x3216B` calls the radar trampoline, `+0x3216D` OnRadarDrawn, then RenderRoute).
- DLL that ran: `build-test-certification-win32` 20:15:44 (SHA-256 prefix E8562C47) —
  inferred from the launcher location; no log records the injected path. No PDB (Release).

### Root cause (source-verified mechanism, runtime-consistent)
`gps_renderer.cpp` called `IDirect3DDevice9::SetFVF` and `DrawPrimitiveUP` directly and
restored state by hand. RenderWare caches FVF / vertex declaration / shaders / stream
source and skips redundant sets (plugin-sdk `rw/rwcore.h` `_rwD3D9SetFVFMacro`,
`_rwD3D9SetVertexDeclarationMacro`); its own UP wrapper resets its stream cache first
(`_rwD3D9DrawPrimitiveUPMacro`). After our draw:
1. stream 0 was unbound (DrawPrimitiveUP semantics) but RW's cache still held its VB;
2. `SetFVF(XYZRHW|DIFFUSE)` replaced RW's vertex declaration; `SetFVF(oldFvf)` with
   oldFvf = 0 did not bring the declaration back, while RW's cache said it was current.
RW then drew with no stream / the wrong vertex layout → wrong vertex colours (the colour
corruption) and out-of-bounds vertex reads in the driver (the AV). The first victim is
the next RW Im2D draw — radar_hud's DrawRect on the following frame.

Why ESC MAP looked normal: ImGui draws inside a `D3DSBT_ALL` state block (pause_renderer),
and the GPS route only draws once status = Navigating, i.e. after a waypoint exists and
gameplay resumes. Absence of the route on ESC MAP is expected: API-003.0 draws on the
minimap only.

Ruled out by inspection: CRadar waypoint calls (addresses/enums match plugin-sdk);
CPathFind `DoPathSearch`/`SetPathsNeededAtPosition`/`GetPathNode`/`GetNodeCoors`
(addresses, `__thiscall`, parameter order match SDK `CPathFind.h/.cpp`); none appear in
the crash stack. Not ruled out at runtime — confirm with the staged test below.

### Fix
- `client/gps/gps_draw.h/.cpp` (new, pure D3D9): `DrawRouteStrip()` captures a
  `D3DSBT_ALL` state block before the first change and `Apply()`s it before return on every
  path after creation; if creation fails nothing is drawn. Sets VS/PS = null, FVF, stage 0/1
  ops, blend/z/fog/stencil/alpha-test/cull, optional scissor. The project's other overlays
  (lobby, intro, launch cover, FPS engine, ESC menu) already use this pattern.
- `gps_renderer.cpp`: hand-written save/restore and line builder removed; calls
  `DrawRouteStrip`; logs failures (no silent SEH).
- Radar geometry, scissor in HookedDrawRadar, radar_hud, game_state.cpp: **unchanged**.

### Feature switches (`client/gps/gps_flags.h/.cpp`, new)
`%LOCALAPPDATA%\SKR\gps_debug.ini`, `[GPS]` `Core` / `Renderer` / `Waypoints`, all **0 by
default** (template written on first run). Re-read ≤1×/s when the file timestamp changes,
so stages can be switched while the game runs. Core off → `gps::Reset()` + `ResetRenderer()`;
Renderer off → `ResetRenderer()`. Guards: top of `gps::Tick`, top of `RenderRoute`,
ESC MAP right-click / marker / CLEAR WAYPOINT / legend hint (MAP is pre-API-003.0.1 when off).
Log: `<gta>\SKR\gps.log` (flag changes, route results, render diagnostics every 5 s).

### Builds and tests (2026-10-23)
- `SKR.Client.dll` Win32 (machine 0x14C) Release, build-test-certification-win32: 0 errors, 0 warnings.
- New `SKRGpsRenderStateTests` (build-gate, real NVIDIA HAL device): 20/20 — state identical
  after clipped, unclipped, 200 repeated and rejected draws; flag transition rules incl. 50
  toggles; and confirms the mechanism (raw UP unbinds stream 0, raw SetFVF replaces decl).
- Regression: SKRGpsTests 23/23, SKRNativeApiTests 70, SKRRadarTransitionTests 223,
  SKRClientPolicyTests pass, SKRRejectionTests 40, SKREnhancedTests 588301 — all 0 failures.
- **Runtime: NOT verified.** Nothing deployed.

### Residual risks (not changed, out of scope)
`render-d3d9/chat_box.cpp` and `render-d3d9/d3d9_hook.cpp` also call SetFVF/DrawPrimitiveUP
with manual restore. They predate GPS and were not implicated, but follow the same unsafe
pattern if they ever run inside RW's scene.

## API-003.0.4 — Long-Distance Routing + Arrival Cleanup (2026-10-23)

Status: **IMPLEMENTED + COMPILED + UNIT-TESTED (incl. real GTA road data). NOT runtime-tested in GTA.**

### Cause of `status=4, nodes=0` (verified by disassembling GTA 1.0 US, ts 0x427101CA,
`D:\SA-MP Reverse\gta_sa.bak.exe`; the Steam exe has SecuROM jump stubs, bodies relocated to 0x15xxxxx)
1. **Our cost cap:** `DoPathSearch` 0x4515D0 stops when its distance counter passes argument
   `+0x38`; we passed 750 → any route longer than ~750 road units fails (0x45199B).
2. **GTA's visit cap:** search fails after touching **4950** nodes (`cmp edx,1356h` at 0x4519AE;
   array 0x972CD0). Intercity searches exceed this regardless of argument 1.
3. **Streaming:** `SetPathsNeededAtPosition` 0x44DCD0 (body 0x156DE80) only stores ONE position
   (0x977B70) + flag 0x96F030 — nothing loads synchronously; our origin call was overwritten
   by the target call. `UpdateStreaming` 0x450A60 keeps areas within 350 of the player, 300 of the
   needed position, 300 of some vehicles, plus a script rectangle (CPathFind+0x3C6C..0x3C7C, set by
   0x450D70, cleared by 0x44DD00; plugin-sdk mislabels it "ForbiddenForScriptedCars"); every other
   area is unloaded. Areas: 8x8 of 750, index = floor((y+3000)/750)*8 + floor((x+3000)/750).
4. **Endpoint resolution:** endpoints go through `FindNodeClosestToCoors` 0x44F460 with radius =
   `maxSearchDistance` (we pass 999999) → nearest *loaded* node, possibly far from the destination.
   Different flood fill (+0x17) → count 0, distance 100000.
5. **Silent truncation:** results are written origin-first and cut at `maxNodesToFind` (our 64).

### Design
- `client/gps/gps_graph.h/.cpp` (new): parses `models\gta3.img` nodes0..63.dat (the data GTA
  loads) on a one-time background thread (file I/O only), validates structure (header sizes,
  node area/id, link ranges, positions inside their area), resolves links globally, A* with
  3D-distance cost + admissible heuristic over routable nodes (vehicle, non-water, linked).
  Endpoint snap: origin 300 (3D), waypoint 300 (2D, waypoint z = 0). Flood-fill pre-check.
- `gps_route.cpp`: `CalculateRoute` → `RouteOutcome` {Found, Pending, Unreachable, Failed}.
  Local (≤500 straight): native DoPathSearch first (call unchanged), accepted only if 0<count<64
  and both ends within 100 of origin/destination; otherwise the road graph. Long: road graph.
  No node cap. `RouteSnapshot.source` = Native/Graph. Logs every decision to `SKR\gps.log`.
- Progress: `UpdateNavigation` also jumps to the nearest upcoming segment (40 lookahead, ≤25
  units) so a skipped node cannot stall a long route and exhaust the 10-recalc budget.
- `gps_engine.cpp`: graph load starts when Core is enabled; Pending → Calculating, retried every
  250 ms, Failed after 20 s. Status meanings: 0 Idle, 1 Calculating (graph loading), 2 Navigating
  (complete route), 3 Arrived (unused), 4 Unreachable (no road near an endpoint / not connected),
  5 Failed (road data unavailable or timeout).

### Arrival cleanup
Arrival = within 15 of the waypoint (unchanged) **or** within 20 of the route's last road node
(off-road waypoints are snapped up to 300 away, so the old rule alone could never fire).
On arrival: if the active waypoint is still the navigated one, `ClearMapWaypoint()` (new, in
gps_destination; CRadar::ClearBlip 0x587CE0 via m_nTargetBlipIndex, then any sprite-41 blip) →
Idle. A different waypoint is never touched. If GTA refuses the clear, that waypoint is
suppressed until moved/removed (no navigate/arrive loop). Pathfinding failure never clears.
ESC MAP's clear button now calls the same function (no duplicate CRadar code).

### Files
New: `client/gps/gps_graph.h/.cpp`, `tests/unit/test_gps_routing.cpp`.
Modified: `client/gps/gps_route.h/.cpp`, `gps_engine.h/.cpp`, `gps_destination.h/.cpp`,
`gps_flags.h/.cpp` (test override), `client/native-pause/pause_map.cpp` (delegates clear),
`CMakeLists.txt`, `server/skr-gate-omp/CMakeLists.txt` (test target only).
Unchanged: gps_renderer/gps_draw, radar, radar_hud, game_state, ESC MAP layout, Gate, Pawn.

### Results (2026-10-23)
Real data (Steam gta3.img): 68237 nodes / 30587 vehicle / 143622 links, 0 unresolved links,
0 flood-fill mismatches across links.

| Route | nodes | road len | straight | expanded | time |
|---|---|---|---|---|---|
| LV Strip ↔ LS Grove St | 262 | 4053 | 3217 | 2729–3450 | ≤0.5 ms |
| LV Strip ↔ SF Doherty | 266 | 5793 | 4287 | 5213–6097 | ≤0.6 ms |
| LS Grove St ↔ SF Doherty | 540 | 6872 | 4878 | ~10600 | ≤0.9 ms |

Every step of every route verified as a real link. Ocean waypoint → Unreachable, empty route.
- `SKR.Client.dll` Win32 Release: 0 errors, 0 warnings.
- `SKRGpsRoutingTests` 56/56 (synthetic A*, real data, planner, progress, arrival incl. refused clear).
- Regression: GpsRenderState 20, Gps 23, NativeApi 70, RadarTransitions 223, ClientPolicy,
  Rejection 40, Enhanced 588301 — 0 failures.

### Limitations
- Not runtime-verified in GTA. Graph route positions are from nodes*.dat; equality with live
  `ThePaths` memory is assumed (same files), not checked at runtime.
- Modded/replaced nodes*.dat are used as-is (validation rejects malformed files → Failed).
- Directionality: links are treated as two-way, like the native call (`oneSideOnly=false`).
- Waypoint further than 300 units from any road → Unreachable.
- Renderer still transforms every route node per frame (up to ~540 for LS↔SF).

## ESC MAP route line (2026-10-23)

Status: **IMPLEMENTED + COMPILED + UNIT-TESTED. NOT runtime-tested in GTA.**

- `pause_renderer.cpp`: `DrawMapRoute()` draws the remaining route (from `currentIndex - 1`)
  as an ImGui polyline (dark 7 px outline, lime 4 px, minimap colours) over the tiles and under
  the waypoint marker. Only when `Waypoints=1` and status = Navigating. Separate function because
  RenderTabMap uses `__try` (C2712). Legend/layout unchanged. ImGui draws inside the ESC menu's
  D3DSBT_ALL state block, so no RenderWare state risk.
- Why the route needed a sync from the map: the GPS tick is hooked on CGame::Process call sites
  `0x53BF4E` (runs while paused — before the `CTimer::m_UserPause` check at 0x53BF82) or
  `0x53C0BC` (skipped while paused, behind `jne 0x53C234`). Which one is installed is logged at
  startup but the logs from that run were gone. `gps::SyncWaypoint()` (new; same waypoint check
  as Tick plus pending retry, no arrival/deviation) is called by the MAP tab every frame, so a
  waypoint placed in ESC gets its route immediately either way.
- `pause_map`: `GetPlayerPosition(x, y, z)` (route search needs z).
- Tests: `SKRGpsRoutingTests` 59/59 (+3: SyncWaypoint builds/clears route without Tick, respects
  Core=0). Client 0 errors / 0 warnings. GpsRenderState 20, Gps 23 pass.

## Runtime verification — 2026-10-23 21:23 (user session, Core=1 Renderer=1 Waypoints=1)

Evidence: user screenshots + `<gta>\SKR\gps.log`, `game_state.log`, `native_pause.log`; no
`crash_trace.log` written.
- **Manually verified:** ESC MAP route line drawn (LV → LS intercity and LS local, zoomed and
  full map); waypoint set/clear from ESC MAP (native_pause.log handles 0x20002…0x50002).
- **Log-verified:** intercity graph routes LS→SF 273 nodes/5915, LS→LS 182/2323, 122/2297,
  124/2327, each 0.3–0.8 ms; route computed ~170 ms after the ESC MAP click; removal →
  "route invalidated"; progress 2327 → 527 m while driving (123 → 22 render pts);
  minimap renderer active the whole session with no crash and no reported colour corruption
  (API-003.0.3 state-block fix holds in this run).
- Tick hook installed at `0x53BF4E` (CGame::Process → 0x6997E0), which runs while paused; the
  ESC MAP `SyncWaypoint` call is therefore redundant on this setup but harmless.
- Observed: route ends at the road node nearest the waypoint (e.g. 1807,-645 snapped 90 units
  to the highway). This is by design — no straight-line connector to off-road waypoints.
- **Not yet verified:** arrival cleanup (session ended 527 m short), off-route recalculation,
  native local path (all routes this session were > 500 straight → graph).

## Session termination at ~120 s — NOT GPS (investigated 2026-10-23, no fix applied)

**Mechanism (log-verified):** the launcher kills GTA with `TerminateProcess(process, 1)` from
its handoff watchdog, `apps/launcher/lobby_window.cpp:286-288` (`TickLaunch`): `deadline =
launch-start + 120000` (line 189); fires when `sessionReady` was never observed.
Evidence: `build-test-certification-win32\Release\SKR\handoff.log` — `handoff-timeout` at
launch+120,078 ms (latest, launcher pid 47048), +120,032 ms (previous), +120,093 ms (earlier);
45 launches, 12 timeouts, **0 `session-visual-ready`**. TerminateProcess skips
DLL_PROCESS_DETACH → no `exit_trace.log`; no exception → no `crash_trace.log`, no WER.
Exit code 1. The launcher then shows "GTA sau interfața serverului nu a confirmat încărcarea".

**Why sessionReady is never signalled (source + client log):** the only setter is
`launch_cover::Presented()` (`client/intro/launch_cover.cpp:127`), reached from
`HookedPresent`. Present (vtable 17) is hooked only on the original device vtable
(`client/render-d3d9/d3d9_hook.cpp:284`); the one-shot re-chain after SA-MP init (lines
375-415) re-patches EndScene and Reset on SA-MP's new vtable but not Present. Client
`<gta>\SKR\handoff.log`: "hook Present" every 5 s until 340461812, vtable 05B61C5C →
043DD8F0 re-chained at 340467390, no Present trace afterwards; netGameReady came at
340462203, so the cover's completion frame never happens. CEF's alternative
(`SkrAfterPresentV1`) was not active ("CEF real-device Present owner" never logged).

Not location- or navigation-dependent: every session from this build living past 120 s is killed.

### Fix (2026-10-23) — IMPLEMENTED + COMPILED + UNIT-TESTED, NOT live-verified
- `client/render-d3d9/present_chain.h/.cpp` (new): Present hook on the original vtable
  (Primary) and on SA-MP's proxy vtable (Rechained); per-frame SKR work only at the outermost
  hook (thread-local guard) so proxy→real forwarding cannot recurse or double-run; never
  installs over an entry that is already ours; uninstall restores only entries still ours.
- `d3d9_hook.cpp`: Present body is the frame handler; initial install + re-chain use
  present_chain (re-chain now covers Present next to EndScene/Reset); traces install result,
  re-chain result and every 5 s `frames primary/rechained owner=cef|skr`. CEF real-device
  owner path unchanged: handler passes straight through when `g_cefPresentOwner`.
- `launch_cover.cpp`: completion frame presents the game (no cover flash); visual-ready is
  signalled once (`finished` guard) only after that frame's Present returned D3D_OK; a failed
  Present re-arms. `MarkGameplayConfirmed()` also signals a new launcher event (once).
- IPC `LaunchParams` v5: `gameplayConfirmedHandle` (launcher + client must match; v4 rejected).
- Launcher: `apps/launcher/launch_watchdog.h` (pure policy). Watchdog armed at launch (120 s),
  disarmed permanently by visual-ready **or** gameplay-confirmed; still terminates launches with
  neither. `handoff.log` now records `watchdog-armed`, `session-visual-ready after=`,
  `gameplay-confirmed after=`, `watchdog-disarmed reason=`, `watchdog-terminate reason=`, and
  `child-exit pid code after reason=` (watchdog-terminated / launch-cancelled / server-rejection /
  game-exited-after-session / game-exited-before-session).
- Build: SKR.Client.dll + SKRLauncher.exe (build-test-certification-win32, Release Win32):
  0 errors, 0 warnings. `SKRHandoffTests` 64/64 (fake-vtable chain scenarios, real HAL device
  with SA-MP-style takeover + 5 Resets/50 frames, watchdog incl. 200 s replay of the failing
  runs, IPC v5 round trip). Regression: LaunchIpc 18, Intro 98, NativeInit, Lobby 43,
  FpsD3D9, PauseControls, GpsRouting 59, GpsRenderState 20, Gps 23, NativeApi 70,
  RadarTransitions 223 — all pass.
- Not verified live: SA-MP 0.3.7 proxy behaviour, omp-cef owner path, gameplay > 180 s.

## API-003 FINAL — GPS completion + launcher readiness (2026-10-23)

Status: **IMPLEMENTED + COMPILED + UNIT-TESTED. Live GTA certification NOT performed.**

### A. Readiness protocol (IPC v6)
- `shared/ipc/launch_milestones.h` (new): CLIENT_INITIALIZED → SAMP_LOADED → SAMP_READY →
  GAMEPLAY_CONFIRMED / VISUAL_READY → SESSION_READY; `MilestoneTracker` (once per session,
  duplicates ignored, stale/zero sessions ignored, out-of-order flagged). SAMP_READY alone never
  counts as healthy.
- `LaunchParams` v6: `sessionId` + clientInitialized/sampLoaded/sampReady handles; the pipe ack
  now echoes the sessionId (writer rejects a mismatch). v5 peers are rejected.
- Client `client/intro/launch_signals.*` (new) owns every launcher event, signals once, traces
  `MILESTONE <name> session=… pid=… t=…`. Points: CLIENT_INITIALIZED at the end of SkrInitialize;
  SAMP_LOADED when D3D9 + SA-MP hooks are both installed (samp.dll build identified);
  SAMP_READY when CNetGame exists; GAMEPLAY_CONFIRMED from MarkGameplayConfirmed (CGame::Process,
  Present-independent); VISUAL_READY after the cover-completion frame was presented.
- Launcher: fresh event objects per launch (a previous GTA process can no longer signal the
  next launch), per-launch random sessionId, tracker-driven SESSION_READY, watchdog unchanged
  in policy (120 s, disarmed by visual-ready or gameplay), cancellation preserved.

### B. GPS
1. Routing: snapping to the nearest point on a road **link** (z-weighted for the player:
   bridges/overpasses) instead of the nearest node; destination re-snapped to the player's road
   network when the nearest road is unreachable; multi-source/multi-goal A*; routes start/end at
   the on-road points. Native local routes keep their nodes and are extended to the on-road ends
   only along a link touching their end node. `RouteFailure` reasons; origin failures
   (interior/off-map) retried every 3 s; off-route recalculation limited to 10 per rolling 60 s.
   Finding: random-sample snaps improve ≤ 15 units; the 90–192 unit snaps seen in logs were
   mostly genuinely off-road waypoints (now shown as a dotted off-road gap, never as road).
2. Minimap: polyline clipped in GTA radar space (unit circle round / unit square square-custom)
   before GTA's own radar→screen transform (rotation/range/custom size follow GTA); first point
   trimmed to the player's projection; mitred joins (≤ 2.5× half-width); one D3DSBT_ALL state
   block for all pieces; no file I/O on the render path (counters reported by the engine).
3. ESC MAP: navy underlay + lime dashes with continuous phase across vertices, dashes clipped to
   the map rect, dotted off-road connector road-end → waypoint. Widths via `Scale()`.
4. Adaptive zoom (`gps_zoom.*`, `AdaptiveZoom=0` default): wraps CHud::DrawRadar → CRadar::DrawMap
   at 0x58A38A (only if it still calls 0x586B00); replaces CRadar::m_radarRange (0xBA8314) after
   GTA computed it, only while navigating. Target 180→420 with speed (FindPlayerSpeed 0x56E090),
   zoom-in near a junction keeping it in view, clamps 160–460, 0.5 s low-pass + ±4 % backlash,
   0.9 s smoothing, 160 units/s rate limit, blends back to GTA's range then releases.
   Not installed at all unless enabled; removed on SkrShutdown.
5. Feedback (`gps_feedback.*`): events (route-started, rerouted, recalculating, arrived,
   cancelled, unreachable, failed) with listener interface for a future notification renderer
   (Codex C3 not integrated, not copied); summary (state, remaining, failure, next maneuver).
   Maneuvers only at real junctions (graph link count ≥ 3), ±25 m angles. HUD line above the
   minimap (`gps_hud.cpp`, CFont, Renderer=1).

### Files
New: `shared/ipc/launch_milestones.h`, `client/intro/launch_signals.h/.cpp`, `client/gps/gps_clip.h`,
`gps_feedback.h/.cpp`, `gps_zoom.h/.cpp`, `gps_hud.cpp`, `client/native-pause/map_route_geometry.h`,
`tests/unit/test_gps_navigation.cpp`.
Modified: `shared/ipc/launch_params.h`, `client/bootstrap/dllmain.cpp`, `client/intro/launch_cover.*`,
`apps/launcher/game_launcher.*`, `apps/launcher/lobby_window.cpp`, `client/gps/gps_graph.*`,
`gps_route.*`, `gps_engine.*`, `gps_renderer.*`, `gps_draw.*`, `gps_flags.*`,
`client/native-pause/pause_renderer.cpp` (DrawMapRoute only), `client/game-state/game_state.cpp`
(one call: `gps::DrawStatus()`), tests `test_handoff.cpp`, `test_gps_routing.cpp`, both CMakeLists.
Untouched: radar geometry/masks, radar_hud, Native API, CEF, present_chain logic, ESC layout.

### Automated results (2026-10-23 22:37)
SKR.Client.dll + SKRLauncher.exe Win32 Release: 0 errors, 0 warnings.
Handoff 85, LaunchIpc 18, GpsNavigation 439, GpsRouting 60, GpsRenderState 20, Gps 23,
Intro 98, NativeInit, Lobby 43, FpsD3D9, PauseControls, NativeApi 70, RadarTransitions 223,
ClientPolicy, Rejection 40, Enhanced 588301 — all pass.

### Live certification — `%LOCALAPPDATA%\SKR\gps_debug.ini`: Core=1 Renderer=1 Waypoints=1 AdaptiveZoom=1
Launcher log (`<launcher dir>\SKR\handoff.log`), in order:
`launch-started session=<id> ipc=v6`, `watchdog-armed timeout=120000ms`,
`milestone CLIENT_INITIALIZED … recorded`, `milestone SAMP_LOADED … recorded`,
`milestone SAMP_READY … recorded`, `milestone GAMEPLAY_CONFIRMED … recorded` and/or
`milestone VISUAL_READY … recorded`, `milestone SESSION_READY … recorded`,
`watchdog-disarmed reason=…`; on exit `child-exit … reason=game-exited-after-session`.
Must NOT appear: `watchdog-terminate`, `OUT OF ORDER`, `stale session`.
Client log (`<gta>\SKR\handoff.log`): `MILESTONE configured session=<same id>`, `MILESTONE <name>
… launcher=notified` ×5, `Present hook (primary vtable …): installed`,
`D3D9 re-chain: Present installed`, `hook Present frames … rechained=<rising>`.
GPS log (`<gta>\SKR\gps.log`): `route: graph found …` / `route: native … accepted`,
`event: route-started`, `event: recalculating` + `event: rerouted`, `event: cancelled`,
`event: arrived`, `core: arrived … waypoint cleared, Idle`, `zoom: call-site wrapper installed`,
`summary: navigating remaining=… next=…` every 30 s. Must NOT appear: `render: draw failures`.

### Limitations
- Live behaviour unverified: SA-MP proxy Present re-chain, zoom feel, CFont text scale/position
  (visual estimates), maneuver usefulness on real junction shapes, dash look at 1360×768/1080p.
- Graph routes ignore lane direction (as the native call with oneSideOnly=false).
- Milestones are polled by the launcher only once the launch worker returns, so launcher-side
  `after=` for CLIENT_INITIALIZED/SAMP_LOADED is observation time; client traces hold exact times.
- C3 branch was snapshotted from this tree; GPS/ESC files here have since diverged and will need
  a normal merge with codex/c3-native-api-ui (no files were taken from or written to that branch).

### Live certification run — 2026-10-23 22:42 (user, launcher pid 3804, GTA pid 33492, session 900b4f3d5c9a5b2c)
Verified from launcher `handoff.log`, client `handoff.log`, `gps.log`, 5 screenshots:
- **Readiness (PASS):** launcher recorded CLIENT_INITIALIZED, SAMP_LOADED (samp.dll 0.3.7-R5),
  SAMP_READY (12.0 s), VISUAL_READY (17.3 s), SESSION_READY (17.3 s), GAMEPLAY_CONFIRMED (21.2 s),
  same session id on both sides, no out-of-order/stale; `watchdog-disarmed reason=visual-ready`.
- **Present chain (PASS):** primary installed; `D3D9 re-chain: Present installed`; primary frames
  stop at 616 and rechained frames rise to 12532 (no double processing); VISUAL_READY arrived 31 ms
  after the re-chain — the exact path that failed before the fix.
- **Lifetime (PASS):** GTA alive and responding at 459 s (old kill at 120 s); 2 device Resets
  handled (`OnAfterReset … ok=1`), ESC open/close, no crash_trace / exit_trace.
- **GPS (PASS for this route):** zoom wrapper installed; graph route 190 nodes / 3554 m with 47 m
  off-road gap; summaries every 30 s with junction maneuvers (left@31, right@550, right@35);
  render frames 8416, pieces 1–3, no render failures; arrival → waypoint cleared → Idle; HUD lines
  "GPS 3.3 km / Turn right in 290 m", "Destination in 90 m", "GPS Arrived" visible; ESC MAP dashed
  route on roads.
- **Not covered by this run:** normal GTA exit + launcher `child-exit` (game still running when
  checked), waypoint cancellation, LS↔SF↔LV routes on this build (only on earlier builds),
  zoom feel (no range values logged).
- **Observed, minor:** (1) driving off-road toward an off-road waypoint triggers one off-route
  recalculation ~50 m before arrival (harmless: native 2-node reroute, arrival 110 ms later);
  (2) summary logs `next=none@0(unreliable)` when the next junction is beyond the 1.5 km horizon
  (cosmetic; HUD shows no turn line, correctly).

- **Exit (PASS):** launcher `child-exit pid=33492 code=0x00000000 after=537312ms
  reason=game-exited-after-session`, `lobby-restored`; both processes gone.

### Follow-up fixes (2026-10-23 22:53, after the live run) — compiled + unit-tested, not live-verified
- `gps_engine.cpp`: no off-route recalculation when the remaining route is ≤ 120 m or the player
  is within (off-road gap + 30 m) of the waypoint — leaving the road toward an off-road waypoint.
- `gps_feedback.cpp`: a junction beyond the 1.5 km horizon now yields `None` with
  `reliable = true` when road data is known (log no longer says "unreliable"); zoom ignores `None`.
- Tests: GpsNavigation 443 (+4: beyond-horizon label; off-road approach without recalculation
  using the live 1929,-509 waypoint; arrival at the off-road waypoint). All other suites pass.

## Next Steps

1. Live: cancellation mid-route, LS↔SF↔LV on the final build, zoom feel, off-road approach fix.
1. Staged runtime test (API-003.0.3): Baseline → Core → Waypoints → Renderer → full.
2. Runtime test: verify waypoint detection, route calculation, route rendering in GTA
2. Consider increasing kMaxRouteNodes or implementing multi-segment routes for long distances
3. Tune line widths and opacity based on in-game visual testing

## API-003.2 — Navigation panel + precision route strokes (2026-10-23 23:25)

Status: **IMPLEMENTED + COMPILED + UNIT-TESTED + offscreen-rendered. NOT verified in GTA.**
Checkpoint before edits: `_backups/api-003-2-pre/`.

### A. ImGui navigation panel above the minimap
- `client/gps/gps_panel_model.h` (new, pure): `ComputeLayout` (left edge + width = measured radar
  rect, gap clamp(5u, 4, 6) px, unit u = backbuffer height/768 × UI scale applied once, hidden when
  the panel would rise above 30 % of screen height = chat area), `AlignLeft/AlignRight` (measured
  size, vertical centre, pixel snap), `FitText` (UTF-8 safe "..." truncation, drops dangling
  separators), `DistanceDisplay` (10 m / 0.1 km steps, 0.35-step deadband, m↔km at 1000/950),
  `FormatLocation` ("Area · City", printable ASCII only), `Model` (Idle hidden; Navigating; Calculating/
  Recalculating amber with last distance dimmed; Arrived 3.5 s; Unavailable 6 s; 160 ms fades;
  reliable turn ≤ 250 m replaces the location row as "Turn right in 120 m").
- `client/gps/gps_panel.h/.cpp` (new): `WantsRender()` (Renderer=1, view visible, radar drawn since the
  panel's previous frame via `gps_renderer.h RadarFrame.sequence`), `DrawImGui()`, `DrawPanel()`.
- Rendering: the EXISTING ImGui owner. `native_pause::OnEndScene` → `renderer::Render(device, pauseOpen)`
  when the pause menu is open OR the panel wants a frame (same shape as codex/c3-native-api-ui).
  Gameplay frames: foreground draw list only, `ImGuiConfigFlags_NoMouseCursorChange` set before
  NewFrame, no CFont text queue flush; whole pass inside the existing D3DSBT_ALL state block. No new
  hook, context, device ownership or input path (ImGui gets Win32 messages only while paused).
- Location: `game_state.cpp` Tick calls `ResolveZones` (existing, SEH-guarded) ≤ 2×/s while navigation
  is not Idle → `panel::SetLocation`. Row stays empty until the first lookup.
- CFont line (`gps_hud.cpp DrawStatus`) now draws only while `native_pause::renderer::IsReady()` is
  false (fallback) — never together with the panel.

### B. Route strokes
- `gps_draw.cpp`: anti-aliased strokes (solid core + 1 px alpha fringe each side, triangle list),
  `MinimapRouteStyle(screenH)`: fill 2.0 px / outline 2.9 px at 768p, ×≤1.15 → 2.3 / 3.2 px at 1080p;
  colours #A3E65B @ 88 %, #102016 @ 55 %. Joins: miter while ≤ 2× half-width, else bevel (two pairs).
  Same state-block isolation as before; vertex arrays reused (no per-frame allocation).
- `gps_renderer.cpp`: passes the style from RsGlobal height; publishes `LastRadarFrame()`.
- ESC MAP: `client/native-pause/map_route_draw.h` (new, shared with the preview): dash 1.7 px
  (≤ 2.0) on a +1 px navy underlay @ 47 %, dash 7 / gap 5 px (×≤1.15), dots r 1.1; widths from
  backbuffer height, never map zoom. `BuildDashes(startPhase)`: phase = travelled route length × zoom,
  so dashes stay fixed to the road as progress advances. `DrawMapRoute` delegates to it.

Measured from rendered pixels (SKRGpsHudPreview, opaque white on black, Σ coverage):
| | before | after 1360×768 | after 1920×1080 |
|---|---|---|---|
| minimap route | 7.00 px | 2.00 (1.96 @30°) | 2.30 (2.22 @30°) |
| minimap outline | 11.00 px | 2.90 | 3.20 |
| ESC dash / underlay | 2.49/4.98 (768) · 3.50/7.00 (1080) | 1.70 / 2.70 | 1.95 / 2.95 |

### C. Notifications
`client/gps/gps_notify.h/.cpp` (new): `Policy` (route set coalesced 4 s; rerouted ≤ 1 per 20 s;
arrived deduped 5 s; failures ≥ 10 s apart, same failure ≥ 30 s; recalculating/cancelled silent),
subscribed to gps_feedback via `notify::Install()` (game tick, idempotent), delivered through a `Sink`.
**C3 renderer is not in this tree** (only on codex/c3-native-api-ui) → no sink installed; notices are
counted only. Merge point documented in gps_notify.h (PushLocalNotification(GpsNavigation/Success/Error)).
No Pawn/Gate protocol change.

### Tests / builds (2026-10-23 23:21)
- SKR.Client.dll Win32 (0x14C) Release, build-test-certification-win32: full solution 0 errors, 0 warnings.
  SHA-256 9967964e77110d422d5981dcdad205218db93d70589a8f511f726b0bc226bbf6. Launcher not affected.
- New `SKRGpsPanelTests` (build-gate) 108/108: anchoring (default 1360×768/1920×1080, narrow/wide custom,
  off-screen, too-high, UI scale once), alignment, truncation, distance hysteresis, lifecycle incl.
  arrival/cancel clearance, stroke widths + joins, dash phase continuity, notification policy + bus
  (12 events → 3 notices), source scan: no file/log/blocking calls in panel/draw/dash code.
- Regression: GpsNavigation 443, GpsRouting 60, GpsRenderState 20, Gps 23, NativeApi 70,
  RadarTransitions 223, ClientPolicy, Rejection 40, Enhanced 588301, PauseControls, Handoff 85,
  NativeInit, FpsD3D9, Intro 98, LaunchIpc 18 — all pass.

### Previews — `docs/gps-previews/api-003-2/` (SKRGpsHudPreview, real HAL device)
Production code: DrawRouteStrips/MinimapRouteStyle, ClipPolyline, DrawPanel/ComputeLayout/Model,
DrawRouteStrokes/StyleForScreen, real gta3.img routes. Emulated: radar tiles (assets/minimap/minimap.png
through CRadar-equivalent math), vitals strip, blips, chat placeholder, ESC frame, zone names
(zones.json + info.zon city rects), world scene (flat). `minimap.png` sits ≈4 world units off the road
nodes in central LS (79 % → 86 % hits at offset 0,−4), so routes can look slightly off-centre on that
backdrop; GTA draws its own tiles in game. Files: 1–8 + 7b_BEFORE at both resolutions, `_zoom3x` crops,
`widths.txt`.

### Not verified (needs GTA)
Panel position vs real HUD/chat, fonts at real resolutions, CFont fallback path, route weight on real
radar tiles, ESC dashes in the real map tab, cursor behaviour during gameplay frames, device Reset with
the gameplay ImGui pass, notifications (no sink yet).

## API-003.2 corrections — driver / on-foot HUD + C3 bridge (2026-10-23 23:40)

Status: **IMPLEMENTED + COMPILED + UNIT-TESTED + offscreen-rendered. NOT verified in GTA.
C3 notifications: BLOCKED — C3 NOT IN CURRENT TREE.**

### Context-aware HUD
- `client/gps/gps_display_policy.h` (new, pure): driver → Full, on foot / passenger → Compact,
  no navigation → Hidden; Full↔Compact applied only after 300 ms stable (enter/exit animation,
  odd samples); show-from-hidden and hide are immediate. `ModeOverride` (Auto/ForceFull/
  ForceCompact/ForceHidden) = hook for API-003.3 via `panel::SetDisplayOverride`.
- Seat (`game_state.cpp ReadSeat`, SEH-guarded, every game tick ≈100 ms), BINARY_CONFIRMED on
  `D:\SA-MP Reverse\gta_sa.bak.exe` with dumpbin 14.51: FindPlayerVehicle 0x56E0D0 tests
  `[ped+0x46C] & 0x100` (bInVehicle) and returns `[ped+0x58C]`; CVehicle::IsDriver 0x6D1C40
  compares `[veh+0x460]`. Passed with player XY to `panel::SetPlayerContext`.
- Compact box: straight-line 2D distance player → engine destination (`HasDestination/
  GetDestination`, pure engine state; waypoint z = 0), same distance hysteresis; centred near the
  radar's top edge, all corners inside the round (elliptical) radar, square radar 3 px inside the
  top; hidden if it would not fit. Shown on foot even when the road route is Unavailable; cleared
  on arrival/cancel. The full panel and the box never overlap (outgoing fades out first).
- Engine untouched: no recalculation or state change on seat changes.

### C3 notifications
- `client/gps/gps_notify_c3.cpp` (new): sink → `native_api::ui::PushLocalNotification`
  (GpsNavigation / Success / Error), only while `native_api::IsReady()` (C3 ticks toasts only in
  that state). Built only with CMake `SKR_GPS_C3_BRIDGE=ON` (default OFF); `notify::Install()`
  connects it. Compile-verified against the real C3 headers (build-gate `SKRGpsC3BridgeCheck`,
  configured once with `-DSKR_C3_TREE=<C3 worktree>`, then removed from the cache).
- Branch analysis + exact conflict resolution for `native_pause.cpp` / `pause_renderer::Render`:
  `docs/GPS_C3_INTEGRATION.md`. Do NOT merge the branch (its 053cd6c snapshot carries stale GPS/
  pause files); cherry-pick d2fc5ea after Codex commits its pending edits.

### Tests / builds (23:36)
- SKR.Client.dll Win32 (0x14C) Release, full solution 0 errors / 0 warnings,
  SHA-256 242bdc7cbc01704ca1295f2d2b1b53ba3c9adfc9ef635b177ccf1b1d46f29e08. Launcher unchanged.
- SKRGpsPanelTests 154/154 (108 previous + 46: policy, debounce/no-flicker, compact layout inside
  round/square radar, driving/walking/passenger/exit/arrival/cancel/no-route, overrides).
- Regression: GpsNavigation 443, GpsRouting 60, GpsRenderState 20, Gps 23, NativeApi 70,
  RadarTransitions 223, ClientPolicy, Rejection 40, Enhanced 588301, PauseControls, Handoff 85,
  NativeInit, FpsD3D9, Intro 98, LaunchIpc 18 — all pass.
- Previews `docs/gps-previews/api-003-2/`: 9 driving, 10 walking, 11 passenger, 12 walking on custom
  radar, 13a–d exit vehicle at 150/380/560/720 ms, 14 arrival driving, 15 arrival on foot (nothing
  drawn) — both resolutions, `_zoom3x` crops.

### Known limits
- CFont fallback (only when the ImGui renderer is unavailable) still shows the old driver-style line
  regardless of seat.
- On foot, leaving the road still triggers the engine's existing off-route recalculation (≤ 10 / 60 s),
  unchanged by design; the compact box does not depend on it.

## Live feedback round — typography, C3 integration analysis, API-003.3 checkpoint (2026-10-23 23:58)

### 1. Location text readability — BUILT + PREVIEWED, GTA re-test pending
`gps_panel_model.h`: row 2 font 12u → 13u (13 px at 768p, 18 px at 1080p), row 2 height 15u → 17u
(panel 40 → 42 px at 768p, 57 → 60 at 1080p). `gps_panel.cpp`: location row uses the heading face
already in the atlas (Bahnschrift, as `GPS ROUTE`) instead of Segoe UI, colour (184,196,208) →
off-white (226,232,238); measured + truncated with the same face. No new fonts, no per-frame
font work. Row 1 unchanged. Previews regenerated (`docs/gps-previews/api-003-2/`).

### 2. C3 — BLOCKED (approval + Codex commit required); simulation done, nothing applied
Simulated `cherry-pick d2fc5ea` onto the working tree with a temporary index
(`git read-tree HEAD` + add of the touched files into a scratch index, `commit-tree`,
`merge-tree --write-tree --merge-base 053cd6c`) — real index and files untouched:
- `client/native-api/` is **untracked** on main → a real `git cherry-pick` aborts (modify/delete +
  untracked overwrite). Content-wise `native_api_ui.cpp/.h` and `native_api.cpp` here are identical
  to C3's base, so C3 applies cleanly to them.
- `CMakeLists.txt` auto-merges. 3 conflicted files / 5 hunks: `native_pause.cpp` (include; overlay
  condition), `pause_renderer.h` (comment), `pause_renderer.cpp` (gameplay branch) — all covered by
  `docs/GPS_C3_INTEGRATION.md`.
- Codex has ~100 lines of uncommitted C3 work in its worktree (wrapped-text toast layout,
  `native_api_ui.cpp` 21:57) that `d2fc5ea` does not contain → must be committed first.

### 3. World checkpoint (API-003.3) — IMPLEMENTED + COMPILED + UNIT-TESTED, NOT GTA-tested
- `client/gps/gps_checkpoint_model.h` (pure): slot 0 = local waypoint at the exact engine destination
  (map waypoint, never the road-snapped end), slots 1..4 = server destinations; style (colour, size,
  draw distance 250, pulse, visible), arrival policy (Remove / KeepFor / KeepUntilCleared); cancel
  always removes; local+server at the same spot → one marker; advisory server entry queue; no
  placement until a ground height is known (map waypoints have z = 0).
- `client/gps/gps_checkpoint.cpp`: `Tick` (10 Hz, after gps::Tick) syncs local state, ground via
  `CWorld::FindGroundZFor3DCoord` 0x5696C0 (from z 1000, `found` flag, ≤ 2/s per marker, only within
  draw distance + 50); `PlaceFrame` every CGame::Process frame (existing game_state tick, before the
  100 ms gate) → `C3dMarkers::PlaceMarker` 0x725120, type 1 cylinder, normal 0, zCheck 0 (as GTA's
  own PlaceMarkerSet), SEH-guarded, no I/O/allocation. Gated by Renderer=1.
- BINARY_CONFIRMED (clean 1.0 US exe, dumpbin 14.51): 32 markers × 0xA0 at 0xC7DD58; Render 0x725040
  renders only markers placed since the last frame and frees the rest; PlaceMarker reuses only a slot
  with the same id that is not yet placed this frame, else a free slot (type word 0x101), and for
  type 1 returns null when full (types 0/3/5/6 would evict the farthest). Marker ids = addresses of a
  static array in SKR.Client.dll → cannot collide with GTA/SA-MP/script ids. samp.dll (SHA-256
  b72b5dbe…) holds no direct references to the C3dMarkers/CCheckpoints entry points (SA-MP drives
  its checkpoints via script opcodes; INFERRED, not traced).
- Pawn natives: client functions ready (`SetServerCheckpoint` / `RemoveServerCheckpoint` /
  `ClearServerCheckpoints` / `TakeServerArrival`, `panel::SetDisplayOverride`); wire + Gate + include
  drafted in `docs/protocol/GPS_API_003_3_DRAFT.md`, NOT implemented (Native API contract change).
- No offscreen preview: the marker is rendered by GTA itself.

### Builds / tests (23:55)
SKR.Client.dll Win32 (0x14C) Release, full solution 0 errors / 0 warnings, SHA-256
6886de3745cac1a4c94e07f47952957184e05b1e14e05ccac157e5c4bd508606; SKR_GPS_C3_BRIDGE=OFF. Launcher unchanged.
GpsPanel 154, GpsCheckpoint 44 (new), GpsNavigation 443, GpsRouting 60, GpsRenderState 20, Gps 23,
NativeApi 70, RadarTransitions 223, ClientPolicy, Rejection 40, Enhanced 588301, PauseControls,
Handoff 85, NativeInit, FpsD3D9, Intro 98, LaunchIpc 18 — all pass.
