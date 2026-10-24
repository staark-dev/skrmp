# SKR-X PHONE v1 — M5.8 implemented contract

This is a project-defined namespace, not an upstream open.mp RPC API. Implementation: `shared/protocol/skrx_phone.h`. No UI, socket, RPC ID, road graph or pathfinder is added.

## Negotiation and one active transport

Native API Hello/Welcome remains version 1 on existing steps 5/6. Capability bits:

| Bit | Meaning |
|---|---|
| `0x08 CapPhone` | Existing phone service / legacy domain support |
| `0x10 CapGateway` | Existing SKR-X step-10 envelope support; alone does not mean PHONE support |
| `0x20 CapPhoneSetup` | Existing activation/profile/read-state operations |
| `0x40 CapPhoneGateway` | Explicit support for **PHONE namespace version 1** |

The server intersects offered capabilities with known support, removes phone capabilities when disabled, removes Setup/GatewayPhone without CapPhone, and removes GatewayPhone without CapGateway. The client rejects an incoherent Welcome. `CapPhoneGateway` is version-specific; future incompatible versions require explicit further negotiation rather than assuming all PHONE versions work.

At accepted Welcome, both peers choose once:

- `CapPhone + CapGateway + CapPhoneGateway`: PHONE v1 Gateway for every phone request, reply and push.
- `CapPhone` without that full combination: unchanged legacy API steps 9/7, including Setup only if separately negotiated.
- No `CapPhone`: phone service unavailable.

No phone request is sent before Welcome. An old server masks the unknown bit and selects legacy. An old client does not offer it and receives legacy responses. Send failure, timeout or unsupported namespace traffic **never switches transport during a session**. Step-9 phone requests are rejected for Gateway sessions; legacy phone Calls cannot mutate a Gateway client's model. A new API session resets the choice and queues. CORE Ping/Pong and VEHICLES retain their own existing negotiation and dispatch.

Server build switch `-DSKR_PHONE_GATEWAY=OFF` removes only the PHONE Gateway capability from Welcome. After operator-controlled server restart/reconnect, clients use legacy phone transport with the same asynchronous backend/database. It does not turn off CORE/VEHICLES or silently switch an established session.

## Exact registry ownership

No additional registry entry is registered for PHONE, because the key is `(rpcId, step, direction)`, not namespace.

| Registry surface | Existing key used |
|---|---|
| Server inbound | `(240,10,Inbound)` |
| Server outbound | `(241,10,Outbound)` |
| Client outbound | `(240,10,Outbound)` |
| Client inbound | `(241,10,Inbound)` |

The current Gateway dispatch adds one PHONE case inside those handlers. Steps 0–9 and RPC 242/243 retain their layouts. The server now rejects non-byte-aligned/oversized Gateway packets before copying; valid CORE/VEHICLES frames retain the existing 400-byte payload limit.

## Frame layout

All integers are little-endian; length is exact. Strings inside reused codecs are byte-counted str8 values. Existing Gateway header:

```text
u8  0xE0
u8  10
u8  namespace = 5 (PHONE)
u8  version = 1
u16 payloadLength <= 400
... namespace payload
```

PHONE request payload:

```text
u8  operation
u32 API sessionId (nonzero, must match live server state)
u32 requestId (nonzero for client traffic)
... existing typed request body, <= 200 bytes
```

PHONE response/push payload:

```text
u8  operation
u32 API sessionId (nonzero, must match current client session)
... existing typed response body
```

Response bodies already carry their original request ID or durable message ID; no redundant API Call envelope/call ID is nested. ProfileSave retains its existing codec's zero requestId field; the real correlation ID is the request header. Server-initiated PhoneSync retains body requestId 0.

The worst request budget is `6 + 9 + 200 = 215` bytes, within the existing client RakNet helper's 255-byte bound. The response namespace header consumes 5 of the Gateway's 400 payload bytes. PHONE v1 uses **two messages per history page**, since a legacy worst-case three-message page would overflow. The legacy backend still uses three. Pagination fields/codecs and bounded UI merge behavior are reused; no truncation or silent oversize packet is sent.

## Implemented operation registry

Operation numbers intentionally match the existing phone domain. Every listed operation is validated with its actual typed codec before processing. Reserved call operations 8–11 and response 23 are **unsupported** in PHONE v1; no working calls are advertised.

