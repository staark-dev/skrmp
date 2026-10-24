# GPS notifications → C3 Native API toasts — integration plan

Status (2026-10-23): **BLOCKED — C3 NOT IN CURRENT TREE.** GPS notifications are
not visible in game yet. The GPS side is complete and waiting:

| Piece | Where | State |
|---|---|---|
| Event filter / rate limits | `client/gps/gps_notify.h/.cpp` (`Policy`) | in tree, unit-tested (SKRGpsPanelTests) |
| Subscription to GPS events | `notify::Install()` from `game_state.cpp` Tick | in tree, active (counts only) |
| C3 sink | `client/gps/gps_notify_c3.cpp` | written, **compiled against the real C3 headers** (build-gate `SKRGpsC3BridgeCheck`, `-DSKR_C3_TREE=<C3 worktree>`); not in the client build |
| Switch | CMake `SKR_GPS_C3_BRIDGE` (default OFF) | turn ON only after C3 is in the tree |

No second notification renderer exists or is planned: GPS notices go through
C3's `native_api::ui::PushLocalNotification` (renderer-only styles
`GpsNavigation`, `Success`, `Error`; no wire/Pawn change).

| GPS event | Notice | C3 style |
|---|---|---|
| Route started | "GPS route set · 3.3 km" (coalesced 4 s) | GpsNavigation |
| Rerouted | "GPS route recalculated" (≤ 1 per 20 s) | GpsNavigation |
| Arrived | "You have arrived at your destination" (dedupe 5 s) | Success |
| Unreachable / Failed | "GPS: <reason>" (≥ 10 s apart, same reason ≥ 30 s) | Error |
| Recalculating, Cancelled | silent | — |

Distance changes never produce notices; the panel / compact box carry distance.

## Branch facts (read-only inspection)

- `codex/c3-native-api-ui` = `main` (5006a96) + `053cd6c` "snapshot integrated local
  baseline for B.3.1" + `d2fc5ea` "feat(native-api): overhaul notifications and dialogs".
- `053cd6c` is a snapshot of an older working tree. It contains older GPS, pause and
  game-state files (`git diff --stat 053cd6c` against this working tree: 17 files,
  +125 / −1181 in client/gps + native-pause + game_state alone). **Merging the branch
  would pull that stale snapshot over the current uncommitted work.**
- `d2fc5ea` (the C3 work) touches only: `CMakeLists.txt`, `client/native-api/native_api_ui.{h,cpp}`,
  `native_api_ui_model.{h,cpp}`, `client/native-pause/native_pause.cpp`,
  `client/native-pause/pause_renderer.{h,cpp}`, `docs/C3_NATIVE_API_UI.md`,
  `tests/native_api_ui_preview.cpp`, `tests/unit/test_native_api_ui_model.cpp`.
- The C3 worktree still has uncommitted edits (`native_api_ui.cpp`, docs, preview) —
  Codex must commit them first.

## Simulation (2026-10-23 23:45, temporary index, nothing applied)

- `client/native-api/` is untracked on `main`: a real `git cherry-pick d2fc5ea` would abort
  (modify/delete + "untracked working tree files would be overwritten").
- Content check: `native_api_ui.cpp`, `native_api_ui.h`, `native_api.cpp` in this working tree are
  identical to C3's base `053cd6c` → C3's changes apply to them without conflict.
- `git merge-tree --write-tree --merge-base 053cd6c <working-tree snapshot> d2fc5ea`:
  `CMakeLists.txt` auto-merged; conflicts in `native_pause.cpp` (2 hunks: include, overlay
  condition), `pause_renderer.h` (1 comment hunk), `pause_renderer.cpp` (1 hunk: gameplay branch) —
  exactly the resolutions below.
- Codex's worktree holds ~100 uncommitted lines (wrapped toast text layout in `native_api_ui.cpp`,
  docs, preview) that are not in `d2fc5ea`.

