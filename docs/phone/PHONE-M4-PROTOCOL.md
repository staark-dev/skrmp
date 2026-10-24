# PHONE-M4-PROTOCOL — Bidirectional Phone Protocol (Revised)

**Date:** 2026-10-24 (revised 2026-10-24)
**Status:** Design specification (M4-B, corrected)
**Depends on:** Native API v1 (`shared/protocol/native_api.h`, `docs/protocol/SKR_NATIVE_API.md`)

---

## 1. Transport Extension: Client Request (Step 9)

### Problem

The Native API v1 is server-initiated only. The server sends Calls (step 7,
up to 400 bytes payload); the client can only Reply (step 8, max 32 bytes
payload). Phone operations require the client to initiate requests — sending
an SMS, adding a contact, dialing a number. These payloads exceed the 32-byte
Reply limit (an SMS alone is up to 120 bytes + metadata).

### Solution: New Envelope Step 9 — ClientRequest

Add step 9 to the existing `0xE0` envelope, sent client→server on RPC 240
(same RPC as Hello and Reply). The server handles it alongside existing API
messages. No new RPC IDs.

#### Wire Format

```
E0 09 u32 sessionId u32 requestId u8 domain u8 op u16 payloadLen payload[payloadLen]
```

| Field | Size | Description |
|---|---|---|
| marker | 1 | `0xE0` (kEnhancedMarker) |
| step | 1 | `9` (ClientRequest) |
| sessionId | 4 | Current API session (from Welcome) |
| requestId | 4 | Client-assigned, monotonic, nonzero. Echoed in server response. |
| domain | 1 | Feature domain (1 = Phone). Extensible to future features. |
| op | 1 | Operation within the domain |
| payloadLen | 2 | Payload byte count |
| payload | 0..200 | Operation-specific data |

**Header size:** 14 bytes
**Max payload:** 200 bytes
**Max total size:** 214 bytes

### Constraints

- Session id must match the current API session (stale → reject).
- Request id must be nonzero and must not repeat within a session (server
  tracks the last seen request id per player; going backwards → reject).
- Domain must be known (unknown domain → reject with PHONE_ERROR).
- Op must be known within the domain.
- Payload length must match exactly (trailing bytes → reject).
- Rate-limited: max 10 requests per second per player (reuses the existing
  Reply rate limiter window logic).
- The server NEVER trusts a client-supplied player id or account id. The
  sender is identified from the authenticated session's `accountId`.

### Why Not Reuse Reply?

Reply (step 8) is semantically tied to answering a server-initiated Call. It
carries a `callId` that must match a pending Call, and its `result` field is an
enum for Confirm responses. Phone requests are client-initiated with different
semantics. A new step keeps the protocol clean and the two message types
independently evolvable.

---

## 2. Server Response: Phone Call Ops

Server responses to client phone requests use the existing Call (step 7)
mechanism with new ops in the **Phone domain** (ops 16–31, leaving 1–15 for
existing/future core API ops). Each response carries the client's `requestId`
in its payload so the client can match it to the pending UI action.

### New Capability Bit

```cpp
CapPhone = 1u << 3   // bit 3 = 0x08
```

Added to `kKnownCapabilities`. The client advertises it in Hello; the server
grants it in Welcome **only when the Pawn script has called `SKRE_PhoneEnable()`**
(see §12). Phone ops are only sent/received when both sides have `CapPhone`.

### New Ops (Server→Client, domain = Phone, inside Call step 7)

