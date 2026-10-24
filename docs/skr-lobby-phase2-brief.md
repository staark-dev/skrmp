# SKR Lobby — Phase 2 brief

Target audience: a fresh agent (or human engineer) picking up Phase 2 of the
in-game SKR lobby after Phase 1 lands. This document carries every piece of
context Phase 2 needs so no earlier conversation has to be replayed.

Branch snapshot this brief describes: `skr-lobby-phase1` at
commit `9953029` (Phase 1 bug-fix commit). All paths are relative to that
checkout.

---

## 1. Project rules you must respect (AGENTS.md excerpt)

- GTA module is x86 only; separate ABI for server/helpers.
- No CEF / graphics / network / threads / wait in `DllMain`.
- No secrets in code, CLI, logs, temp files.
- Every protected action is authorized server-side; client hashes/heartbeats
  are NOT attestation.
- Mock SKRAC is dev-only, rejected in production.
- Do not disable TLS, sandbox, or signatures to pass tests.
- Do not redistribute GTA, non-public sources, or dependencies with
  unverified licence.
- Report changed files, validation, risks and real blockers.
- Do not mark R2 compatible without a hash matrix.
- Do not publish or sign with real keys without owner authorization.

The project owner is Romanian-speaking; commit messages, UI copy, and
handover notes are in Romanian or English — follow the surrounding style.

---

## 2. What exists after Phase 1

### 2.1 Launch flow (today)

1. `apps/launcher/` (Win32 launcher) spawns `gta_sa.exe` suspended, injects
   `SKR.Client.dll` via `CreateRemoteThread(LoadLibraryW)`, then resumes.
   WPF variant lives in `apps/launcher-ui/` (parallel session —
   `local_f598672d-bd25-4a8c-bdb9-01a75980dd8e` / branch
   `skr-launcher-ui-26fd81`). WPF now has a `--lobby` mode that strips the
   SA-MP `-c -h -p` auto-connect flags so CMenuManager actually loads.
2. `SKR.Client.dll` → `SkrInitialize()`:
   - Reads launch params from a named pipe (`ipc::ReadLaunchParams`).
   - Installs D3D9 hooks (`EndScene` + `Reset` on the GTA device vtable,
     device pointer at `0x00C97C28`).
   - Installs SA-MP string patches, chat line-length patch, chat timestamps,
     and the SKR client signature at `0x00B7CE50`.
   - Initializes the lobby manager.
   - Initializes the AC policy enforcer (currently mock SKRAC adapter).
3. `samp-cef` loads (committed pin: `f14fe4faf7f0d036edf445ba02b4bddd4cccac377`)
   and calls `SKR.Client.dll!cef_initialize(InternalApi*)` → bridge becomes
   Ready. `cef_quit` is called on teardown. **`cef_samp_mainloop` is NOT
   called by samp-cef** — Phase 1 shipped it as a reserved no-op.
4. Intro (`client/intro/urban_intro.cpp`) draws through samp.dll's
   pre-Present proxy (RVA 0x75C73) and GTA's EndScene fallback. Finishes
   when connected (SA-MP netGame state 5) or when the hold timeout runs out.
5. Post-intro, `d3d9_hook.cpp` hands each EndScene to
   `skr::lobby::OnEndSceneAfterIntro(device)`, which:
   - Logs a one-shot `"first post-intro EndScene frame"` marker.
   - Calls `PatchGtaMainMenu()` each frame until it sticks → validates the
     canonical Hoodlum signature and writes `0xC3` at the one-byte entry
     instruction of `CMenuManager::Process` (0x0057B440). `0x0057B210` is
     inside the preceding routine and is never a patch target.
   - Calls `skr::lobby::OnCefTick()` → creates the lobby CEF browser with
     id `skr::cef::BrowserId::kMainMenu` once the intro reports
     `IsFinished()` and `cef_ready()` is true.
   - Draws a full-screen opaque black quad so no vanilla menu pixels leak
     through under the CEF browser.

### 2.2 Files Phase 1 added (`client/lobby/`)

