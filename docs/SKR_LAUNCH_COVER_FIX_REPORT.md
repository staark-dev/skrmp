# SKR Enhanced v1.0.0 — Launch Cover & FPS Fix Report

**Date:** 2026-10-20
**Commits:** `04969d4` (checkpoint) → `88b9ad8` (fix)

---

## Bug 1: Launch Cover Never Disappears

### Root Cause

The launch cover (`launch_cover.cpp::Draw`) requires three conditions to set `completionPending = true` and dismiss:

1. **`worldConnected`** — `IsPlayerInGame()` checks CNetGame state == 5. **MET** (chat/scoreboard work).
2. **`visualReady`** — For SKR sessions (`IsSkrSession() == true`), computed as `skr_cef_session_visual_ready_v1()`. **NEVER MET.**
3. **`takeoverEvent`** — Signaled by the launcher after `frameEvent`. **MET** (logs show "cover-presented").

`skr_cef_session_visual_ready_v1()` (`deps/omp-cef/src/client/core/skr_local_api.cpp:64`) requires ALL of:
- CEF `runtime` exists
- `BrowserManager` exists
- `runtime->ResourcesReadyForHandoff()` returns true (resources download state == COMPLETED)
- HUD browser instance ID 101 exists with `first_paint_uploaded` flag set

When the server doesn't push CEF resources or create the HUD browser (ID 101), this function returns `false` forever. The lobby system (`OnGameplayConfirmed`) correctly detects gameplay is active and hides the lobby browser, but it **never tells the launch cover to finish** — these are two independent systems with no bridge.

### Fix

Added `MarkGameplayConfirmed()` to `launch_cover` — an `std::atomic<bool>` flag that acts as a fallback OR condition for `visualReady`:

```cpp
// Before (launch_cover.cpp):
const bool visualReady = skr::lobby::IsSkrSession() ? (ready && ready())
    : !skr::game_state::IsEnhancedNegotiationPending();

// After:
const bool visualReady = gameplayConfirmed.load(std::memory_order_relaxed)
    || (skr::lobby::IsSkrSession() ? (ready && ready())
        : !skr::game_state::IsEnhancedNegotiationPending());
```

Called from `lobby_manager.cpp::OnGameplayConfirmed()` — a game-state-driven event, not a blind timeout.

### Why This Is Correct

- `OnGameplayConfirmed` fires only after the lobby system has verified the player is in-world through proper game state checks (session mode + timeout/CEF page focus).
- The cover's normal completion flow (setting `finished`, signaling `readyEvent`, releasing texture via `BeforeReset`) still runs through `Presented()` — no bypass of cleanup.
- The `gameplayConfirmed` flag is one-shot (never reset), matching the launch cover's design as a one-time splash.
- Thread safety: `std::atomic<bool>` with relaxed ordering is sufficient since the flag is only written once and read in the same render-thread tick loop.

### Files Changed

| File | Change |
|---|---|
| `client/intro/launch_cover.h` | +4 lines: `MarkGameplayConfirmed()` declaration |
| `client/intro/launch_cover.cpp` | +5 lines: `<atomic>` include, `gameplayConfirmed` flag, modified `visualReady`, `MarkGameplayConfirmed()` body |
| `client/lobby/lobby_manager.cpp` | +3 lines: `launch_cover.h` include, `MarkGameplayConfirmed()` call in `OnGameplayConfirmed()` |

---

## Bug 2: FPS Counter Invisible

### Root Cause

Direct consequence of Bug 1. FPS rendering is gated on the cover not being drawn:

```cpp
// d3d9_hook.cpp — CEF path (SkrBeforePresentV1):
if (!g_cefDrewCover && skr::intro::IsFinished()) fps::Draw(device);

// d3d9_hook.cpp — Non-CEF path (HookedPresent, SKR_EXTERNAL_LOBBY):
if (!drawn && skr::intro::IsFinished()) skr::render::fps::Draw(device);
```

While the cover persists, `g_cefDrewCover` / `drawn` is always `true`, so `fps::Draw()` is never called.

### Fix

No additional changes needed. Once the cover dismisses (Bug 1 fix), `launch_cover::Draw()` returns `false`, setting `g_cefDrewCover = false`, and `fps::Draw()` executes normally.

---

## Bug 3: Standard/Enhanced Negotiation Validation

### Analysis (4 scenarios)

| Client | Server | visualReady Path | Cover Dismissal | Status |
|---|---|---|---|---|
| SA-MP standard | open.mp standard | `!IsEnhancedNegotiationPending()` → true (enhancedAdvertised=false) | Normal | OK |
| SKR Enhanced | open.mp standard | Negotiation timeout (10s) → Failed → `!Pending()` = true; also `OnGameplayConfirmed` fires after 1.5s grace | Normal + gameplayConfirmed | OK |
| SA-MP standard | SKR Hybrid | No SKR.Client.dll loaded; standard connection | N/A | OK |
| SKR Enhanced | SKR Hybrid | Handshake completes → Enhanced; RPC 241 → IsSkrSession; `OnGameplayConfirmed` → gameplayConfirmed=true | gameplayConfirmed fallback | OK |

The fix does not modify the negotiation protocol (RPC 240/241). Standard clients are not affected.

---

## Test Status

| Criterion | Status |
|---|---|
| "Preparing your session..." dispare după intrarea în joc | **UNVERIFIED** — requires GTA runtime |
| Scena GTA vizibilă | **UNVERIFIED** — requires GTA runtime |
| Contor FPS vizibil și funcțional | **UNVERIFIED** — requires GTA runtime |
| HUD CEF și radar funcționează | **UNVERIFIED** — requires GTA runtime |
| Chat și scoreboard SA-MP | **UNVERIFIED** — requires GTA runtime |
| Alt+Tab fără regresii | **UNVERIFIED** — requires GTA runtime |
| D3D9 Reset corect | Not modified — existing BeforeReset() flow preserved |
| Clienți standard nu sunt respinși | OK — protocol unchanged |
| Negociere Enhanced funcționează | OK — code path analysis verified |
| Reconectare/deconectare | gameplayConfirmed is one-shot; launch cover is also one-shot (no reconnect reset needed) |

**Build required:** Open `build-external-lobby/SKR.Client.sln` (or run CMake + MSBuild) to produce `SKR.Client.dll`, then install in `GTA San Andreas\SKR\` for testing.