| Op | Name | Payload | Worst-case bytes | Description |
|---|---|---|---|---|
| 16 | PhoneSync | `u32 reqId, str8 ownNumber(7), u8 flags` | 13 | Response to PHONE_SYNC. |
| 17 | ContactList | `u32 reqId, u8 count, u8 flags, [str8 name(≤23) str8 number(7) u8 hue] × count` | 6 + 33n (n≤11) = 369 | Contacts page. |
| 18 | ContactResult | `u32 reqId, u8 status` | 5 | Ack for CONTACT_ADD/REMOVE. |
| 19 | SmsThreadList | `u32 reqId, u8 count, u8 flags, [str8 peerName(≤23) str8 peerNumber(7) str8 preview(≤47) u32 ts u8 unread u8 hue] × count` | 6 + 86n (n≤4) = 350 | Thread summaries page. |
| 20 | SmsHistory | `u32 reqId, u8 count, u8 flags, [u32 msgId str8 text(≤120) u32 ts u8 delivery u8 direction] × count` | 6 + 131n (n≤3) = 399 | Message history page. |
| 21 | SmsSendResult | `u32 reqId, u8 status, u32 messageId` | 9 | Persistence ack for a sent SMS. |
| 22 | SmsReceived | `str8 senderName(≤23) str8 senderNumber(7) str8 text(≤120) u32 ts u32 messageId u8 hue` | 162 | Push: new SMS arrived (no reqId). |
| 23 | CallState | `u32 reqId, u32 callSessionId, u8 state, str8 peerName(≤23) str8 peerNumber(7) u8 peerHue u32 startTime` | 50 | Call state transition. |
| 24 | PhoneError | `u32 reqId, u8 errorCode, str8 message(≤64)` | 70 | Generic error response. |

### Worst-Case Byte Size Proofs (400-byte Call payload limit)

Every response's maximum serialized size is calculated below. All string
fields use the `str8` wire encoding: 1 byte length prefix + up to N content
bytes. Phone numbers are always exactly 7 digits.

**ContactList (op 17):**
- Header: u32(4) + u8(1) + u8(1) = 6 bytes
- Per entry: str8(1+23) + str8(1+7) + u8(1) = 33 bytes
- Max entries: floor((400 − 6) / 33) = floor(394/33) = **11**
- Worst case: 6 + 11×33 = **369 bytes** ≤ 400 ✓

**SmsThreadList (op 19):**
- Header: u32(4) + u8(1) + u8(1) = 6 bytes
- Per entry: str8(1+23) + str8(1+7) + str8(1+47) + u32(4) + u8(1) + u8(1) = 86 bytes
- Max entries: floor(394/86) = **4**
- Worst case: 6 + 4×86 = **350 bytes** ≤ 400 ✓

**SmsHistory (op 20):**
- Header: u32(4) + u8(1) + u8(1) = 6 bytes
- Per entry: u32(4) + str8(1+120) + u32(4) + u8(1) + u8(1) = 131 bytes
- Max entries: floor(394/131) = **3**
- Worst case: 6 + 3×131 = **399 bytes** ≤ 400 ✓

**SmsReceived (op 22):**
- str8(1+23) + str8(1+7) + str8(1+120) + u32(4) + u32(4) + u8(1) = **162 bytes** ≤ 400 ✓

**CallState (op 23):**
- u32(4) + u32(4) + u8(1) + str8(1+23) + str8(1+7) + u8(1) + u32(4) = **46 bytes** ≤ 400 ✓
- (With callSessionId added: 50 bytes.)

All other responses are trivially small (PhoneSync 13B, ContactResult 5B,
SmsSendResult 9B, PhoneError 70B).

### Op Value Allocation

```
1..15   — Core API (existing: Notify=1, Confirm=2, HudSet=3, CancelConfirm=4)
16..31  — Phone domain
32..47  — Reserved (future domains)
```

---

## 3. Client Request Ops (Phone Domain)

Sent via ClientRequest (step 9), domain = 1 (Phone).

| Op | Name | Payload | Max bytes | Description |
|---|---|---|---|---|
| 1 | PHONE_SYNC | (empty) | 0 | Request own phone number and status. |
| 2 | CONTACT_LIST | `u8 page` | 1 | Request contacts page. Page 0 = first. |
| 3 | CONTACT_ADD | `str8 number(7) str8 displayName(≤23)` | 32 | Save a contact. |
| 4 | CONTACT_REMOVE | `str8 number(7)` | 8 | Delete a contact. |
| 5 | SMS_THREADS | `u8 page` | 1 | Request thread summaries page. |
| 6 | SMS_HISTORY | `str8 peerNumber(7) u8 page` | 9 | Request conversation page. |
| 7 | SMS_SEND | `str8 recipientNumber(7) str8 text(≤120)` | 130 | Send an SMS. |
| 8 | CALL_DIAL | `str8 number(7)` | 8 | Initiate a call. |
| 9 | CALL_ACCEPT | `u32 callSessionId` | 4 | Accept an incoming call. |
| 10 | CALL_DECLINE | `u32 callSessionId` | 4 | Decline an incoming call. |
| 11 | CALL_HANGUP | `u32 callSessionId` | 4 | End an active or outgoing call. |

