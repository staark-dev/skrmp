# PHONE-001 — M0 Architecture Audit + M1 Visual Prototype

Date: 2026-10-24. Scope: M0 + M1 only. **Stopped for design/architecture approval.**
No gameplay hooks, no Pawn/Gate protocol change, nothing linked into SKR.Client.dll,
nothing committed.

## Verification status

| Item | Status |
|---|---|
| `client/phone/*` (layout, controller, model, renderer) | Implemented, compiled (MSVC Win32 Release, 0 warnings) |
| `SKRPhoneTests` (layout at 9 resolutions × 6 scales, state machine, model) | Unit-tested: **648/648 pass** |
| `SKRPhonePreview` (real D3D9 HAL device, production ImGui DX9 backend) | 36 PNGs rendered, 0 failed |
| In-game rendering, input, animation, multiplayer | **Not done — not authorised in M1** |

Previews: `docs/phone-previews/m1/` (`*_1360x768`, `*_1920x1080`, `*_2560x1080`,
`*_zoom2x` crops, `report.txt`). Backdrop = real SA-MP screenshot of an SKR session
(`skr-001.png`, cover-fit per resolution); all phone values are sample data, tagged
`PREVIEW` inside the device and in the caption.

| Required preview | File |
|---|---|
| Closed | `01_closed` |
| Opening transition | `02a_opening_90ms`, `02b_opening_170ms` |
| Home | `03_home`, `03b_home_hover_messages`, `13_home_uiscale_080/125` |
| Contacts | `04_contacts` (hovered row) |
| Messages | `05a_messages_threads`, `05b_messages_conversation` |
| Incoming call | `06_incoming_call` |
| GPS | `07_gps` |
| Extra | `08_settings`, `09_phone_dialer`, `10a/10b` transitions, `11_notification_banner`, `12_closing_120ms` |

Rebuild/re-render:
```
cmake --build build-gate --config Release --target SKRPhoneTests SKRPhonePreview
build-gate\Release\SKRPhoneTests.exe
build-gate\Release\SKRPhonePreview.exe docs\phone-previews\m1 "<...>\SAMP\screens\skr-001.png"
```

## 1. Architecture (proposed, prototype matches it)

```
client/phone/
  phone_layout.h        pure: measured device geometry from backbuffer + UI scale
  phone_controller.*    pure: state machine + all animation timing
  phone_model.h         fixed-size presentation model + per-app view state
  phone_renderer.*      ImDrawList geometry only (shell, pages, overlays, hit regions)
  -- later (M2+) --
  phone_input.*         ownership policy, WndProc routing (inside pause_input chain)
  phone_gate.*          Native API adapter (requests / acks)
  phone_character.*     animation + held-object adapter
```

- **States**: `HIDDEN, OPENING, HOME, APP_ACTIVE, APP_TRANSITION, CLOSING, SUSPENDED`.
  One controller, so transitions can't compete. Requests during a transition are
  rejected, except Close, which completes the transition at once.
- **Timing**: progress = elapsed ms / duration (identical at 30/144 fps, unit-tested).
  Open and close use the same out-cubic curve, so an interrupted open/close reverses
  from the exact on-screen position. `Update()` is a no-op while hidden.
- **Apps**: Home, Phone, Messages (list → conversation page), Contacts, GPS, Settings.
  Each app has its own view-state struct, and the renderer dispatches by `AppId`.
- **Layout**: unit `u = H/768 × scale` (scale clamped 0.75–1.35). Body 400u × 196u,
  bottom-right, 28u/18u margins. It never enters the top 30 % of the screen (GTA HUD);
  if it would, it shrinks. On ultrawide it anchors inside a centred 21:9 region.
- **Rendering**: draw-list only (no ImGui windows), so ImGui never wants input on its own.
  Pages never push clip rects; lists scroll under an opaque header. That lets a whole
  page be scaled or translated for transitions by transforming its vertices after
  recording.
