# SKR-X PHONE M5.8 — transport integration report

Date: 2026-10-24. **GO for an operator-controlled, isolated two-client runtime test. NO-GO for production certification.**

Implemented one negotiated PHONE transport per session, reusing the existing native phone, payload codecs and SQLite business logic. No GTA process or live server was launched, no candidate was deployed/signed, and no active/certified OP215 output was overwritten. Builds are isolated under `out/phone-m58-*`.

## Audit: actual architecture and status

Before this migration, `phone_net.cpp` paced native UI requests through RPC 240 / API step 9. `phone_backend.cpp` executed SQLite directly in that callback and returned phone Calls through RPC 241 / step 7. SMS sending already worked through native Messages; it was not a chat-command implementation. M5 already supplied the 555 allocator, profiles, receipts, contacts/history, unread watermarks and local GPS actions.

SKR-X already owned one step-10 handler in each direction with CORE and VEHICLES cases. Namespace 5 was reserved and explicitly excluded from `IsKnownNamespace`. `CapGateway` did not prove PHONE support. Client phone diagnostics also performed synchronous file I/O in callbacks.

| Existing feature | Integration status | Runtime boundary |
|---|---|---|
| Native Messages, send/ack/receive | Existing UI preserved; now Gateway v1 or legacy, never both | Real model/store round trips pass offline; actual GTA/RakNet delivery still unrun |
| Contacts and conversations | Same codecs/schema; add/refresh/persistence and history merge exercised | Full native input and live server adapter remain operator tests |
| Numbers / activation | Existing transactional 555 allocator and staged profiles reused | Concurrent real worker requests pass; no new activation engine or renumbering |
| GPS | Existing local engine/map/waypoint implementation unchanged | No GPS data sent over PHONE; previous GTA verification limits remain |
| Settings | Existing profile operations use selected phone transport | Setup capability still separately required |
| Notifications | Existing SmsReceived drives native banner/toast and unread updates | No fake notification-center app; overlay coexistence unrun |
| Phone / dialer / calls | Existing own-number screen preserved; calls unsupported | No signaling/voice functionality is claimed |
| CORE / VEHICLES | Existing branches, opcodes and vehicle-pool hooks preserved | Codec/registry/manager regressions pass; vehicle runtime certification is separate |

The app registry, ImGui/D3D9 drawing, wallpaper, controller, input ownership, GPS calculations, native HUD and ESC UI were not redesigned or replaced. There is no CEF/HTML phone integration or new socket.

## Implemented file changes

| File | Purpose |
|---|---|
| `shared/protocol/skrx_phone.h` | New PHONE-v1 frame, typed bounds/validation, capability normalization and transport selection. Reuses phone bodies; two history entries per Gateway page. |
| `shared/protocol/native_api.h` | Explicit `CapPhoneGateway = 0x40`; existing capability/op values retained. |
| `shared/protocol/skrx_gateway.h` | Enable existing namespace 5; retain exact Gateway envelope. |
| `client/native-api/native_api.cpp` | Offer/validate explicit support and bind phone transport at Welcome. |
| `client/skrx/skrx_client.cpp` | PHONE branch in existing dispatcher; suppress additional per-message PHONE disk-log traffic. CORE/VEHICLES dispatch retained. |
| `client/phone/phone_net.{h,cpp}` | Existing send queue/model handlers use selected transport. Opposite-path and stale Gateway responses cannot mutate the model. No automatic fallback/dual send. Phone diagnostic file writes removed. |
| `server/skr-gate-omp/phone_backend.cpp` | Main-thread SDK/auth/transport adapter; no SQLite/filesystem/wait calls. Bounded enqueue and response delivery, queued auth sync. |
| `server/skr-gate-omp/phone_store.cpp` | Extracted existing SQL/codecs/business logic into a project-owned store, independent of SDK/player/network objects. |
| `server/skr-gate-omp/phone_worker.{h,cpp}` | Single-owner SQLite connection, bounded queues, cancellation leases, bounded results, initialization and shutdown. |
| `server/skr-gate-omp/skr_gate_component.{hpp,cpp}` | Own worker, drain at most 16 responses per tick, cancel on disconnect/disable; reject non-byte-aligned/oversized Gateway input. No vehicle-pool hook changes. |
| `server/skr-gate-omp/native_api_server.cpp` | Normalize negotiated capabilities; server rollout switch only removes PHONE Gateway support. |
| `server/skr-gate-omp/skrx_gateway_server.cpp` | PHONE case inside existing handler; no duplicate RPC registration. |
| `server/skr-gate-omp/CMakeLists.txt` | Exact existing SQLite 3.36.0 dependency, thread support, separate tests, `SKR_PHONE_GATEWAY` option. |
| `dependencies.lock.json` | Record consumed SQLite source ID, header/library/license hashes; retain unresolved preexisting release fields. |
| `tests/unit/test_phone_gateway.cpp` | Real phone client adapter + real worker/store with explicit transport/SDK test doubles, protocol/registry/negotiation and persistence cases. |
| `tests/unit/test_phone_worker.cpp` | Actual linked SQLite revision/thread ownership, concurrent activations, cancellation/disable, migration and retained legacy identity. |
| `tests/unit/test_skrx.cpp` | Update namespace-5 assertion from reserved to implemented. |