All payloads fit within the 200-byte ClientRequest payload limit.

### Pagination (consistent across all paginated ops)

All paginated requests (CONTACT_LIST, SMS_THREADS, SMS_HISTORY) carry a `u8
page` field. Page 0 is the first page. All paginated responses carry a `u8
flags` field where bit 0 (`hasMore`) indicates whether more pages exist.

| Response | Max per page | Page field source |
|---|---|---|
| ContactList | 11 | CONTACT_LIST `u8 page` |
| SmsThreadList | 4 | SMS_THREADS `u8 page` |
| SmsHistory | 3 | SMS_HISTORY `u8 page` |

### Call Session ID (stale-action protection)

Call actions (CALL_ACCEPT, CALL_DECLINE, CALL_HANGUP) carry a `u32
callSessionId` that must match the server's current call session. The server
generates a unique callSessionId per call attempt (monotonic counter per
server lifetime). The CallState response delivers this ID to both parties.

If a client sends CALL_ACCEPT with a stale callSessionId (e.g., from a
previous call that already ended), the server rejects it with
PhoneError(STALE_CALL). This prevents race conditions where a call ends
and a new call begins before the client processes the ENDED state.

### SMS Idempotency

The server tracks the last successful SMS requestId per player. If the client
retransmits an SMS_SEND with the same requestId (e.g., after a timeout without
receiving SmsSendResult), the server returns the same messageId without
inserting a duplicate row. The client should use monotonically increasing
requestIds to make this work correctly.

---

## 4. Phone Number System

### Format

Phone numbers are 7-digit strings: `"1000000"` to `"9999999"`.
- Purely in-game — not real phone numbers.
- Unique per account (tied to `accountId`, not to player nickname).
- Allocated on first PHONE_SYNC if the account has none.
- Stored persistently in SQLite.

### Allocation