- **Text**: drawing 8–12 px text from the pause renderer's 40 px faces makes thin stems
  vanish (seen in the first previews: "NAVIGAT NG"). The renderer now takes faces at 3
  raster sizes per family and picks the closest. **Integration requirement:** the ImGui
  owner must rasterise ~10u and ~20u faces for the current backbuffer height and
  rebuild them on resolution change.

## 2. Existing phone-system audit (gamemode)

Source: `server-runtime/includes/skr/apps/phone.inc` (814 lines; identical copy in
`server-runtime-branded-test`), included by `skr_dev.pwn` (`SKR_APP_PHONE 1`).

| Area | Finding |
|---|---|
| Presentation | **CEF** browser 103 (`http://cef/apps/phone/index.html`), layer 11, created on `OnCefReady` |
| Key | `L` (0x4C) through `CEF_SetKeyCapture` / `OnCefPressKey`; blocked while `CEF_IsChatInputOpen` |
| Open/close | toggles browser focus, `CEF_ToggleChatInput(false)`, custom escape-menu mode |
| Numbers | **none**: identity = playerid / name |
| Contacts | = online players, built on request (`OnPhoneContacts`), no storage |
| SMS | `OnPhoneSms(to=playerid, text)`, 120 chars, 1 s flood guard, recipient must be online, no persistence |
| Calls | **none** |
| Other apps | bank (SQLite `skr.db`: `accounts`, `txns`), vehicles, services (taxi-like requests), camera/photo mode, map pin |
| Animation / objects | **none** in phone.inc. `skr_dev.pwn` uses attached-object **slot 8** (helmet, model 18645) |
| Gate/Native API use | none |

Consequences:
- The server owns the business logic, but there are no phone numbers, no calls and no
  message history. The brief's `SetNumber` / incoming-call / delivery features need new
  gamemode logic. The client must not invent it.
- The native phone and the CEF phone must not both react to the key. For SKRE clients
  the gamemode should skip creating browser 103, or the key should be released.
- Bank, vehicles, services and camera exist only in CEF. Their native versions are out
  of PHONE-001 scope.

## 3. Client integration points (verified in source)

| Concern | Existing owner | Phone use |
|---|---|---|
| ImGui | `native-pause/pause_renderer.cpp` (single context, DX9+Win32 backends, device-swap rebind, Reset handling) | Add phone to the existing gameplay pass in `native_pause::OnEndScene`: `if (pauseOpen \|\| gps::panel::WantsRender() \|\| phone::WantsRender())`. No new hook or context. |
| Input | `pause_input.cpp` WndProc subclass + DirectInput vtable ownership (`ShouldOwnDirectInput`) + SA-MP R5 `SetCursorMode` (RVA 0xA06F0) | Phone routing as one more branch in `HandleMessage`. Reuse `SetSampInputOwnership` / cursor helpers. **No new hook chain.** |
| Native API confirm | `native_api::OnWndProc` (gameplay only, before ESC) | Phone yields while `ConfirmActive()` |
| GPS | `client/gps/` engine, panel, display policy (`panel::SetDisplayOverride`) | GPS app reads engine state and requests waypoints. No second router. |
| Notifications | C3 renderer **not in tree** | Phone banner is self-contained; future adapter via `gps_notify`-style sink |
| Game state | `game_state.cpp` Tick (CGame::Process), `ResolveZones`, `ReadSeat` | Phone eligibility (on foot, alive, not in menu), location string |

**Gaps found:**
1. No client code detects **SA-MP chat input open** or **SA-MP dialog open**. Phone
   ownership needs both. They require new SA-MP R5 offsets, verified the same way
   `kSampR5SetCursorModeRva` was.
2. `pause_renderer::Render` creates a `D3DSBT_ALL` **state block every frame** it renders.
   Once the phone keeps that pass alive during gameplay, this becomes the main per-frame
   cost. Recommendation: create the state block once and recreate it on Reset (M2/M6
   measurement item).
