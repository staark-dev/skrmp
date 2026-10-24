# PHONE-M4-AUDIT — Existing Systems & Compatibility Matrix

**Date:** 2026-10-24
**Status:** Phase M4-A complete

---

## 1. Existing CEF Phone Backend (`server-runtime/includes/skr/apps/phone.inc`)

### Architecture
- CEF browser 103, key L (`0x4C`), layer 11.
- HUD browser 101 used for banner notifications when phone is closed.
- SQLite database `skr.db` for bank (`accounts`, `txns` tables).
- Player identification: **SA-MP player ID** (runtime, not persistent).
- No persistent phone numbers, no persistent contacts, no persistent SMS, no call system.

### Implemented CEF Phone Features

| Feature | Status | Implementation |
|---|---|---|
| Open/Close (L key) | Working | `OnCefPressKey` → `SKR_PhoneOpen/Close` |
| Player state sync | Working | `SKR_PhoneEmitState` — sends id, name, cash, bank, location, pin |
| Contacts | **Online-only list** | `OnPhoneContacts` — iterates connected players, sends playerid+name. No saved contacts. |
| SMS | **Session-only, by playerid** | `OnPhoneSms` — CEF→server→CEF relay. 120-char max. 1s flood guard. No persistence. |
| Bank | Working (SQLite) | Deposit/withdraw (ATM proximity), transfers, balance, transaction log |
| Vehicles | Working | List owned vehicle, locate/engine/lock actions |
| Services | Working | Taxi/Mechanic/Medic requests, accept, cancel (session-only TTL) |
| Camera | Working | Photo mode — hides HUD 6 seconds for F8 capture |
| Map/Pin | Working (optional) | Integration with `map.inc` — set/clear/get pin, GPS pin display |
| Notifications | Working | Top-drop banner via `SKR_PhoneNotify` through CEF browser |

### What Does NOT Exist in the Backend
- **No persistent phone numbers** — players are identified by runtime playerid only.
- **No saved contacts** — the "contacts" list is just online players.
- **No SMS persistence** — messages are CEF events relayed in-memory, lost on disconnect.
- **No call system** — no call state machine, no ringing, no accept/decline.
- **No MySQL** — bank uses SQLite `skr.db`. No phone-specific MySQL tables exist.
- **No conversation history** — no database tables for SMS.

---

## 2. Native Phone Client (M1-M3)

### Files
```
client/phone/phone_controller.{h,cpp}  — state machine (HIDDEN→OPENING→HOME→APP_ACTIVE→CLOSING)
client/phone/phone_model.h             — data model (contacts, threads, conversations, GPS, settings)
client/phone/phone_renderer.{h,cpp}    — ImGui rendering
client/phone/phone_layout.h            — measured layout
client/phone/phone_input.{h,cpp}       — L key, mouse, WndProc integration
client/phone/phone_body.{h,cpp}        — M3 coordinator
client/phone/phone_body_gta.{h,cpp}    — GTA CTaskComplexUseMobilePhone integration
client/phone/phone_policy.h            — eligibility, focus, cursor
```

### State
- M1: Visual prototype, 648/648 unit tests. **Working.**
- M2: Interactive input, L toggle, ESC/T/F6, cursor, SA-MP compatibility. 241/241 tests. **Working in GTA.**
- M3: Character animation (phone_in/phone_out), body coordinator. 116/116 tests. **Not GTA-tested yet.**

### Native Phone Data Model (phone_model.h)
The model is already structured for server-backed data:
- `Contact`: name[24], number[16], hue
- `ThreadSummary`: peer, preview, time, unread, hue
- `Message`: text[121], time[8], `Delivery` enum (Received/Sending/Delivered/Failed)
- `Conversation`: peer, number, hue, items[48], count
- `GpsInfo`: status, destName, destArea, remainingM, destinations list
- `Caller`: name, number, hue
- `PhoneSettings`: uiScale, openKey, vibrate, sounds, banners
- **`previewData` flag** — renderer shows PREVIEW tag when true

### External Event Hooks (phone_input.h, already declared for M4)
```cpp
void OnIncomingCall(const char* name, const char* number, uint64_t now);
void OnCallEnded();
void OnPushNotification(const char* title, const char* text, uint64_t now);
```
These are declared but not wired to any transport yet.

---

## 3. SKR Gate Transport (Native API v1)

### Existing Envelope
- RPC 240 client→server, RPC 241 server→client.
- Enhanced marker `0xE0`, steps 1-4 (handshake/rejection), steps 5-8 (Native API).
- After Enhanced handshake: Hello(5)→Welcome(6)→Call(7)/Reply(8).