Server-side, sequential from a counter stored in a `phone_meta` table.
Starting at `1000000`. The counter increments atomically (SQLite transaction).
If the number space is exhausted (won't happen in practice), allocation fails
and the player gets PhoneError(INTERNAL_ERROR).

---

## 5. Call State Machine (Server-Authoritative)

```
                    DIAL
  IDLE ────────────────────▶ OUTGOING
   ▲                            │
   │                      recipient online?
   │                       ┌────┴────┐
   │                      yes        no
   │                       │      ENDED(offline)
   │                       ▼
   │                    RINGING ◀──── INCOMING (recipient side)
   │                       │
   │              ┌────────┴────────┐
   │           ACCEPT            DECLINE
   │              │                 │
   │              ▼              ENDED
   │           ACTIVE
   │              │
   │       ┌──────┴──────┐
   │    HANGUP(A)     HANGUP(B)
   │       │              │
   └───── ENDED ◀─────────┘
```

### States (u8 enum in CallState payload)

| Value | Name | Description |
|---|---|---|
| 0 | Idle | No call in progress. |
| 1 | Outgoing | Caller is waiting for recipient. |
| 2 | Ringing | Recipient's phone is ringing. |
| 3 | Incoming | Recipient sees incoming call screen. |
| 4 | Active | Both parties connected (signaling only — no voice in M4). |
| 5 | Ended | Call terminated (transitions to Idle after client processes it). |

### Server Call Session

```cpp
struct PhoneCall {
    uint32_t callSessionId = 0;  // unique per call attempt, echoed in CallState
    int callerId = -1;
    int receiverId = -1;
    uint32_t startTime = 0;      // gettime() when Active began
    CallStatus state = CallStatus::Idle;
    uint64_t ringStartMs = 0;
};
```

- `callSessionId`: monotonic counter, incremented each time a new call is
  created. Sent in every CallState message. Client MUST echo it back in
  CALL_ACCEPT/DECLINE/HANGUP. Mismatches are rejected (PhoneError(STALE_CALL)).
- One active call per player (no call waiting in M4).
- Ring timeout: 30 seconds → auto-decline, ENDED sent to both.
- Disconnect during any state → ENDED to the other party.
- Death during call → configurable server policy (default: end call).
- Self-call → PhoneError(SELF_ACTION).
- Busy → PhoneError(BUSY).
- Offline → PhoneError(OFFLINE).

### Call Duration

`startTime` in the CallState payload is the server's `gettime()` value when
the call entered Active. Both clients compute elapsed time locally. The server
sends the time; clients don't need clock sync beyond what SA-MP provides.

---

## 6. Error Codes

Used in the PhoneError response (op 24).

| Code | Name | Description |
|---|---|---|
| 1 | INVALID_NUMBER | Number doesn't exist or is malformed. |
| 2 | OFFLINE | Recipient is not connected. |
| 3 | BUSY | Recipient is in another call. |
| 4 | SELF_ACTION | Tried to call/SMS yourself. |
| 5 | RATE_LIMITED | Too many requests. |
| 6 | NO_PHONE | Player has no phone number assigned. |
| 7 | CONTACT_EXISTS | Contact with this number already saved. |
| 8 | CONTACT_NOT_FOUND | Contact doesn't exist for deletion. |
| 9 | CONTACTS_FULL | Max contacts reached (64). |
| 10 | SMS_TOO_LONG | Message exceeds 120 bytes. |
| 11 | INTERNAL_ERROR | Server-side failure (DB error, etc.). |
| 12 | STALE_CALL | callSessionId does not match current call. |

---

## 7. Security Model

### Authentication

- The server identifies the sender from the RPC 240 handler's `IPlayer& peer`.
- **Identity is the player's authenticated nickname** (from the gamemode's
  auth flow, not from `SkrPlayerData::accountId` — see §11 Identity Audit).
  The Gate stores an `authenticated` flag set by the Pawn native
  `SKRE_PlayerAuthenticated(playerid)`. The nickname comes from
  `IPlayer::getPlayerName()` in the omp SDK.
- The session id in ClientRequest must match the current API session.
- The server looks up the player's phone number from their authenticated
  nickname (via `phone_numbers`) — never from the request payload.
- Client-supplied numbers (recipient for SMS/call, contact number) are
  validated against the phone_numbers table.
- **Phone operations are rejected when the player is not authenticated.**
  The Gate returns PhoneError(NOT_AUTHORIZED) for any step 9 phone request
  from a player whose `authenticated` flag is false.

### Text Validation and Encoding

All string fields are validated with `IsValidText()` on the wire. The
validation is **byte-level**, not character-level:

- Allowed bytes: 0x20–0x7E (ASCII printable, excluding `~` and DEL) and
  0x80–0xFE (high bytes, covering multi-byte UTF-8 continuation and leading
  bytes). Byte 0x7F (DEL) and 0x00–0x1F (control chars) are rejected.
- All length fields and limits are **byte counts**, not character counts.
  A Romanian character like ă (UTF-8: 0xC4 0x83) counts as 2 bytes.
- SMS text: 1–120 **bytes** of validated text. A message using Romanian
  diacritics may contain fewer visible characters than 120.
- Contact names: 1–23 **bytes** of validated text.
- Full UTF-8 sequence validation is not performed at the wire level (the
  SA-MP environment does not guarantee UTF-8). The byte-level filter blocks
  control characters and SA-MP CFont tokens while passing through all
  multi-byte encodings that real clients produce (UTF-8, Windows-1250,
  Windows-1252).

### Rate Limiting

- Client requests: max 10 per second (same window logic as Reply rate limiter).
- SMS send: additional 1-second cooldown between successful sends.
- Call dial: 5-second cooldown between attempts.
- All rate limiters are per-player, server-side.

### Privacy

- A player can only see their own contacts, conversations, and call state.
- Contact list requests return only the authenticated player's contacts.
- SMS history requests return only conversations involving the player's number.
- No player can enumerate another player's contacts or messages.

---

## 8. SMS Persistence and Delivery

### Persistence Acknowledgment vs Delivery