Practical application (after approval): apply the C3 delta file-by-file as a 3-way merge
(base `053cd6c`, ours = working tree, theirs = C3 head incl. Codex's committed follow-up),
add the new `native_api_ui_model.*` and test files, resolve the 5 hunks as below, no git commit;
then `-DSKR_GPS_C3_BRIDGE=ON`, build, run tests.

## Intended strategy

1. Codex commits its pending C3 edits on `codex/c3-native-api-ui`.
2. Commit the current main working tree (GPS API-003.2 etc.) — owner's decision.
3. **Cherry-pick `d2fc5ea` (plus Codex's follow-up commit) onto main — do not merge
   the branch** (avoids `053cd6c`).
4. Resolve the two expected conflicts as below. `pause_renderer.h` and the
   `Render(IDirect3DDevice9*, bool drawPauseMenu)` signature are identical on both
   sides, so those hunks should apply cleanly.
5. Configure with `-DSKR_GPS_C3_BRIDGE=ON`, build, run SKRGpsPanelTests + C3's
   `SKRNativeApiUiModelTests`.

### Resolution: `client/native-pause/native_pause.cpp` (`OnEndScene`, end)

Both overlays share the one ImGui owner; the pause menu stays exclusive. Evaluate
both predicates (GPS `WantsRender()` also advances its panel model; do not let `||`
short-circuit it away):

```cpp
    // One existing ImGui owner serves every surface. Runtime API toasts and the
    // GPS navigation HUD render during gameplay; the pause menu is exclusive.
    const bool pauseOpen  = input::GetState() == input::PauseState::PauseOpen;
    const bool apiOverlay = skr::native_api::ui::WantsRender();
    const bool gpsOverlay = !pauseOpen && skr::gps::panel::WantsRender();
    if (pauseOpen || apiOverlay || gpsOverlay)
        renderer::Render(device, pauseOpen);
```
Includes: keep both `native-api/native_api_ui.h` and `gps/gps_panel.h`.

### Resolution: `client/native-pause/pause_renderer.cpp` (`Render`)

Keep this tree's cursor guard (set before `NewFrame`, cleared after
`RenderDrawData`), keep both includes, and draw both overlays in the gameplay branch:

```cpp
    if (drawPauseMenu) {
        RenderPauseMenu();                       // queues CFont text entries
    } else {
        // Gameplay overlays: draw-list commands only. C3 owns toasts/confirm,
        // GPS owns the minimap panel / compact box; they never overlap
        // (toasts bottom-right, confirm centred, GPS on/above the radar).
        skr::native_api::ui::DrawImGui(g_fontHeading, g_fontBody, settings::GetUiScale());
        skr::gps::panel::DrawImGui(g_fontHeading, g_fontBody, settings::GetUiScale());
    }
    ...
    if (drawPauseMenu) FlushTextQueue();
```
GPS draws only on frames where GTA drew the radar (`RadarFrame.sequence`), so a
`Render` call made for C3 alone never makes the GPS widgets blink or double-draw.

Check after merge: C3's confirm dialog takes mouse clicks without changing the
OS cursor, so `ImGuiConfigFlags_NoMouseCursorChange` during gameplay frames should
not affect it — verify by clicking a confirm dialog in game.

### Known C3 behaviour that affects GPS notices

`native_api.cpp` (C3) calls `ui::Tick` only while the Native API session is
`Ready`. A toast pushed outside that state would never age out, so the GPS sink
drops notices unless `native_api::IsReady()`. Consequence: **GPS toasts appear only
on SKR servers that completed the Native API handshake.** Showing them elsewhere
needs C3 to tick its UI independently of the session — a C3 change for its owner,
not done here.

## Not done (by design)

- No merge, cherry-pick, commit or edit in the C3 worktree.
- No copy of C3 code into this tree.
- Visual notification integration is **not** complete until the steps above run
  and the toasts are seen in GTA.