3. Native API v1 is **server-initiated only** (Call S→C, Reply C→S matches a server
   call id) and inbound messages are capped at **46 bytes**. Phone actions (send SMS,
   dial, pick a destination) are client-initiated and SMS needs ~125 bytes. That requires
   a protocol extension (§6).

## 4. Input-routing proposal (M2)

Order inside `pause_input::HandleMessage` (existing chain, no new subclass):

1. Binding capture → settings → Native API confirm (unchanged).
2. **ESC**: if the phone owns input → `phone.Back()` (page → app root → home → close);
   it does not open the pause menu. A ringing call ignores ESC.
3. Pause open / native submenu → phone `Suspend()`. The phone never draws over the pause menu.
4. Phone key (configurable, default **L** — see decision D1), on key-down, no repeat →
   `Toggle()`, but only if `CanOpen()`:
   on foot, alive, not in a vehicle [D3], no SA-MP chat input, no SA-MP dialog, no
   Native API confirm, no lobby, pause state = Gameplay.
5. While the phone owns input: mouse messages go to the phone's hit regions (cursor shown
   via the existing helpers, SA-MP cursor mode 2 so the camera and fire stay locked,
   DirectInput zeroed via `ShouldOwnDirectInput() || phone.OwnsInput()`). The wheel
   scrolls the hovered list. All other gameplay keys are consumed except T/F6 (chat):
   those close the phone first and then pass through.
6. Release paths (each one restores the previous SA-MP cursor mode and hides the cursor
   only if no other owner wants it): close done, `WM_KILLFOCUS`/Alt+Tab → `Suspend()`,
   death/disconnect/`TickNegotiation` reset → `Reset()`, `SkrShutdown` → `Reset()` and
   release, D3D9 Reset → nothing (geometry only; fonts rebuilt by the owner).

Safety invariant (to be unit-tested in M2): when `!phone.OwnsInput()` the phone never
consumes a message and never holds cursor or DirectInput ownership.

## 5. Character-animation plan (M3)

Verified against the installed game (`Steam\...\anim\ped.ifp`): the only phone
animations are **`phone_in`, `phone_talk`, `phone_out`**. `anim.img` has none.
**There is no stock "look at handset" pose.**

| Need | Option | Notes |
|---|---|---|
| Talking (call) | SA-MP `SPECIAL_ACTION_USECELLPHONE` / `STOPUSECELLPHONE` set by the **server** | SA-MP attaches model 330 in hand, plays phone_in/talk, and **syncs it to every player** with no slot and no new protocol. Recommended for calls. |
| Viewing (browsing apps) | (a) `phone_in` blended and held at a mid frame, before the hand reaches the ear | Must be checked in game. Client-local only. |
| | (b) Custom IFP (handset in front of the chest) through the Resources Manager | Correct look. Remote players see it only via server `ApplyAnimation` and only on SKRE clients. |
| Held object | model **330** `cellphone` (default.ide) | Reused. The SA-MP special action already handles hand-bone attachment. No skin change. |
| Attached-object slot | gamemode uses slot 8 | If a slot is needed (view pose), reserve it through a Pawn constant, never hardcode it. |

UI timing: the UI starts its slide when `phone_in` reaches the "phone in hand" point
(~250–300 ms), not at keypress. The controller's `Open()` is deferred by the character
adapter.

Multiplayer split: local presentation (UI, local pose) | server-authoritative (special
action / ApplyAnimation set by the gamemode after a phone-open request) | remote
(standard SA-MP sync). A client-local attached object would **not** replicate.

## 6. Proposed API contract (M4, not implemented)

Transport: the existing 0xE0 envelope on RPC 240/241 with **new steps**, a new
capability bit `8 Phone`, negotiated in Hello/Welcome (old clients and servers unaffected):