| File | Role |
|---|---|
| `menu_patch.{h,cpp}` | Byte-patches CMenuManager; returns `MenuPatchStatus` with per-site `firstByte` + `reason` for diagnostic logging. Loose guard (rejects only 0x00/0xCC/0xFF + non-executable mappings). |
| `settings_io.{h,cpp}` | Reads/writes `%APPDATA%\SKR\launcher.ini` (same INI as WPF launcher-ui — `[launcher]` section, keys `nickname`, `gtaPath`, `resolution`, `windowed`, `intro`, `volume`). Mirrors `windowed`→`sa-mp.cfg`'s `fullscreen` key. Inline JSON encode/decode for the sidecar. |
| `lobby_manager.{h,cpp}` | Orchestrator. Owns the lobby browser lifecycle, subscribes `skr:lobby:*` CEF events, writes a `settings.json` sidecar, draws the D3D9 cover, handles Quit (sends `WM_CLOSE` + `ExitProcess(0)`). |
| `lobby.html` + `lobby.css` + `lobby.js` | Design port of the launcher-ui ingame-menu preview. Two modes (`lobby` default, `pause` stub); 4 settings tabs; servers list stub. |
| `assets/background.png` | 3.1 MB urban illustration shown by `body.in-game`. Phase 2 flips to `body.transparent` for a true pause overlay. |

CMake has a `SKRClient` POST_BUILD step that copies `lobby.html/css/js` and
`assets/background.png` to `$<TARGET_FILE_DIR:SKRClient>/SKR/lobby/`.
Deployment is one `cmake --build --preset release --target SKRClient` plus
copying both `SKR.Client.dll` and the `SKR/lobby/` folder next to the game.

### 2.3 Logs you can rely on

- `<game>\SKR\intro.log` — intro timeline / audio / fade state.
- `<game>\SKR\lobby.log` — menu patch state, CEF bridge readiness, browser
  creation.
- `<game>\SKR\game_state.log` — RPC 240 tick diagnostics (not relevant to
  Phase 2 unless the lobby ↔ in-game pause transition needs CGame hooks).

---

## 3. CEF events contract (Phase 1 shape)

Events the lobby page can emit (`cef.emit(name, payload?)`) and events the
C++ side emits back (`cef.on(name, cb)` in JS):

| Direction | Name | Payload (JS side) | Phase 1 behavior |
|---|---|---|---|
| JS→C++ | `skr:lobby:settings:get` | — | C++ writes a fresh `settings.json` next to `lobby.html` and emits `skr:lobby:settings:ready`. |
| JS→C++ | `skr:lobby:settings:save` | JSON string | **Stub.** C++ cannot read `CefListValue` payloads yet (see §4.1). JS toasts "Phase 2". |
| JS→C++ | `skr:lobby:play` | `{"address":"ip:port"}` | **Stub.** Toasts "Phase 2". |
| JS→C++ | `skr:lobby:disconnect` | — | **Stub.** Phase 2 target. |
| JS→C++ | `skr:lobby:resume` | — | **Stub.** Phase 2 pause coupling. |
| JS→C++ | `skr:lobby:quit` | — | Sends `WM_CLOSE` to the GTA window then `ExitProcess(0)`. |
| C++→JS | `skr:lobby:settings:ready` | — | JS refetches `settings.json`. |
| C++→JS | `skr:lobby:mode` | `"lobby"` \| `"pause"` | Flips `window.skrLobby.setMode(...)`. |

JS also exposes `window.skrLobby = { setMode(mode) }` as a direct hook for
Phase 2's native adapter.

---

## 4. Phase 2 scope (what to build)

Phase 2 flips the lobby from "static showcase" to "actually connects and
replaces the paused game menu". Three independent pieces:

### 4.1 CefListValue readers in the bridge (prereq for save/connect)

**Problem**: `client/cef-bridge/cef_api_types.h` exposes `cef_create_list`
on `InternalApi` but no getters, so when JS emits `cef.emit("x", "payload")`
and C++'s subscriber receives `CefListValue*`, we cannot read the payload.
Phase 1 settings save is a stub and `skr:lobby:play` can't read the server
address.

**Fix**: extend `InternalApi` with the getter functions samp-cef exports
on its own `cef-api` crate at commit `f14fe4fa`. The Rust side is at
`cef-api/src/lib.rs`. The functions we need (named on the Rust side via
`#[no_mangle] pub extern "C"` on `cef_list_value_*` wrappers):

