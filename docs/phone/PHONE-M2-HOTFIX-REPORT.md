# PHONE-M2 Hotfix — One-Time Phone Visibility & Input Lifecycle

Date: 2026-10-24. Status: **implemented, compiled, unit-tested. Live GTA test: NOT TESTED.**

## A. Root cause

### A1. Primary defect — wrong ped flag offset blocked opening (source-verified)

`phone_input.cpp` read "in vehicle" as `CPed + 0x46C, bit 0`. Per plugin-sdk
`CPed.h` (`m_roadRageWith` 0x460, `field_464`, `field_468`, flags at 0x46C), bit 0 of
0x46C is **`bIsStanding`**. `bInVehicle` is the ninth flag bit: **byte 0x46D, bit 0**.

Resulting `CanOpen()` behaviour on foot:

| Player situation | Old read | Old passenger check (`m_pVehicle` 0x58C) | Result |
|---|---|---|---|
| Standing, never drove | "in vehicle" | `m_pVehicle` = null → not passenger | **blocked** |
| Airborne (spawn drop, jump, fall) | not in vehicle | — | opens |
| Standing after leaving a car | "in vehicle" | `m_pVehicle` keeps the last car, its driver ≠ ped → "passenger" | opens |

The phone could open only while the player was in the air or after leaving a vehicle.
Every other L press failed `CanOpen()` silently and the key passed through. That matches
"appeared once, then never again".

### A2. Contributing defects fixed in the same path

- **Focus recovery was unreachable.** `WM_SETFOCUS → Resume()` sat behind
  `if (!OwnsInput()) return false`, and a Suspended phone does not own input.
- **L during CLOSING was eaten.** `Visible()` is true while closing, so the handler called
  `Close()`, which returns false there. The press was consumed with no effect, so an
  interrupted close could not reopen.
- **Unguarded memory reads.** The SA-MP singletons and the ped were dereferenced without
  protection, inside the WndProc.
- **Pause over phone blinded the pause cursor.** After `Suspend()`, the phone's next `Tick`
  called `HidePhoneCursor()`, which forced `ShowCursor(FALSE)` and SA-MP cursor mode 0
  while the pause menu owned the cursor.
- **Suspended phone survived death.** The death reset only checked `Visible()`.

### A3. Old CEF phone conflict — reported, not changed (server-runtime is protected)

`server-runtime/includes/skr/apps/phone.inc` calls `CEF_SetKeyCapture` + `CEF_EnableKey(L)`
on `OnCefReady`. `OnCefPressKey` toggles CEF browser 103 with `CEF_FocusBrowser(…, true)`.
omp-cef's `WndProcHook::EnsureInstalled` (deps/omp-cef/src/client/core/hooks/wndproc.cpp)
re-installs itself on top of the WndProc chain every 250 ms. So on a server running
`phone.inc`, CEF sees every L before the native phone. A focused CEF browser then consumes
**all mouse messages** (`BrowserManager::OnWndProcMessage`), so the native phone gets no
clicks. `cef_local.log` shows omp-cef loaded in every recent session. Whether the joined
server ran `phone.inc` is **not verified**. Fixing this needs a gamemode decision (see E).

### A4. Investigated and ruled out

- **Double dispatch.** `native_pause::OnWndProc` has no callers. Only `HookedWndProc` feeds
  the phone.
- **Tick/render circular dependency.** `phone::input::Tick` runs every `OnEndScene` before,
  and independently of, the render decision.
- **Render path.** The phone draws in the gameplay pass after GPS, on the shared ImGui
  context, with no extra hook. Fonts are built once in `renderer::Initialize`, and D3D9
  reset only recreates device objects, so the `ImFont*` pointers stay valid.
- **Frame delta.** Progress clamps to 0..1, and a backwards clock gives `dt = 0`.

## B. Fix (files)

| File | Change |
|---|---|
| `client/phone/phone_policy.h` (new) | Pure `Evaluate(GameView)`, `KeyEdge` (repeat bit + 60 ms duplicate latch that cannot wedge), `OnPhoneKeyEdge` (Closing → reopen), `WhileOpen` (death → reset, driver → close), `ShouldRender` |
| `client/phone/phone_input.cpp` | `bInVehicle` = byte 0x46D bit 0; vehicle checked only while `bInVehicle`; SEH-guarded reads, with each failure logged once and treated as "closed"; focus handling moved before the ownership guard; cursor release defers to the pause menu; `WhileOpen` checked for every non-Hidden state; `[PHONE-M2]` diagnostics |
| `tests/unit/test_phone_m2.cpp` (new), `server/skr-gate-omp/CMakeLists.txt` | `SKRPhoneM2Tests` target (+2 lines) |

The fix is safe because chat/dialog/driver protection is preserved. A failed SA-MP read
blocks nothing, as before. No hook, ImGui context, keybinding, visual, GPS, pause, C3 or
IPC code changed.

