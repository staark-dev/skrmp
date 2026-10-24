# SKR Native Runtime API — v1 (TASK API-001)

Bidirectional, versioned API between Pawn scripts, the SKR Gate open.mp
component and the SKR Enhanced client. First milestone: one complete
confirm roundtrip plus notification and one HUD value.

| Layer | File |
|---|---|
| Wire codec (shared, header-only) | `shared/protocol/native_api.h` |
| Unit tests | `tests/unit/test_native_api.cpp` (target `SKRNativeApiTests`, `server/skr-gate-omp/CMakeLists.txt`) |
| Server (Gate) | `server/skr-gate-omp/native_api_server.cpp` (+ routing/state in `skr_gate_component.{hpp,cpp}`) |
| Client session | `client/native-api/native_api.cpp` (+ hooks in `client/game-state/game_state.cpp`) |
| Client UI | `client/native-api/native_api_ui.cpp` (+ key hook in `client/native-pause/pause_input.cpp`) |
| Pawn API | `server/gamemode-integration/skr_enhanced.inc` |
| Roundtrip test | `server/gamemode-integration/skre_test.pwn` → `server-runtime-branded-test/filterscripts/` |

## 1. Transport — one transport, no new RPC IDs

The API rides inside the existing **SKR Enhanced envelope** (`enhanced.h`,
first byte `0xE0`):

| Direction | RPC | Existing users of the envelope |
|---|---|---|
| client → server | 240 (`client_state.h`) | step 1 Hello, 3 Confirm |
| server → client | 241 (`session_info.h`) | step 2 Offer, 4 Rejection |

The API owns envelope **steps 5–8**. RPC 242 (radar style) and 243 (radar rect)
are not touched. The Enhanced handshake (steps 1–3, exactly 64-bit messages,
capabilities `0x07`) is unchanged, so old clients and old servers keep working:

* old client + new server: never sends step 5 → API never becomes ready, natives return 0.
* new client + old server: step 5 reaches `Negotiate()`, which ignores non-64-bit
  messages → no Welcome → client marks the API unavailable after 5 s.
* any client + plain SA-MP / open.mp server: no Enhanced handshake → nothing is sent.

Routing:
* Server: `SkrClientStateHandler::onReceive` peeks marker + step; steps 5–8 go
  to `SkrGateComponent::OnApiMessage`, everything else is unchanged.
* Client: `OnSessionInfoRpc` checks for marker + step 5–8 **first** (the older
  branches are gated on the handshake deadline) and forwards to
  `native_api::OnServerMessage` only when the Enhanced handshake is confirmed.

## 2. Session model

```
client                                   server (Gate)
Enhanced handshake confirmed (step 3) ─▶ EnhancedSession.token = T
Hello{versions 1..1, token T, caps} ───▶ token == T ? version = max common
                                         session id S (random, != 0)
                        ◀─── Welcome{version, S, caps = offered & supported}
                                         OnSKREClientReady(playerid)  [queued → onTick]
            ◀── Call{S, callId, op, payload}
Reply{S, callId, op, result} ──────────▶ matches pending call? resolve once
                                         OnSKREConfirmResponse(...) [queued → onTick]
disconnect: client Reset() / server erases SkrPlayerData → S and pending calls die
```

* **Binding**: Hello must echo the Enhanced token of *this* connection.
* **Session id**: generated per Welcome, carried in every Call and Reply.
  A reconnect creates new per-player state, so anything carrying an old id is
  rejected (`stale-session`).
* **Call ids**: server-assigned, monotonic per session, never 0.
* **Duplicate Hello**: ignored (no session churn within a connection).

## 3. Wire format (little-endian, byte-aligned, length-exact)

Trailing bytes, unknown steps/ops/results/enum values and out-of-range values
are rejected. Strings are `u8 length + bytes`, never NUL-terminated on the wire,
length ≥ 1, bytes `>= 0x20`, not `0x7F`, not `~` (CFont token prefix).

| Step | Dir | Layout | Size |
|---|---|---|---|
| 5 Hello | C→S | `E0 05 u8 verMin u8 verMax u32 enhancedToken u32 caps` | 12 |
| 6 Welcome | S→C | `E0 06 u8 version u8 status u32 sessionId u32 caps u16 maxCallPayload u16 0` | 16 |
| 7 Call | S→C | `E0 07 u32 sessionId u32 callId u8 op u16 len payload[len]` | 13 + ≤400 |
| 8 Reply | C→S | `E0 08 u32 sessionId u32 callId u8 op u8 result u16 len payload[len]` | 14 + ≤32 (v1: 0) |