```c
typedef size_t   (__cdecl *CefListSizeFn)       (CefListValue*);
typedef uint8_t  (__cdecl *CefListTypeFn)       (CefListValue*, size_t idx);
typedef bool     (__cdecl *CefListGetBoolFn)    (CefListValue*, size_t idx);
typedef int32_t  (__cdecl *CefListGetIntFn)     (CefListValue*, size_t idx);
typedef double   (__cdecl *CefListGetDoubleFn)  (CefListValue*, size_t idx);
// String getter: samp-cef returns a CefString; the C ABI typically writes
// into a user-provided UTF-8 buffer and returns required length.
typedef size_t   (__cdecl *CefListGetStringFn)  (CefListValue*, size_t idx,
                                                 char* out_utf8, size_t cap);
```

Verify every signature against the Rust source before using it. Mirror the
new fields at the END of the `InternalApi` struct in `cef_api_types.h` so
older samp-cef builds that stop at `cef_gta_window_active` don't break
layout; gate reads on `api_` struct size if samp-cef adds any form of
version negotiation. Then expose typed wrappers in `cef_bridge.{h,cpp}`
and plug them into:

- `lobby_manager.cpp` → `OnSettingsSave`: parse the first list arg as a
  UTF-8 JSON string, call `SettingsFromJson(json, s); SaveSettings(s);`,
  then emit `skr:lobby:settings:saved` (or `:error` on failure).
- `lobby_manager.cpp` → `OnPlay`: parse the address string, kick off the
  connect flow (§4.2).

### 4.2 Lobby → connect flow

Clicking JOACĂ or a server's "Conectează" button must:

1. Validate the server address (host + port parse, port 1..65535).
2. Dismiss the lobby UI: hide the CEF browser
   (`CefBridge::HideBrowser(kMainMenu)`), stop drawing the D3D9 cover,
   restore CMenuManager (`skr::lobby::RestoreGtaMainMenu()`) if we want
   the vanilla netgame reconnect path, OR skip the menu and drive the
   SA-MP connect ourselves.
3. Call the SA-MP connect API. SA-MP 0.3.7-R1 (what the user's install
   ships) exposes connection via `CNetGame::ConnectToServer(host, port,
   password)` — a `__thiscall` on `*(samp+0x26EB94)`. The exact RVA for
   `ConnectToServer` needs to be rediscovered on R1; cross-check against
   `D:\SA-MP Reverse\` which the shared workspace has. **Do not** assume
   the R5 RVA from the current string patches.
   Alternative (simpler, more robust across SA-MP builds): write the
   address to the SA-MP config structure and let the vanilla connect
   path reach it — SA-MP has `g_pHostAddress` / `g_wPort` globals; find
   their RVAs for R1 and overwrite.
4. The intro already handles the "world ready at netGame state 5"
   signal (`d3d9_hook.cpp::IsPlayerInGame`). Re-enable the Phase 1 fade
   transition `Covering → FadingOut → Done` once lobby ends and the
   player is in-game.

### 4.3 Pause coupling (in-game Esc → lobby overlay)

When the player is in-game and presses Esc, GTA SA normally opens
CMenuManager (which Phase 1 has suppressed). Phase 2 should:

1. Add a key-event hook (either a game-thread `GetAsyncKeyState(VK_ESCAPE)`
   poll in a CGame tick, or a dedicated `WH_KEYBOARD_LL` hook limited to
   the GTA window), gated by `IsPlayerInGame()` so Esc only arms once the
   player has entered the server.
2. On Esc: pause the game (CTimer::m_UserPause = true, address TBD; or
   call `CPauseMenu::StartPauseMenu` and immediately override), show the
   lobby browser in pause mode via `window.skrLobby.setMode('pause')`
   (emit `skr:lobby:mode`, payload `"pause"`), and flip
   `body.transparent` so the paused game frame shows through under the
   SKR UI.
3. "Continuă" (resume) → emit `skr:lobby:resume` → C++ unpauses, hides
   the browser, sets mode back to `"lobby"` (or leaves it — Phase 2
   decides).
4. "Deconectare" → emit `skr:lobby:disconnect` → C++ calls SA-MP's
   `CNetGame::ShutDown()` (RVA TBD on R1), returns to lobby mode on the
   Servere page.

### 4.4 Per-server loading screen

Each server can provide a loading-screen HTML served while the netgame
negotiation runs. Simplest shape:

1. Server-side (open.mp gamemode) ships a URL alongside the SA-MP
   `OnPlayerConnect` handshake — a new SKR RPC that the client reads
   before SA-MP's own join reply.
2. Client creates a second CEF browser (new id, e.g. `kLoadingScreen = 104`)
   pointing at that URL. The intro's existing hold logic (world-ready
   signal in `urban_intro.cpp`) already waits for netGame state 5 — reuse
   that path: show the loading browser until the state flip, then fade
   out into the HUD.
3. Must sandbox the per-server HTML: samp-cef runs with `--allow-file-access-from-files`,
   so remote URLs are fine but any `file://` scheme must stay inside
   `SKR\lobby\` and the loading-screen folder; never let a server-supplied
   URL become `file:///`.