| Direction | Op | Function / reused body |
|---|---:|---|
| C→S | 1 | PhoneSync / PHONE_READY: empty body, request authoritative number/state |
| C→S | 2 | ContactList / CONTACT_SYNC: page |
| C→S | 3 | ContactAdd / CONTACT_UPDATE: canonical number and display name |
| C→S | 4 | ContactRemove / CONTACT_UPDATE: canonical number |
| C→S | 5 | SmsThreads / CONVERSATION_SYNC: page |
| C→S | 6 | SmsHistory / CONVERSATION_SYNC: canonical peer and page |
| C→S | 7 | SMS_SEND: canonical recipient and bounded text |
| C→S | 12 | ProfileGet / PHONE_STATE: empty; requires Setup |
| C→S | 13 | ProfileSave / PHONE_ACTIVATE: existing stage/preferences; requires Setup |
| C→S | 14 | MarkRead: peer and last visible incoming ID; requires Setup |
| S→C | 16 | PhoneSync / PHONE_STATE: authoritative existing/provisioned number |
| S→C | 17 | ContactList: existing paginated contacts |
| S→C | 18 | ContactResult: request ID and actual save/remove outcome |
| S→C | 19 | SmsThreadList: persisted summaries/unread state |
| S→C | 20 | SmsHistory: existing history codec, maximum two entries |
| S→C | 21 | SMS_ACK: request ID, storage outcome and durable message ID |
| S→C | 22 | SMS_RECEIVED / PHONE_NOTIFICATION: sender, bounded text, timestamp and durable message ID |
| S→C | 24 | PHONE_ERROR: existing bounded error codec |
| S→C | 25 | PhoneProfile / PHONE_ACTIVATED: actual authoritative stage/preferences; Setup only |
| S→C | 26 | PhoneReadResult: acknowledged request ID; Setup only |

There is no separate generic notification opcode or duplicate activation engine: existing SmsReceived drives real-time notifications, and existing profiles drive activation. No continuous GPS data is transmitted.

## Server processing and storage

The packet callback checks negotiated transport, current API session, quarantine, phone enabled state, authentication, typed payload and the existing 10 requests/sec limiter. It snapshots authenticated owner, player ID, connection generation, API session and negotiated flags, then submits to a bounded worker queue. No SQLite, disk logging or waiting occurs in the phone packet adapter.

One worker owns one private SQLite connection. No SDK pool/result/player/network objects cross threads. Existing SQL and codecs moved to `phone_store.cpp`; this is the same backend behavior and database, not a parallel phone service. The path remains the exact existing `skr.db` relative to the server working directory. Do not infer a `scriptfiles` prefix: the inspected SDK open implementation passes the original path directly to SQLite.

Limits: 64 queued requests globally, 4 queued per player (plus an executing request), 128 queued responses, 200-byte request bodies, 400-byte response buffers; at most 16 responses are drained per main tick. Database results are capped at 128 rows, 16 columns, 4096 bytes/cell and 64 KiB total. SQLite lock waiting is bounded at 100 ms on the worker. SQLite access from another thread is rejected by the adapter. Initialization/migration runs on the worker as well; enabled-but-unavailable storage returns errors, not successful acknowledgements.

Disconnect, owner/session replacement and disable invalidate cancellation leases. Queued/cancelled work does not acquire a new identity. Query/commit boundaries check the lease; completed transactions are not retroactively undone. Before sending an exact requester response the main thread rechecks authentication, owner, generation, session, capabilities, quarantine and enabled state. Committed SMS pushes resolve the currently authenticated recipient owner; an offline recipient retains the stored message for later history retrieval. The worker never calls an SDK/player function.

Database schema and the 555 allocator are unchanged from M5. Existing numbers, setup, contacts, messages, read watermarks and receipts remain in the same tables. No live DB was modified for this task. M5 migration/rollback tools remain applicable.

## Retries, reconnects and limits

SMS insertion and `(owner, API session, requestId)` receipt creation remain one transaction. Replaying that request returns the original message ID without another row or push. Replaying an old frame after reconnect fails the session check. No automatic resend under a new ID or dual legacy/Gateway send is introduced. Duplicate incoming pushes retain existing bounded message-ID suppression.

This does **not** make a manual new send with a fresh ID after a new login equivalent to an earlier request. M4 has no reconnect-stable client idempotency token; new sends are new messages. Contacts/history and authoritative number provisioning persist normally. Histories retain the existing bounded UI window; no unbounded archive browser is claimed.

The old synchronous development-only `SKRE_TestSmsSend` native now returns failure rather than doing callback SQL or fabricating an immediate database message ID. `SKRE_TestPhoneSync` queues real sync. Production native Messages/Contacts actions are preserved. Calls remain unavailable. Native UI, local GPS and vehicle-pool hooks are unchanged.