No API message is 8 bytes long, so none can be mistaken for a handshake message.

Welcome status: `0` accepted, `1` version unsupported (no session).

Capabilities: `1` Notify, `2` Confirm (+CancelConfirm), `4` HudValue.
Server masks unknown client bits; client rejects a Welcome granting bits it did not offer.

### Ops (Call payloads)

| Op | Payload | Reply |
|---|---|---|
| 1 Notify | `u8 kind(0 info,1 success,2 warning,3 error) u8 durationSec(1..15) str8 text(≤144)` | none |
| 2 Confirm | `u8 timeoutSec(5..120) str8 title(≤64) str8 message(≤200)` | exactly one |
| 3 HudSet | `u8 component(1 = vitals numbers) u8 value(0/1)` | none |
| 4 CancelConfirm | `u32 targetCallId` | none |

Reply results: `1` confirmed, `2` cancelled, `3` busy, `4` unsupported, `5` timeout (client countdown), `6` invalid.
Mapped to Pawn `SKRE_RESPONSE`: confirmed 1, cancelled 2, busy 3, timeout 4, unsupported 5, error 6.

## 4. Limits and abuse handling (server)

| Rule | Value | Effect |
|---|---|---|
| Calls per player per second | 10 | native returns 0 (HudSet is state, not limited) |
| Replies per player per second | 10 | dropped, logged |
| Pending confirms per player | 1 | `SKRE_ShowConfirm` returns 0 |
| Confirm server deadline | timeout + 3 s | `SKRE_RESPONSE_TIMEOUT` callback |
| Invalid inbound messages | 20 per connection | API traffic ignored for the rest of the connection (`quarantined`), no kick |
| Inbound size | ≤ 46 bytes | larger → invalid |

Replies only ever resolve a call the server itself issued (session id + call id + op).
The first matching reply resolves it; later copies are `unknown-call`.

## 5. Threading

* Server: RPC handlers, `onTick` and natives run on the open.mp main thread.
  Script callbacks are queued (`apiEvents_`) and dispatched from `onTick`, never
  from inside packet handling; queued events whose session changed are dropped.
  `OnSKREClientDisconnected` is called synchronously from `onPlayerDisconnect`.
* Client: RakClient RPC handlers, `CGame::Process` tick, `CHud::Draw` hook and
  WndProc all run on the GTA thread. Replies are sent from Tick, never from WndProc.
  No network work happens in the render path.

## 6. Client UI (minimal, new)

No in-game (non-ESC) UI existed, so two minimal components were added in
`native_api_ui.cpp`, drawn in GTA's 2D HUD pass with `CSprite2d::DrawRect` and
`CFont` — the same primitives as `radar_hud.cpp`. No ImGui, no D3D9 hook changes.

* Notifications: top-centre toasts, ≤3 visible, queue of 8 (oldest dropped), fade in/out.
* Confirm: centred modal, dimmed backdrop, `[Y] CONFIRM` / `[N] CANCEL`, countdown.
  Keys: Y / Enter = confirm, N / ESC = cancel. Keys are ignored for 400 ms after
  it opens. Handled in `pause_input.cpp` only in the Gameplay state, before the
  ESC→pause logic. Only those four keys are consumed while it is open.
* HUD value: `radar_hud::SetServerVitalsNumbers` — session-only override of
  `hud.ini` `[Vitals] ShowNumbers`, never written to disk, cleared on disconnect.
  Radar visibility and scale are not touched (RPC 242 stays the only radar path).

## 7. Diagnostics

Server console, one line per event, `key=value`:
```
[SKR API] player=0 event=ready version=1 session=1a2b3c4d caps=0x7 client_caps=0x7 welcome=sent
[SKR API] player=0 event=confirm-request call=1 request=7001 timeout=30s sent
[SKR API] player=0 event=confirm-reply call=1 request=7001 result=1
[SKR API] player=0 event=notify call=2 kind=1 len=29 sent
[SKR API] player=0 event=invalid reason=unknown-call
[SKR API] player=0 event=confirm-timeout call=3 request=7001 (server deadline)
[SKR API] player=0 event=disconnected pending_confirm=none
```
Invalid-message logging is capped at 5 lines per connection.

