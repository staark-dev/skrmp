# PHONE-M3 — Native Character Animation & Physical Phone

Date: 2026-10-24. Status: **implemented, compiled, unit-tested. GTA runtime: NOT TESTED.**

## A. Mechanism

M3 adds no custom animation framework and no memory patch. CJ's physical phone is GTA's own
**`CTaskComplexUseMobilePhone`** (task 1600). The phone starts it in the ped's
`TASK_SECONDARY_PARTIAL_ANIM` slot, the way the engine's `TASK_USE_MOBILE_PHONE` opcode
(0x0729) does. Each step below was read from the 1.0 US binary (`gta_sa.bak.exe`,
PE 0x427101CA), with addresses taken from plugin-sdk:

| Step | Engine behaviour (address) |
|---|---|
| Start | `CTask::operator new` 0x61A5A0 → ctor 0x6348A0 (`duration = -1`, until quit) → `CTaskManager::SetTaskSecondary` 0x681B60, slot 4 |
| First sub-task (0x634C10) | Saves the current weapon (`CPed+0x70C`), switches to fists, sets the ped's phone flags, plays **phone_in** (anim 0x91, blend 4.0) |
| Prop (0x634D60) | `AddWeaponModel(330)` (cellphone, 0x5E5ED0) once phone_in passes **0.85 s**. This is the ped's weapon-model slot: **no SA-MP attached-object slot is used**, and slot 8 (helmet) is untouched |
| Hold | **phone_talk** (0x93) loops while open |
| Close | We set `m_bQuit` (+0x1D) and call `MakeAbortable(LEISURE)`. This mirrors GTA's quit helper 0x634A40. **phone_out** (0x92) plays; at ~1.566 s GTA removes model 330 and restores the saved weapon, and the task ends |
| Hard stop | `MakeAbortable(IMMEDIATE)` removes the model and restores the weapon (0x6348F0). Then `SetTaskSecondary(nullptr, 4)` deletes the task through its virtual destructor (confirmed in 0x681B60) |

The anims are partial (upper body), so CJ can **walk and turn** while holding the phone,
as in single-player phone calls. GTA's own task also disables sprint during the phone.

**Hold-pose experiment (default OFF).** `[Phone] HoldPose=1` in
`%LOCALAPPDATA%\SKR\client.ini` freezes phone_in at `HoldPoseMs` (default 1000, clamped to
860–1500). It does this by setting `m_fCurrentTime`/`m_fSpeed` on the phone_in
`CAnimBlendAssociation` (lookup 0x4D68B0, +0x20/+0x24). The lower bound of 860 ms keeps the
freeze after the 0.85 s point where GTA adds the model. Whether that frame looks natural is
**unverified**. If it doesn't, leave it off: the native phone_talk pose is the supported
default. On close the freeze is released before phone_out, so the hand passes the ear once
on the way down.

**SA-MP synchronisation (source-verified, R5 decompile).** `CLocalPlayer::GetSpecialAction`
(0x10003410) reports `SPECIAL_ACTION_USECELLPHONE` only from SA-MP's own `CPed+0x48` flag.
That flag is written solely by SA-MP's `ToggleCellphone` (0x100AE070); reading it is a
plain field read (0x100AE0A0). A GTA task started by us therefore does **not** set the
cellphone special action. The separate on-foot animation field (`CLocalPlayer+0xFC`) was
not traced, so whether remote players can see the phone anims is **unverified**. It needs a
two-client test.

**Ownership.** The coordinator starts a task only into an **empty** slot. It remembers the
task pointer it created and acts only while the slot still holds that pointer and its
`GetId()` is 1600. A slot holding anything else is foreign, for example a server-set
cellphone action or a script animation: the phone then runs UI-only and never touches it.
Remembered pointers are compared, never dereferenced, so a respawned or destroyed ped can't
cause a stale access.

## B. Source changes

| File | Change |
|---|---|
| `client/phone/phone_body.h/.cpp` (new) | Pure coordinator: one task per open session, graceful quit, immediate abort, foreign/flush/ped-swap handling, hold-pose experiment. No game access |
| `client/phone/phone_body_gta.h/.cpp` (new) | GTA backend (SEH-guarded engine calls, addresses above), `[Phone]` ini reader |
| `client/phone/phone_policy.h` | `BodyRequest()`: the pose follows input ownership. On foot and alive → pose. Passenger → UI only. Dead/no ped → urgent |
| `client/phone/phone_input.cpp` | Owns the coordinator. Updated once per frame in `Tick` (no game reads while hidden). Released urgently on `Shutdown`. Phase and cost logging |
| `client/phone/phone_renderer.cpp`, `phone_layout.h` | Shadow: alpha 255→130, spread 18u→16u, offset 6u→4u |
| `CMakeLists.txt` | +2 SKRClient sources |
| `server/skr-gate-omp/CMakeLists.txt`, `tests/unit/test_phone_m3.cpp` (new) | `SKRPhoneM3Tests` |
| `docs/phone-previews/m3/shadow-{before,after}/` | 36 + 36 offscreen previews |

Not touched: renderer hooks, ImGui context, pause/GPS/C3/Resources Manager/launcher IPC,
and the `server-runtime/` tree.

## C. Evidence

**Shadow (offscreen previews, same real SA-MP backdrop, 1920×1080).** Compare
`shadow-before/03_home_1920x1080_zoom2x.png` with `shadow-after/…`. Darkening of the
backdrop measured against the closed-phone frame:

| Position | Before | After |
|---|---|---|
| Peak (body edge) | 70% | 44% |
| Below, 0–6 px | 70% | 44% |
| Below, 10 px | 70% | 29% |
| Below, 16 px | 38% | 8% |
| Side, 1 / 5 / 9 / 14 px | 70 / 55 / 32 / 15% | 43 / 33 / 14 / 4% |

Peak −37%, visible ink roughly halved, and no slab under the device. Open and close keep
the shadow consistent because the whole device, shadow included, is faded by one
vertex-alpha pass. These are **offscreen ImGui previews, not in-game screenshots.**

**Character animation:** **NOT TESTED.** I could not run GTA, and no screenshots were
taken. Procedure in E.

## D. Tests

Automated (build-gate, Release):

- `SKRPhoneTests`: 648/648.
- `SKRPhoneM2Tests`: 241/241.
- `SKRPhoneM3Tests`: **116/116**. The suite runs controller + KeyEdge + policy + coordinator
  frame by frame against a mock of the binary-verified task lifecycle. It covers:
  - one phone_in per open and one phone_out per close
  - 20 cycles with no leaked task or prop
  - rapid toggle (25 taps: at most 2 tasks, never 2 live)
  - interrupted open, and reopen during phone_out (exactly one restart, after it ends)
  - death, by hard abort or by engine flush
  - disconnect, ped swap and respawn (no stale-ped calls)
  - shutdown release
  - driver blocked; passenger UI-only; entering a vehicle puts the phone away
  - pause and focus suspension
  - foreign task untouched
  - server ClearAnimations: no restart within the same session
  - hold-pose engage and release
  - hidden phone making zero game calls

The suite caught a real bug during development: the coordinator restarted the task on the
frame GTA ended it after a close. It was fixed before the final build.

GTA runtime: **NOT TESTED.**

## E. Performance

- **Coordinator, mock backend** (measured, `SKRPhoneM3Tests`, this PC): 4.4 ns/frame while
  active, 1.1 ns/frame while hidden. The mock excludes GTA memory access.
- **Hidden phone in game:** the body update returns before any game read (unit-tested:
  zero backend calls).
- **Live cost:** `phone.log` prints `Body cost: N frames, avg X us, max Y us` at the end of
  every phone session, timed with QPC around the real GTA backend. I have no in-game numbers
  yet.

## F. Known limitations

| Item | State |
|---|---|
| Passenger pose | **Not supported**: the UI works and no pose is started (seated phone task not verified) |
| Remote players see the phone | **Not intended, unverified.** The cellphone special action is not set (verified); the on-foot anim field is untraced |
| Calls / SMS | Not functional; sample data only (M4) |
| Hold pose | Experimental, off by default, visual quality unverified |
| Armed player | GTA's task holsters the weapon and restores it on phone_out/abort (engine behaviour, not runtime-verified) |
| CEF phone conflict | **Not active locally.** Stable and branded-test servers both run `skr_runtime_test`, which does not include `phone.inc`. Branded-test `phone.inc` no longer captures L. Stable `server-runtime/…/phone.inc` still does (protected, unchanged) |

**M4 migration (CEF phone).** Before any gamemode that includes `phone.inc` is used with
SKR Enhanced clients:

1. Stop `CEF_EnableKey(L)` in the stable copy too (same 1-line change as branded-test).
2. Gate `SKR_PhoneOpen` on "client has no native phone" (Gate capability flag).
3. Route SMS/bank data to the native phone through the M4 protocol instead of CEF browser 103.
4. Retire browser 103 once every client is native.

## Build

`build-test-certification-win32`, VS 2026, Win32 Release, `SKR_TEST_CERTIFICATION=ON`.
0 errors and 0 warnings in SKRClient and the phone targets.

- DLL: `build-test-certification-win32\Release\SKR.Client.dll`, 836 096 B,
  built 2026-10-24 01:13:46.
- **SHA-256 `9173FD0B93B17EDA7AEFA9626EEE57E63D7CA3C5C5F4D03DCE8700DB67BB2189`**.
- Build tag in the DLL / phone.log: `SKR.Client Oct 24 2026 01:13:07`.

It is not yet verified that the launcher loaded this DLL: check that the `Build:` line in
`phone.log` matches the tag above.

## Live test procedure (user)

1. Launch `build-test-certification-win32\Release\SKRLauncher.exe`. Spawn and stand still.
   Take a screenshot of CJ (F8).
2. Press L. CJ reaches to his pocket, the phone appears in his hand (~0.85 s), and he holds
   it in the phone_talk pose while the UI is open. Take screenshots during phone_in and while
   holding.
3. Open Messages, return Home, then press L. CJ plays phone_out, the phone leaves his hand,
   and he returns to idle. Take screenshots.
4. Repeat 20×. Also test: tap L rapidly; L then L at once; walk and turn while holding; with
   a weapon equipped (it should come back after closing); ESC pause; Alt+Tab; T chat; enter
   a car as passenger (the phone is put away, the UI stays); move to the driver seat (UI
   closes); die with the phone out (no phone in hand after respawn); reconnect.
5. Optional: set `[Phone] HoldPose=1` in `%LOCALAPPDATA%\SKR\client.ini`, restart, compare
   the pose, and judge whether it looks natural.
6. With a second client watching, check what remote players see.
7. Send `SKR\phone.log` from the launcher folder. It should show `Body: IDLE -> ACTIVE` on
   open, `ACTIVE -> QUITTING` on close, `QUITTING -> IDLE` plus a `Body cost:` line after
   phone_out, and no `lost` count growing during normal use.