| Step | Dir | Layout |
|---|---|---|
| 9 Request | C→S | `E0 09 u32 session u16 reqId u8 op u16 len payload` (phone ops ≤ 160 B inbound) |
| 10 Ack | S→C | `E0 0A u32 session u16 reqId u8 result` (accepted / rejected / rate-limited / error) |
| 7 Call (new ops) | S→C | `PhoneState`, `PhoneMessage`, `PhoneIncomingCall`, `PhoneCallEnded`, `PhoneNotify`, `PhoneContacts` (paged) |

Pawn (illustrative; names follow `SKRE_`):
```pawn
native SKRE_Phone_SetEnabled(playerid, bool:enabled);
native SKRE_Phone_SetNumber(playerid, const number[]);
native SKRE_Phone_PushContact(playerid, index, const name[], const number[]);   // bounded 64
native SKRE_Phone_PushMessage(playerid, const peer[], const text[], bool:outgoing, msgid = 0);
native SKRE_Phone_ShowIncomingCall(playerid, callid, const name[], const number[]);
native SKRE_Phone_EndCall(playerid, callid);
native SKRE_Phone_Notify(playerid, const title[], const text[]);
native SKRE_Phone_AckRequest(playerid, requestid, SKRE_PHONE_RESULT:result);
forward OnSKREPhoneOpened(playerid);
forward OnSKREPhoneClosed(playerid);
forward OnSKREPhoneRequest(playerid, requestid, SKRE_PHONE_OP:op, const target[], const text[]);
```
The client shows `Sending...` until `Ack` arrives (see the conversation preview) and
never claims delivery or a connected call without it. The existing `OnPhoneSms` logic
in phone.inc becomes the `OnSKREPhoneRequest` handler. Numbers and storage stay in the
gamemode.

GPS: the phone uses the GPS API-003.3 destination list and waypoint protocol once it
exists, plus local engine state. No phone-specific GPS protocol.

## 7. Performance

Measured in `SKRPhonePreview` (`report.txt`). CPU only: draw-list build including ImGui
NewFrame/EndFrame, Release, this machine.

| Screen | 1360×768 | 1920×1080 | vertices (1080p) |
|---|---|---|---|
| Home | 54 µs | 63 µs | 7.8 k |
| Contacts | 26 µs | 32 µs | 3.0 k |
| Conversation | 29 µs | 33 µs | 3.5 k |
| GPS | 34 µs | 36 µs | 4.0 k |
| Incoming call | 30 µs | 32 µs | 3.0 k |
| **Hidden** | **0.0013 µs** (`Update` early-out, no draw) | | |

- Idle overhead is effectively zero: nothing is drawn and no input is processed while hidden.
- Risks: (1) the per-frame `D3DSBT_ALL` state block in the shared pass (§3 gap 2);
  (2) Home is the heaviest screen (layered shadows, wallpaper glow and windows) and can
  drop ~40 % of its vertices by caching the wallpaper into a texture at open time if M6
  measurements require it; (3) font atlas growth from 4 extra faces (~1–2 MB for Latin
  ranges at 1080p); (4) GPU cost of the gameplay ImGui pass was not measured in GTA.
- No allocation, file I/O, IPC or polling in the render path. Lists are bounded
  (64 contacts, 48 messages, 12 destinations).

## 8. Decisions needed before M2

| # | Decision | Recommendation |
|---|---|---|
| D1 | Default key. F4 is SA-MP's class-reselect key [UNVERIFIED in source, standard SA-MP behaviour]; L is the existing gamemode phone key | **L**, configurable. Gamemode disables the CEF phone for SKRE clients. |
| D2 | Viewing pose: held `phone_in` frame vs custom IFP | Try (a) in M3. Fall back to (b) only if it looks wrong. |
| D3 | Phone usable in vehicles? | Passengers only (driver = no). |
| D4 | Protocol extension (§6: steps 9/10, cap bit 8) | Approve the shape. Implement in M4. |
| D5 | Visual direction of the prototype (palette, wallpaper, icon set) | Your review of the previews |