---

## 5. Build & deploy

Requires: Visual Studio 2022 with Win32 toolchain, CMake 3.24+. Two deps
not committed to git — pull them before configuring:

- `deps/libsodium` (x86 static, VS143) — root of the `sodium` imported
  target in `CMakeLists.txt`.
- `deps/omp-sdk` — only needed for `SKRGate` (open.mp plugin). Not needed
  for lobby-only work.

Build:
```powershell
cmake --preset x86-release
cmake --build --preset release --target SKRClient
```

Deploy to a test install:
```powershell
Copy-Item out\x86-release\Release\SKR.Client.dll "<game>\"
Copy-Item -Recurse out\x86-release\Release\SKR\lobby "<game>\SKR\"
```

Run via the WPF launcher in `--lobby` mode (parallel session owns that
flag) so SA-MP's auto-connect args don't skip CMenuManager.

---

## 6. Known gaps & risks

- **CMenuManager offsets are 1.0 US only.** The compat profile
  (`client/compatibility/compat_profile.cpp`) has no hash gate yet. R2
  compat matrix is pending — do not announce R2 support without the
  matrix (AGENTS.md #15).
- **Input routing untested.** Phase 1 relied on samp-cef's
  `cef_focus_browser(kMainMenu, true)` to capture mouse/keyboard. If
  input bleeds through in your test, the mitigation is a low-level
  `WH_KEYBOARD_LL` + `WH_MOUSE_LL` hook gated by `IsLobbyActive()`.
- **Logs are the only in-game diagnostic.** `<game>\SKR\lobby.log` tells
  you whether CMenuManager patched, whether CEF became ready, and whether
  the browser was created. Reach for the one-shot markers:
  `first post-intro EndScene frame`, `CefBridge: Ready`,
  `CefBridge: cef_ready() -> true`, `creating lobby browser:`.
- **sa-mp.cfg mirror**: `MirrorFullscreenToSampCfg` touches
  `Documents\GTA San Andreas User Files\SAMP\sa-mp.cfg`. If the user has
  this file symlinked or redirected (OneDrive), writes may be captured
  by cloud sync — not a correctness issue but worth noting.
- **No tests cover the lobby.** `tests/unit/` has shared/ops tests but
  nothing for `client/lobby/`. Phase 2 should at least add a settings
  round-trip test (`SettingsToJson` / `SettingsFromJson` + ini
  read/write) because the save path is now reachable.
- **ExitProcess on Quit skips SA-MP's RakNet shutdown.** Fine for Phase
  1 (user asked for "închide procesul GTA curat") but Phase 2's in-game
  Quit should call `CNetGame::ShutDown()` before `ExitProcess`.

---

## 7. Shared workspaces

- Repository root (worktree):
  `C:\Users\Staark\Desktop\SKR Platform` (Phase 1 worktree)
- Parent session's shared notes:
  `docs/phase1-handover/` (Phase 1 development notes)
  — `skr-lobby-phase1.md` has the chronological Phase 1 handover (what
  landed, what was broken, how it was fixed).
- SA-MP reverse-engineering reference: `D:\SA-MP Reverse\` and
  `D:\GCMP\deps\SAMPC-main`.
- The GTA SA install the launcher-ui session deploys to:
  `C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto San Andreas\`.

---

## 8. Hand-off checklist for Phase 2

When Phase 2 lands, update this document's "What exists" and "Phase 2 scope"
sections so Phase 3 (per-server loading, chat integration, voice, etc.)
starts cold again from a current snapshot.