Client: `<gta>\SKR\native_api.log` (≤200 lines per session): `hello-sent`, `ready`,
`notify`, `confirm-open`, `reply ... sent`, `hud-set`, `invalid reason=...`,
`unavailable reason=...`, `reset`.

## 8. Not in this milestone

* `skr-runtime.json` HUD defaults are **not** wired to the API yet. Intended
  model (HUD Runtime Integration decisions): JSON = per-player defaults applied by
  the Gate on connect; `SKRE_SetHudVisible` = explicit override. Only one HUD
  value (vitals numbers) exists in the API today.
* No GPS / phone / HUD suite / generic notification framework.
* Reply payloads are reserved (≤32 bytes, always empty in v1).

## 9. Compatibility & security checklist

Status legend: **U** unit-tested here · **C** compile-checked here · **W** needs Windows runtime test.

| # | Check | Status |
|---|---|---|
| 1 | Codec rejects truncated, oversized, trailing-byte, zero-id, unknown-op/result messages | U |
| 2 | Strings: control bytes, DEL, `~`, empty, over-length rejected by decoder; sanitized on server | U |
| 3 | No API message is 8 bytes (cannot hit the 64-bit handshake parser) | U |
| 4 | Version negotiation picks highest common, refuses no-overlap | U |
| 5 | Stale session / unknown call / wrong op / duplicate reply rejected | U |
| 6 | Rate limiter 10/s, resets per window and on clock going backwards | U |
| 7 | Gate compiles against open.mp SDK 08cc69a + omp-network f223fbd | C (64-bit syntax check) |
| 8 | Client files compile as 32-bit Windows code (`-Wall -Wextra -Wshadow`) | C (MinGW syntax check; SEH tokens shimmed) |
| 9 | `skre_test.pwn` compiles with pawncc 3.10.11, no warnings; AMX natives/callbacks match the Gate | C |
| 10 | Full roundtrip (Section 10) | **W** |
| 11 | SA-MP 0.3.7-R5 vanilla client joins the SKR server, natives return 0, no errors | **W** |
| 12 | SKR client on a plain SA-MP/open.mp server: no Hello sent, nothing in `native_api.log` | **W** |
| 13 | SKR client on a Runtime with the old Gate: `unavailable reason=no-welcome`, gameplay normal | **W** |
| 14 | Disconnect with dialog open → reconnect: no dialog, new session id, no stale callback | **W** |
| 15 | ESC with dialog open cancels the dialog and does not open the pause menu | **W** |
| 16 | Dialog/toast layout and text size at 720p / 1080p / 1440p (CFont metrics are estimates) | **W** |
| 17 | RPC 242 radar style/size still applied; radar rect (243) still reported | **W** |
| 18 | 0.3.DL still rejected; Task #16/#17 tests still pass | **W** (no change to that code) |

## 10. Roundtrip test (Windows)

1. Build the Gate: `cmake --build build-gate --config Release` (CMake re-configures
   itself: new source `native_api_server.cpp`, new target `SKRNativeApiTests`).
   Run `SKRNativeApiTests` (expect `native_api: 70 checks passed`). Copy
   `build-gate/Release/SKRGateOMP.dll` to `server-runtime-branded-test/components/Gate.dll`
   (test runtime only — not `server-runtime/`). Until this is done the filterscript
   fails to load ("native not found"); the gamemode is unaffected.
2. Build the client: `cmake --build build-external-lobby --config Release -j4`.
3. Start `server-runtime-branded-test`. The console must show
   `skre_test` loaded (it is listed in `config.json` `pawn.side_scripts`).
4. Join with the SKR Enhanced client. Expect `[SKR API] ... event=ready` in the
   console, `[SKRE] Native API ready` in chat and a green toast.
5. `/skreconfirm` → dialog appears. Press **Y**.
   Expect: console `confirm-request` → `confirm-reply result=1`; filterscript prints
   `OnSKREConfirmResponse(id, 7001, 1)`; armour = 100; toast "Roundtrip OK".
6. `/skreconfirm`, press **N** → response 2, "Cancelled" toast, armour unchanged.
7. `/skreconfirm`, wait 30 s → response 4 (client timeout), "Request timed out".
8. `/skrebusy` → chat `first=1 second=0`. `/skrecancel` → dialog closes, no callback.
9. `/skrespam` → `10 of 15 accepted`.
10. `/skrenumbers on` / `off` → health/armour numbers appear/disappear next to the strip.
11. Quit with a dialog open, rejoin → no dialog; console shows a new `session=`.