### Message Size Limits (VERIFIED)
| Direction | Max Size | Header | Payload |
|---|---|---|---|
| Server→Client (Call, step 7) | **413 bytes** | 13 bytes | **400 bytes** |
| Client→Server (Reply, step 8) | **46 bytes** | 14 bytes | **32 bytes** |
| Client→Server (Hello, step 5) | 12 bytes | fixed | — |

**The "46-byte limit" applies only to Reply messages (client→server).** The server→client direction has a generous 400-byte payload.

### Current Ops (v1)
| Op | Direction | Payload | Reply |
|---|---|---|---|
| 1 Notify | S→C | toast (kind + duration + text ≤144) | none |
| 2 Confirm | S→C | modal yes/no (timeout + title + message) | exactly one |
| 3 HudSet | S→C | component + value | none |
| 4 CancelConfirm | S→C | targetCallId | none |

### Capabilities Negotiated
`CapNotify (1)`, `CapConfirm (2)`, `CapHudValue (4)`. No phone capability yet.

### Key Design Point
The Native API is **server-initiated only** in v1. The server sends Calls; the client can only Reply. There is no mechanism for the client to send an arbitrary request to the server today.

---

## 4. GPS System

### Architecture
```
client/gps/gps_engine.h      — per-frame tick, waypoint polling, route lifecycle
client/gps/gps_route.h/.cpp  — CPathFind pathfinding, route geometry
client/gps/gps_renderer.h/.cpp — D3D9 minimap route rendering
client/gps/gps_destination.h/.cpp — waypoint tracking, arrival detection
```

### Available API
```cpp
skr::gps::Tick(playerX, playerY, playerZ);  // per-frame
skr::gps::GetRoute();    // read-only RouteSnapshot
skr::gps::GetStatus();   // NavStatus: Idle/Calculating/Navigating/Arrived/Unavailable
skr::gps::Reset();       // on disconnect
```

### Status
- Pathfinding, route rendering, deviation detection, arrival detection all implemented.
- **GPS ships DISABLED** pending staged runtime testing (crash on first test 2026-10-23).
- Fix built + unit-tested in API-003.0.3, not yet runtime-verified.
- No server-side GPS — entirely client-side using GTA's CPathFind.

### Phone Integration Needed
- The phone's GPS app needs to read `GetStatus()` and `GetRoute()` for display.
- Setting a destination = placing a GTA map waypoint (the GPS engine auto-detects it).
- Clearing a route = clearing the waypoint.
- The phone model's `GpsInfo` struct already matches the engine's output shape.
- No destination list from the server exists yet (API-003.3 planned, not implemented).

---

## 5. CEF Coexistence — L Key Conflict

### Current State
- **Stable runtime** (`server-runtime/includes/skr/apps/phone.inc`): L key **enabled** for CEF phone.
- **Branded-test runtime** (`server-runtime-branded-test/includes/skr/apps/phone.inc`): L key **disabled** (`CEF_EnableKey(playerid, SKR_PHONE_KEY, false)` in `OnCefReady`).
- The branded-test gamemode (`skr_runtime_test`) does NOT include `phone.inc`, so the conflict was not active in 10-24 test sessions anyway.

### Coexistence Strategy Required
1. The server must detect whether a client is SKR Enhanced (already done: `IsEnhanced(id)` in Gate).
2. For Enhanced clients: disable CEF phone key capture → native phone handles L.
3. For standard/legacy clients: CEF phone works as before.
4. Decision is server-authoritative: the Gate or gamemode decides, not the client.

---

## 6. PHONE-M3 Verification Status

Per `TASK-PHONE-001-STATE.md`:
- M3 implemented 2026-10-24, 116/116 unit tests pass.
- `phone_body_gta.cpp`: CTaskComplexUseMobilePhone at slot 4, model 330.
- Phone_in anim at 0.85s, phone_out at ~1.566s.
- **NOT GTA-runtime tested** — user live test pending per M3 report.
- Shadow alpha reduced (255→130), spread (18→16), offset (6→4).

---

## 7. Compatibility Matrix