Diagnostics go to `<launcher dir>\SKR\phone.log`, gated by `[Client] DebugLog`
(default 1). They are written only on events: init, build tag, L edge,
`Open allowed`/`Block reason`, state edges, input-owner edges, first rendered frame per
open, one-time SA-MP read failures, death/driver resets.

## C. Regression results

**Automated (build-gate, Release):**

- `SKRPhoneTests`: 648 passed, 0 failed.
- `SKRPhoneM2Tests`: 241 passed, 0 failed. Coverage:
  - 20 open/close cycles
  - rapid presses
  - auto-repeat
  - duplicate message and lost key-up
  - interrupted open/close
  - chat and dialog
  - pause suspend/resume
  - Alt+Tab with a large frame delta and a backwards clock
  - renderer absent
  - driver/passenger transitions
  - death, disconnect and respawn
  - no stale render or click targets

These tests cover the pure policy and controller exactly as `phone_input` calls them. Win32
message delivery, SA-MP memory and real rendering are not covered.

**Live GTA:** **NOT TESTED.**

## D. Build report

| Item | Value |
|---|---|
| Config | `build-test-certification-win32`, VS 18 2026, Win32, Release, `SKR_EXTERNAL_LOBBY=ON`, `SKR_TEST_CERTIFICATION=ON` |
| DLL | `build-test-certification-win32\Release\SKR.Client.dll`, 831 488 B, built 2026-10-24 00:47 |
| SHA-256 | `8781E173855B6174400DEC1FA00BEBD5A51B9CB0D57BE66EFF84C63BA5B6ED08` |
| Build tag in DLL | `SKR.Client Oct 24 2026 00:47:25` (logged as `Build:` in phone.log) |
| Compiler | 0 errors, 0 warnings in SKRClient and phone test logs |

**Loaded-path evidence (last runs, 00:32):**

- `%APPDATA%\SKR\preflight.log` lists the launcher as `build-test-certification-win32\Release`.
- The DLL loaded was `…\build-test-certification-win32\Release\SKR.Client.dll`.
- The GTA executable was `C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto San Andreas\gta_sa.exe`,
  SHA-256 `A559AA77…CBD26`, verdict `EXE_APPROVED`.

**Exe "hash rejection" explained (by design, untouched):** that executable is the canonical
1.0 US Hoodlum binary. `exe_fingerprint.cpp` grants `compatibilityApproved` only under
`#ifdef SKR_TEST_CERTIFICATION`. Every normal build, `build-external-lobby` included,
therefore rejects it with `EXE_PROFILE_NOT_CERTIFIED`. Also, `build-external-lobby\Release`
has no `samp\` or `cef\` folders, so it is not a runnable launcher folder. Its DLL is
**stale** (pre-hotfix) and must not be copied. `build_test_certification.bat` configures
`build-test-certification` without `-A Win32`. The working tree is the separately configured
`build-test-certification-win32`.

## E. Known limitations / open items

1. **No live verification.** User procedure below.
2. **CEF phone conflict (A3): fixed in branded-test only.** In
   `server-runtime-branded-test/includes/skr/apps/phone.inc` the CEF phone no longer
   forwards L (`CEF_EnableKey(…, false)`). `gamemodes/skr_dev.amx` was rebuilt with the
   original flags (`-d1`); the previous file is kept as `skr_dev.amx.pre-phone-m2`. The
   stable `server-runtime` is unchanged and still captures L. The configured branded-test
   gamemode `skr_runtime_test` does not include `phone.inc`, so the conflict was not active
   in the 10-24 test sessions.
3. **Pause-menu suspension resumes only on L.** Focus-loss suspension resumes automatically.
4. **`bInVehicle` byte 0x46D comes from plugin-sdk field order.** It is not yet confirmed
   live; check it with `phone.log` (`Block reason`) in a car and on foot.
5. **R5-only chat/dialog detection.** Other SA-MP builds log "not the pinned 0.3.7-R5 build"
   once, and chat/dialog then never block.

## Live test procedure (user)

1. Run `build-test-certification-win32\Release\SKRLauncher.exe` and join the server.
2. Spawn and stand still. Press L: the phone should appear. Open Messages, then press ESC to
   return Home. Press L to close. Repeat 20×.
3. Press T: the phone closes and chat opens. Type a word containing "l": the phone must not
   open. Press ESC on chat, then L: the phone opens.
4. Trigger a server dialog and press L: blocked. Close the dialog and press L: it opens.
5. Open the phone, then press ESC on Home: it closes. Open it, then open the pause menu: the
   pause cursor works. Resume, then press L: the phone opens.
6. Alt+Tab with the phone open, then return: the phone slides back in.
7. As driver, L is blocked. As passenger, L opens. Move to the driver seat: the phone closes.
8. Die with the phone open: it closes. After respawn, L works. Reconnect, and restart GTA.
9. Send `SKR\phone.log` from the launcher folder. Every press must show `Key L down edge`
   followed by a `State:` line or a `Block reason:` line.