Detailed opcode registry and wire layouts: `PHONE-M5.8-PROTOCOL.md`. The implementation plan/audit is retained in `PHONE-M5.8-PLAN.md`.

## Threading, trust and reliability

The official SDK database component uses a shared result pool and optional query logging; it is not a verified worker-thread API. Its connection/result objects are **not** used off-thread. The extracted store uses the already available, verified SQLite C API on one private worker-owned connection. This avoids core open.mp modification while keeping SQL identical in purpose and schema.

Ingress validates current capability combination, selected transport, live API session, authentication, quarantine, phone enabled state and the existing 10 requests/sec rate limiter. Identity always comes from server authentication, not the message body. A request snapshots player ID, connection generation, session, persistent authenticated name and cancellation lease. The worker never calls an SDK/player/network function. Main-thread delivery rechecks current identity/session and capabilities before sending a requester response; committed SMS pushes resolve the current authenticated recipient owner.

SQLite open/migration/queries/lock waiting happen on the worker. Queues are bounded to 64 pending requests, 4 queued per player plus an executing request, and 128 responses. Main-thread operations do not hold the queue lock during SQLite execution and never wait on a condition variable. Worker shutdown may join only during component teardown. Normal phone traffic introduces no phone log file writes or SMS-content logging. Existing general CORE/API diagnostics remain unchanged; this is not a claim that the entire application has no file I/O.

Same-session SMS replay returns its existing durable receipt/message ID, without a second insert or push. Old-session frames are ignored. Failed sends never switch transport or resend under a new ID automatically. A manual new request with a new ID/login remains a new SMS under the existing M4 contract; exactly-once semantics across arbitrary new-session retries are not claimed.

The two old development-only native helpers need explicit attention: `SKRE_TestPhoneSync` now queues actual sync and reports queue acceptance. **`SKRE_TestSmsSend` now returns failure**, because its synchronous database-message-ID return contract cannot safely be preserved in a callback after asynchronous extraction. It does not claim a send succeeded. Native Messages is the preserved functional send path; scripts using the old test command must use the UI for certification.

## Database and rollback status

**No M5.8 schema/data migration is required on an existing M5 database.** Existing numbers, contacts, messages, profiles, reads and receipts are reused. Startup still performs the existing additive/idempotent M5 schema initialization on the worker. The 10,000 candidates remain `5550000`–`5559999`; ownership stays server authoritative and UNIQUE/transactional. Legacy numbers remain unchanged.

The database path remains the exact original Gate path `skr.db`, relative to the server working directory. The inspected SDK implementation passes that path directly to SQLite; it does not prepend `scriptfiles`. Operators must identify and back up the actual existing file rather than accidentally start from a different working directory. No live file was opened by this task's tests; fixtures live under `out/phone-m58-tests` and the existing SQLite test scratch directory.

Transport rollback is independent of data rollback: build with `SKR_PHONE_GATEWAY=OFF`, restart the private server under operator control and reconnect. Welcome removes only `0x40`, and both directions use legacy phone transport with the same worker/store. CORE/VEHICLES remain enabled as negotiated. An OFF candidate was actually compiled separately at `out/phone-m58-server-legacy/Release/SKRGate.dll`.

Alternatively restore the prior M5 client/server binaries as a pair, retaining the unchanged M5 database. For an upgrade from pre-M5 schema, keep using `tools/phone_m5_migrate.py` default dry run, explicit apply/full backup and matching full-restore procedure documented in `PHONE-M5-REPORT.md`. Do not drop/rewrite phone tables or renumber identities to roll back a transport change.

Inherited identity policy remains authenticated account-name ownership, with no automatic name reuse/renaming/character-deletion integration. Existing M5 tombstone/no-reuse requirements and finite number-pool exhaustion handling still apply.

## Provenance and build setup

Read the master plan, lockfile and compatibility profile before edits. Inspected official local sources:

- open.mp SDK `08cc69a32d96c0eb8fe063d26c78259e54ca853f`, database interfaces/result lifecycle.
- open.mp server `3d20f3dc0e26917264cdc8585c65913e080a143c`, `Server/Components/Databases/database_connection.cpp` and `databases_component.cpp`: direct path semantics, result pool and synchronous SQLite/query logging.
- Existing Conan SQLite package: **3.36.0**, Fossil source ID `5c9a6c06871cb9fe42814af9c039eb6da5427a6ec28f187af7ebfb62eafa66e5`. Linked library version/source ID and thread support were actually checked by the worker suite. Package public-domain license was read and hash-pinned with header/library in the lockfile.