| Feature | Existing Backend | Native UI (M1-M3) | Missing for M4 | Required Changes |
|---|---|---|---|---|
| **Phone numbers** | None | Model has `ownNumber[16]` | Persistent number registry | New DB table, allocation on first spawn, Pawn native |
| **Contacts** | Online players only (CEF) | Contact list + detail | Saved contacts, add/remove | New DB table, CRUD protocol, Gate relay |
| **SMS send** | CEF event relay, session-only | Composer + bubbles | Persistence, delivery ack | New DB table, new protocol ops, Gate relay |
| **SMS receive** | CEF event, session-only | Conversation view | Push to native client | New Gate→client Call ops |
| **SMS history** | None | Thread list + messages | Everything | DB table, pagination protocol |
| **Calls** | None | Incoming-call screen, dialer | Everything | Call state machine (server), signaling protocol |
| **GPS** | Client-side engine exists | GPS app screen | Read engine state | Wire `gps_engine` to `phone_model::GpsInfo` |
| **Settings** | None persistent | Settings screen | Local persistence | Write to SKR config file |
| **Bank** | SQLite `skr.db` | Not in native M1 | Not in M4 scope | — |
| **Vehicles** | Working (CEF) | Not in native M1 | Not in M4 scope | — |
| **Services** | Working (CEF) | Not in native M1 | Not in M4 scope | — |
| **Camera** | Working (CEF) | Not in native M1 | Not in M4 scope | — |

---

## 8. Transport Design Constraint — Bidirectional Extension Required

The Native API v1 is **server-initiated only**: the server sends Call (step 7), the client can only Reply (step 8, max 32-byte payload).

**For PHONE-M4, the client must be able to initiate requests** (send SMS, add contact, dial, request sync). This requires extending the protocol:

### Option A: New Client→Server Request Step
Add step 9 (Client Request) to the envelope. Server handles it like it handles Reply today. This is the cleanest approach — it mirrors the Call/Reply pattern.

### Option B: Overload RPC 240 Client-State
Piggyback phone requests on the existing client-state RPC. This is fragile and conflates two message types.

### Option C: CEF Event Tunnel
Route native phone actions through CEF events (the way the CEF phone does it). This creates a CEF dependency the native phone must not have.

**Recommended: Option A.** Add a new step (9 = ClientRequest) to the envelope. Server routes it to a phone handler. Reply comes back as a standard Call (step 7) with phone-specific ops.

### Payload Budget
- Client→Server request: needs to carry SMS text (120 chars) + recipient number (15 chars) + op code + request ID. ~140 bytes minimum for SMS_SEND. The current Reply max is 32 bytes — **insufficient**.
- Solution: define Client Request with its own size limit (e.g., 200 bytes), separate from Reply's 32-byte limit.

---

## 9. Database Schema Required (New)

The existing `skr.db` (SQLite) handles bank only. Phone data requires:

| Table | Purpose | Key columns |
|---|---|---|
| `phone_numbers` | Persistent number registry | account_name, number (unique), created_ts |
| `phone_contacts` | Saved contacts | owner_account, contact_number, display_name, created_ts |
| `phone_messages` | SMS persistence | id, sender_number, recipient_number, text, timestamp, delivered |

**Decision needed:** SQLite (like bank) or MySQL? The task spec mentions MySQL, but the existing phone backend uses SQLite only. MySQL plugin is loaded (`server-runtime/config.json` legacy plugins include `mysql`... actually let me verify).

The server loads: crashdetect, mapandreas, sscanf, streamer. **No MySQL plugin is loaded.** SQLite via `db_*` natives is the only database available without adding a plugin dependency.

**Recommendation:** Use SQLite `skr.db` for M4, consistent with the existing bank system. MySQL can be a future migration.

---

## 10. Summary of Required M4 Work

1. **Protocol extension:** Add client-initiated request step (step 9) to the Enhanced envelope, with phone-specific ops.
2. **Phone number registry:** SQLite table + allocation logic in Pawn.
3. **Contacts CRUD:** SQLite table + protocol ops + Gate relay.
4. **SMS persistence:** SQLite table + send/receive/history protocol.
5. **Call signaling:** Server state machine + protocol ops (signaling only, no voice).
6. **GPS integration:** Wire `gps_engine` API to phone model (client-only, no protocol needed).
7. **Settings persistence:** Local config file read/write (client-only).
8. **CEF coexistence:** Server-side Enhanced detection → disable CEF phone key for Enhanced clients.
9. **Native phone adapter:** New client module that translates protocol messages to/from `phone_model.h` data structures.

### What Can Be Reused
- The entire Native API v1 transport infrastructure (RPC 240/241, envelope, session model).
- The GPS engine (`client/gps/`) — read-only integration.
- The phone UI (M1-M3) — no changes to rendering.
- The phone data model (`phone_model.h`) — already structured for server data.
- The Gate's `IsEnhanced()` check — for CEF coexistence.
- SQLite `skr.db` — extend with phone tables.

### What Must Be New
- Client→Server request mechanism (envelope step 9).
- Phone-specific ops (both directions).
- Server phone handler in the Gate or as a Pawn include.
- SQLite schema for numbers/contacts/messages.
- Call state machine (server-side).
- Client phone adapter (protocol ↔ phone_model).