SmsSendResult (op 21) has a `status` field:
- **0 = Stored**: the message is persisted in the database. If the recipient
  is online, they receive SmsReceived immediately. If offline, the message
  waits in the DB for their next PHONE_SYNC + SMS_THREADS fetch.
- **1 = Error**: the message was NOT stored (invalid recipient, self-send,
  rate limit, etc.). The accompanying PhoneError gives the specific reason.

SmsSendResult means "the server accepted and stored your message." It does
NOT mean the recipient has seen it. True delivery status (read receipts) is
out of M4 scope.

### Idempotent Retries

The server tracks the last successful SMS `requestId` per player. If the
client retransmits the same `requestId`:
- If the previous send succeeded: return the same SmsSendResult with the
  original `messageId`, without inserting a duplicate.
- If the previous send failed: re-attempt normally.

The client must use monotonically increasing requestIds. A retransmit uses
the **same** requestId as the original attempt.

---

## 9. Message Flow Examples

### SMS Send (recipient online)

```
Client A                   Server                    Client B
   │                          │                          │
   │  ClientRequest           │                          │
   │  domain=1 op=7           │                          │
   │  SMS_SEND                │                          │
   │  number="1000042"        │                          │
   │  text="Salut!"           │                          │
   │ ──────────────────────▶  │                          │
   │                          │  validate sender         │
   │                          │  resolve accountId→number│
   │                          │  lookup recipient number │
   │                          │  INSERT into phone_messages
   │                          │                          │
   │  Call op=21              │  Call op=22              │
   │  SmsSendResult           │  SmsReceived             │
   │  status=0 (Stored)       │  sender="PlayerA"        │
   │  messageId=4217          │  text="Salut!"           │
   │ ◀─────────────────────── │ ─────────────────────▶   │
```

### Incoming Call

```
Client A                   Server                    Client B
   │                          │                          │
   │  ClientRequest           │                          │
   │  domain=1 op=8           │                          │
   │  CALL_DIAL               │                          │
   │  number="1000042"        │                          │
   │ ──────────────────────▶  │                          │
   │                          │  validate, check busy    │
   │                          │  create PhoneCall        │
   │                          │  callSessionId=17        │
   │                          │                          │
   │  Call op=23              │  Call op=23              │
   │  CallState               │  CallState               │
   │  callSessionId=17        │  callSessionId=17         │
   │  state=OUTGOING          │  state=INCOMING           │
   │  peer="PlayerB"          │  peer="PlayerA"           │
   │ ◀─────────────────────── │ ─────────────────────▶   │
   │                          │                          │
   │                          │  ClientRequest            │
   │                          │  domain=1 op=9            │
   │                          │  CALL_ACCEPT              │
   │                          │  callSessionId=17         │
   │                          │ ◀─────────────────────── │
   │                          │                          │
   │  Call op=23              │  Call op=23              │
   │  CallState               │  CallState               │
   │  callSessionId=17        │  callSessionId=17         │
   │  state=ACTIVE            │  state=ACTIVE             │
   │  startTime=166849200     │  startTime=166849200      │
   │ ◀─────────────────────── │ ─────────────────────▶   │
```

---

## 10. Pagination

All paginated responses share a consistent structure:

```
u32 requestId       — echoes the client's requestId
u8  count           — number of entries in this page (0 is valid)
u8  flags           — bit 0: hasMore (more pages available)
[entry] × count     — the entries
```

All paginated requests carry `u8 page` (0-based). The client increments
`page` and re-requests with a new `requestId` to fetch the next batch.

| Response | Max per page | Worst-case payload | Proof |
|---|---|---|---|
| ContactList (op 17) | **11** | 369 B | 6 + 11 × 33 = 369 |
| SmsThreadList (op 19) | **4** | 350 B | 6 + 4 × 86 = 350 |
| SmsHistory (op 20) | **3** | 399 B | 6 + 3 × 131 = 399 |

---

## 11. Account Identity

### Audit result: `accountId` is always 0 — use authenticated nickname