No source/API was attributed to a fictitious upstream callback. New classes/ops are project code. The SDK/server release lock entries still have UNKNOWN fields; neither this build nor hashes certify R2, production SKRAC, arbitrary binaries or a client integrity attestation.

Example reproduction in this workspace (no deployment):

```powershell
$phoneSqliteRoot = 'C:/Users/Staark/.conan2/p/b/sqlit1643f3ee1f75c/p'
cmake -S server/skr-gate-omp -B out/phone-m58-server -G 'Visual Studio 18 2026' -A Win32 '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' "-DSQLite3_INCLUDE_DIR=$phoneSqliteRoot/include" "-DSQLite3_LIBRARY=$phoneSqliteRoot/lib/sqlite3.lib"
cmake --build out/phone-m58-server --config Release --target SKRGateOMP SKRPhoneGatewayTests SKRPhoneWorkerTests
cmake -S . -B out/phone-m58-client -G 'Visual Studio 18 2026' -A Win32 '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
cmake --build out/phone-m58-client --config Release --target SKRClient
```

For the rollback candidate, configure a **different** binary directory with `-DSKR_PHONE_GATEWAY=OFF` and the same matching-architecture SQLite paths. Do not overwrite the Gateway candidate while collecting its hashes. Candidates here are Win32; another server ABI requires its own matching SQLite package/build, not the GTA module's ABI by assumption.

## Executed validation

Client DLL, Gateway-enabled Gate DLL and legacy-transport Gate DLL all compiled and linked successfully. Evidence: `out/phone-m58/final-client-build.log`, `final-server-build.log`, `legacy-build.log`. Existing SDK and VEHICLES compiler warnings remain; no vehicle source was modified to suppress them.

**15 C++ test executables exited 0: 3,174 checks passed.**

| Suite | Checks passed |
|---|---:|
| PHONE Gateway integration/protocol | 558 |
| Phone worker / actual SQLite | 153 |
| Phone layout/controller/model | 648 |
| Phone M2 input policy | 241 |
| Phone M3 body coordinator, mock backend | 116 |
| Phone M4F text/navigation | 59 |
| Phone M5 adapter | 49 |
| Phone protocol | 134 |
| Phone backend contract codecs | 48 |
| Native API | 70 |
| SKR-X CORE/registry | 158 |
| SKR-X VEHICLES | 414 |
| GPS basic | 23 |
| GPS routing | 60 |
| GPS navigation | 443 |

Evidence: `out/phone-m58/results.txt`. **10 existing real SQLite migration/allocation/operator-tool tests also passed**, recorded in `out/phone-m58/sqlite-results.txt`.

The new integration test links actual `phone_net.cpp`, `phone_store.cpp`, `phone_worker.cpp` and the real registry. It substitutes the RakNet/SDK ingress-delivery boundary and UI notification endpoints. It exercises negotiated-only traffic, capability/rollback combinations, exact registry direction, malformed/truncated/versioned packets, activation/preferences, contact persistence, send/ack/receive, duplicate receipts/pushes, live model updates, offline-recipient retrieval, unread reconnect, legacy fallback and failed-send no-dual-path behavior. The worker test uses the linked SQLite 3.36.0 library, 32 concurrent request producers, exact-owner connection checks, cancellation/disable and retained legacy activation.

These are **not** running-open.mp adapter tests or GTA sessions. No claim is made that offline doubles prove real RPC hook order, input responsiveness, FPS, overlay coexistence or vehicle runtime behavior. UI/GPS were deliberately not polished or re-rendered for this transport task.

## Artifacts and decision

SHA-256 hashes, sizes, PE architecture, source snapshot and preservation checks are in `PHONE-M5.8-HASHES.json`. The hashes identify unsigned local candidates from a shared dirty workspace, not published/certified releases. The candidate also includes the current shared VEHICLES state, so runtime testers must use the recorded matching snapshot.

**GO: isolated operator-controlled two-client test**, following `PHONE-M5.8-RUNTIME-CHECKLIST.md`, after the operator verifies server ABI, correct DB path/private backup and matching hashes. No automated launch or install is performed.

**NO-GO: production certification/deployment.** Real GTA/open.mp runtime integration, latency/FPS/input checks, existing compatibility/dependency release requirements, production SKRAC and account-name lifecycle guarantees remain unverified. Calls, immutable account/character migration and a notification-center app remain outside the implemented feature set. The retired synchronous development SMS native is an explicit compatibility limitation, not a working feature.