**Verified 2026-10-24:** `SkrPlayerData::accountId` (uint32_t) is initialized
to 0, reset to 0 on disconnect, and set only by `AuthorizePlayer()` — which
has **zero callers** in the omp gate. The method exists as scaffolding from the
old SA-MP gate (`server/skr-gate/gate.cpp` had `OnRedeemResult` wiring), but
the omp port never integrated the auth service (`services/auth/`). No Pawn
native wraps `AuthorizePlayer`. **`accountId` is not a usable identity.**

**The authoritative identity is the player's SA-MP nickname:**

- `skr_auth.inc` stores `GetPlayerName()` in `g_authName[playerid]` and uses
  it as the account lookup key. After successful login/register,
  `OnSKRAuthSuccess(playerid, username[])` fires.
- The bank uses `nick TEXT PRIMARY KEY` in `accounts`. Transactions use `nick`.
- There is no numeric account ID in the gamemode layer.

**Decision: phone tables use authenticated nickname (TEXT), not accountId.**

- `phone_numbers.nick TEXT PRIMARY KEY` — maps nickname to phone number.
- `phone_contacts.owner TEXT` — keyed by owner's phone number (unchanged;
  the phone number is tied to `nick` via `phone_numbers`).
- `phone_messages.sender/recipient TEXT` — keyed by phone number (unchanged).

**Nickname rename risk:** SA-MP nicknames can be changed (rare, admin-only).
A rename would orphan the phone number. This matches the existing bank
behavior (bank balance is also keyed by nickname). A future `accountId`
integration would require a migration (`ALTER TABLE ... ADD COLUMN` + backfill).
This is acceptable for MVP: the rename problem is pre-existing and affects all
server data, not just phone.

**Authentication gate:** The Gate requires `SkrPlayerData::authenticated == true`
before accepting any phone request. This flag is set by the new Pawn native
`SKRE_PlayerAuthenticated(playerid)`, called by the gamemode after
`OnSKRAuthSuccess`. If the flag is false, the phone returns
PhoneError(NOT_AUTHORIZED) and the phone UI shows "Unavailable".

If `accountId` is later wired (e.g., launcher ticket auth), the migration
path is: add `account_id INTEGER` column to `phone_numbers`, backfill from
the nickname→account mapping, then switch the lookup. No protocol or codec
changes needed — identity is resolved server-side only.

---

## 12. Gate↔Pawn Communication

### How the Gate knows phone support is available

The Pawn script calls a native during initialization:

```pawn
native SKRE_PhoneEnable();
```

This sets a flag in the Gate component. When a new client sends Hello with
`CapPhone`, the Gate checks this flag. If set, Welcome includes `CapPhone`
in the negotiated capabilities. If not set, `CapPhone` is masked out.

### Request flow

1. Client sends step 9 (ClientRequest) with domain=Phone.
2. Gate's `OnApiMessage` routes it to `ApiOnClientRequest`.
3. `ApiOnClientRequest` validates the session, rate-limits, decodes the
   domain and op, and dispatches to the phone request handler.
4. The phone handler calls a Pawn callback with the operation details:

```pawn
forward OnSKREPhoneRequest(playerid, requestId, op, const payload[], payloadLen);
```

5. The Pawn callback performs SQLite operations (`db_query`, etc.) and calls
   a native to send the response:

```pawn
native SKRE_PhoneSendResponse(playerid, op, requestId, const data[], dataLen);
```

6. The Gate encodes the response as a Call (step 7) with the appropriate
   phone op and sends it to the client.

### Alternative: Gate-side SQLite

If the Gate opens `skr.db` directly (via SQLite C API), the Pawn round-trip
is eliminated. The Gate handles the full phone request lifecycle in C++.
Pawn callbacks become notification-only (for scripts that want to react to
phone events, e.g., logging). This is the preferred approach if the Gate
can safely share `skr.db` with the Pawn-side bank code (both run on the
main thread, no concurrent access issues).

---

## 13. CEF Coexistence and Migration

### M4 scope: contacts, SMS, calls only

The native phone in M4 replaces **only** the Contacts, Messages, and Phone
(calls) features of the CEF phone. The following CEF phone features are
**NOT replaced** in M4:

- **Bank** (deposit, withdraw, transfer, balance)
- **Vehicles** (locate, lock/unlock)
- **Services** (taxi, mechanic, medic request)
- **Camera** (screenshot with phone UI)

### Current state (as of M2 hotfix)

The M2 hotfix disabled the CEF phone L-key for Enhanced clients:
```pawn
CEF_EnableKey(playerid, SKR_PHONE_KEY, false);  // in OnCefReady
```

This means Enhanced clients currently have **no access** to bank, vehicles,
services, or camera via the phone UI. These features remain accessible via:
- Chat commands (if the gamemode provides them)
- Other CEF interfaces (if registered on different keys/browsers)

### Migration plan

The native phone UI should present bank/vehicles/services/camera as app
icons that are marked "Coming soon" or that trigger the relevant CEF
browser/event when tapped (requires CEF interop, which is M5+ scope).

For M4, the spec acknowledges this gap. The Pawn script may re-enable
specific CEF phone events for Enhanced clients alongside the native phone
if the gamemode requires bank/vehicle/service access during the transition.
The L-key conflict is resolved by the native phone taking input priority.

---

## 14. Runtime Status Corrections

### M3 (Character animation)

M3 is **unit-tested only** (116/116 checks). It has **NOT been verified in
GTA runtime**. The phone body task, shadow adjustments, and GTA
CTaskComplexUseMobilePhone integration are implemented and compiled but
require live testing with the actual game client.

### GPS integration

The GPS engine (`client/gps/gps_engine.h`) is implemented, compiled, and
unit-tested. It ships **DISABLED** (`NavStatus::Disabled`) and has **NOT been
verified in GTA runtime**. The phone GPS app reads from `gps::GetStatus()`
and `gps::GetRoute()` — no protocol is needed for this client-side
integration, but it depends on GPS being enabled and working in GTA first.

---

## 15. Backward Compatibility

### Old client + new server (no CapPhone)

- Client Hello does not include `CapPhone`.
- Server Welcome does not grant `CapPhone`.
- No phone ops are sent or accepted.
- CEF phone works as before.

### New client + old server (no CapPhone in Welcome)

- Client advertises `CapPhone` in Hello.
- Old server masks unknown bits: Welcome has `CapPhone = 0`.
- Client marks phone as unavailable → shows "Phone unavailable" in native UI.

### New client + new server, no phone gamemode

- Pawn script does not call `SKRE_PhoneEnable()`.
- Server Welcome does not grant `CapPhone` (flag not set).
- Same as old server case.

### Step 9 and old servers

An old server's `OnApiMessage` reads the step byte and hits the `default`
branch → `ApiInvalid`. But in practice, the client only sends step 9 after
receiving `CapPhone` in Welcome, which an old server never grants. The path
is never reached.

---

## 16. Implementation Checklist

### shared/protocol/

- [x] `Step::ClientRequest = 9`, `IsApiStep` updated
- [x] `CapPhone = 0x08`, `kKnownCapabilities` updated
- [x] Phone ops 16–24 in Op enum, `IsCoreOp`/`IsPhoneOp`/`IsKnownOp`
- [x] ClientRequest struct + encode/decode
- [ ] Phone payload codec (`phone_protocol.h`) — **this revision**
- [ ] Unit tests for all phone codecs

### server/skr-gate-omp/

- [ ] Route step 9 → `ApiOnClientRequest` (buf size: `kMaxInboundSize`)
- [ ] Phone request handler with Pawn callback or direct SQLite
- [ ] PhoneCall state machine with `callSessionId`
- [ ] SMS idempotency tracking (last requestId + messageId per player)
- [ ] Rate limiters for SMS send and call dial
- [ ] `SKRE_PhoneEnable()` native + capability flag
- [ ] Pawn callbacks / natives per §12

### client/phone/

- [ ] Phone adapter: ClientRequest step 9 + phone Call ops
- [ ] Request id management, pending request tracking, timeout
- [ ] callSessionId tracking for call actions
- [ ] GPS integration (read-only, client-side)

### Tests

- [ ] Codec unit tests for all phone payloads + boundary sizes
- [ ] Phone number validation tests
- [ ] Call state machine tests (including stale callSessionId rejection)
- [ ] SMS idempotency test
