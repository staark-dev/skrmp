# PHONE M5 — implementation, audit and controlled certification

Date: 2026-10-24. **NO-GO for production certification; candidate for operator-controlled GTA tests.**

No GTA process was launched. No live database, runtime installation, certified OP215 output, signing key or VEHICLES source was changed by this work. Builds live under `out/phone-m5-client` and `out/phone-m5-server`. Other work was already present and was changing during this session; these binaries include that shared working-tree state, not an isolated certified release.

## Audit and resulting app status

The audit read actual code in `client/phone`, `client/gps`, `shared/protocol`, `server/skr-gate-omp/phone_backend.cpp` and native API dispatch. Earlier M4 documents describe intended contracts and are not proof of current implementation.

| App / feature | Before M5 | Result and remaining limitations |
|---|---|---|
| First activation | Missing | Implemented server-backed welcome → language → assigned number → wallpaper → Home. English is the only supported language; there is no fictional second translation. Existing owners migrate as activated. Shell slide, breathing greeting and stage dots use native drawing. |
| Phone / dialer | Functional but incomplete: own number / copy; calls unavailable | Own number and copy preserved. **Voice/dialer remains missing**, explicitly stated in UI; reserved call actions are not shown as working calls. |
| Messages | Implemented direct UI send, server storage and history | Preserved transport and UI. Added durable same-session send receipts, persistent unread watermarks, duplicate-push suppression, history/live-push merging, stable message-id order and insert-failure handling. Removed automatic deletion after 100 messages per pair. |
| Contacts | Implemented list/add/remove, SQLite ownership | Preserved. Formatted numbers accepted by network actions. SQLite failures no longer acknowledge successful save/remove. Existing duplicate-add behavior is idempotent and does not rename contacts. |
| GPS | Functional but incomplete UI; destination select and clear were local view changes | Existing engine status, actual remaining route distance, failure and destination are displayed. Open map transfers ownership to the existing native map; map waypoint starts the existing engine. Clear removes the actual map waypoint, then resets the engine. No invented POIs, ETA or instructions. GTA behavior is unverified. |
| Settings | Placeholder display; click handler and persistence absent | Size, wallpaper and banners persist on the authenticated identity, applied after server acknowledgement. Nonfunctional vibration/sound switches removed. Key binding remains the existing fixed L key, displayed as information. |
| Notifications | Live SMS banners/toasts, session-only unread counts | Persistent unread state added; duplicate pushes do not duplicate banners/unread within the bounded recent-ID cache. Banner preference applies both inside and outside phone. **No standalone notification-center app or durable notification archive.** Overlay coexistence still needs GTA testing. |
| Bank / vehicles / services / camera | Outside native phone registry | No new icons or fake functionality added. Existing CEF/gamemode features and VEHICLES workstream are unchanged by this work. |

## File-by-file implementation map

All entries below are implemented source, not pseudocode or purported upstream APIs.

| File | Change / purpose |
|---|---|
| `shared/protocol/native_api.h` | Negotiate `CapPhoneSetup`; typed M5 response ops without changing M4 payloads/RPC IDs. |
| `shared/protocol/phone_profile.h` | Exact-length validated profile codec. |
| `shared/protocol/phone_protocol.h` | M5 client op recognition; canonical/display number helpers and space normalization; retain valid legacy numbers. |
| `server/skr-gate-omp/phone_m5_sql.h` | SQL shared by the SDK adapter and real SQLite tests: pool, profiles, guarded updates, reads and receipt schema. |
| `server/skr-gate-omp/phone_backend.cpp` | Checked atomic migration, serialized allocation, identity-scoped profile API, persistent reads/receipts, SMS ordering/storage corrections and contact failure handling. Removed a recursive mutex acquisition on invalid-session requests. |
| `server/skr-gate-omp/native_api_server.cpp` | Only grant setup when Phone is also granted/enabled. |
| `client/native-api/native_api.cpp` | Offer capability and pass negotiated setup support to the existing phone adapter. |
| `client/phone/phone_model.h` | Authoritative profile state, loaded/pending flags. |
| `client/phone/phone_net.{h,cpp}` | Tracked profile requests/timeouts, acknowledged settings, read acknowledgement/retry, canonical UI entry points, SMS merge/dedup. |
| `client/phone/phone_input.cpp` | Setup/settings actions, read existing GPS, transfer to existing map and clear waypoint. Existing input ownership remains in use. |
| `client/phone/phone_renderer.{h,cpp}` | Native setup, real settings actions, canonical number formatting, actual GPS controls/status and no fictional destination list. |
| `tools/phone_m5_migrate.py` | Default dry run; explicit apply with full SQLite backup; explicit backup restore. |
| `tests/test_phone_m5_sqlite.py` | Real concurrent SQLite connections and migration/rollback/operator-tool tests using production SQL. |
| `tests/unit/test_phone_m5.cpp` | Actual production phone network adapter with transport/UI test doubles; no GTA memory access. |
| `tests/unit/test_phone_protocol.cpp`, `test_phone_backend.cpp` | Update range assertions for the separately negotiated extension. |
| `tests/phone_preview.cpp` | M5 setup/settings/GPS scenes, optional solid backdrop. Sample data remains tagged PREVIEW. |
| `server/skr-gate-omp/CMakeLists.txt` | M5 test target and shared-header paths for existing phone/preview tests. |

## Identity, provisioning and migration policy

The persistent identity is the **existing server-authenticated account name `authName`**, stored in `phone_numbers.nick`, not transient playerid, launcher state or a client-supplied owner. The trusted gamemode calls `SKRE_PlayerAuthenticated` after account login. This is not a new immutable account UUID or a character-slot model. Production requires the operator to guarantee this name is persistent, unique and not reused after deletion. Rename/character-slot integration is not implemented and must not be assumed.

New allocations are canonical text `5550000` through `5559999`, exactly 10,000 candidates. Display uses `555 ####`. Seven-digit legacy identities remain routable; they are **not silently converted**. A preexisting number inside the 555 pool reserves that candidate. `UNIQUE(number)` and the primary-key owner constraint remain enforced. Clients never send their own desired allocation.

The allocator obtains `BEGIN IMMEDIATE`, rechecks ownership after obtaining the SQLite write lock, inserts the lowest free candidate and its incomplete profile, then commits. Any failure rolls back both records. SQLite lock waiting is bounded to 100 ms in the production connection; callers get an unavailable/error response and can retry. Exhaustion does not create a profile/number or disturb existing owners. Python contention tests use a longer connection timeout to demonstrate serialized allocation; they are not a measurement of open.mp main-thread latency.

Startup migration is additive and transactional:

1. Check creation of existing phone schema/indexes instead of ignoring SQL failures.
2. Create/fill `phone_number_pool`, create `phone_profiles`, `phone_reads`, `phone_sms_receipts`.
3. Grandfather existing owners lacking a profile with stage 4 (activated). Existing incomplete profiles remain incomplete on every later startup.
4. Leave `phone_numbers`, `phone_contacts`, `phone_messages` identities/history and legacy `phone_meta.next_number` untouched. The old counter is no longer used by the M5 allocator.

Stages are 0 welcome, 1 English selection, 2 assigned-number acknowledgement, 3 personalization, 4 complete. Server updates allow the same stage or one forward stage; skipping or resetting completed activation does not change the row. Number provisioning occurs through existing PhoneSync before the assigned-number screen, not a client choice. Disconnect/crash does not undo committed stages. A lost acknowledgement is recovered by profile sync. ESC/L/chat can close the phone; no background wait or mandatory gameplay lock is introduced.

**Deletion policy:** retain number/profile ownership as a tombstone indefinitely; do not recycle numbers or authenticated owner names. There is no automatic character/account deletion hook in this backend. Operators must not delete/reuse these ownership keys or advertise automatic number release. A future immutable-ID/deletion integration needs an explicit migration. This finite retention policy can exhaust the pool, which is handled as unavailable service.

### Operator migration / rollback

Stop the test server and every process using its database. Copy the runtime tree and use a private test database first. Do not migrate a running production service as part of this task.

```powershell
# Inspect and run migration against an in-memory copy; source unchanged.
python tools/phone_m5_migrate.py 'C:\operator-test\scriptfiles\skr.db'

# Explicit apply: creates a timestamped full SQLite backup, then commits migration.
python tools/phone_m5_migrate.py 'C:\operator-test\scriptfiles\skr.db' --apply

# Full restore while all users of the DB are stopped. A safety backup is made first.
python tools/phone_m5_migrate.py 'C:\operator-test\scriptfiles\skr.db' --apply --restore 'C:\operator-test\scriptfiles\skr.db.pre-m5-<timestamp>.sqlite'
```

Rollback means restore the **entire** matching pre-migration backup and previous server binary together. Writes after the backup, including SMS/contacts and new ownership, are absent afterward; take a fresh safety backup before restoring. Do not drop M5 tables or renumber rows to improvise a forward-data-preserving rollback. The utility does not stop processes for the operator.

## Wire extension and compatibility

RPC 240/241, envelope steps, existing op payloads and direct SMS sending remain unchanged. New project-local values are explicitly defined here and in code; they are not upstream open.mp RPCs or callbacks.

| Contract | Value / layout |
|---|---|
| Capability | `CapPhoneSetup = 1 << 5`, only with `CapPhone` |
| C→S ProfileGet | Phone domain client op 12, empty payload |
| C→S ProfileSave | Op 13, profile below; embedded requestId must be zero |
| C→S MarkRead | Op 14, str8 canonical peer (7 bytes), u32 last visible incoming message ID |
| S→C PhoneProfile | Call op 25; u32 requestId, u8 stage, language, wallpaper, scale, banners (9 bytes total) |
| S→C PhoneReadResult | Call op 26; u32 acknowledged requestId (4 bytes) |
| Supported values | stage 0..4; language 0 English; wallpaper 0 image / 1 skyline; size 80..125 percent; banners 0/1 |

M5 handlers verify negotiated capabilities, live API session, quarantine, request rate and server authentication; DB owner comes only from server state. ProfileSave does not carry an identity or phone number. MarkRead is clamped by actual incoming rows and maintains a monotonic watermark. Read failures/timeouts can retry; profile buttons remain bounded while pending and recover after timeout.

Old client/new server: no M5 ops sent; direct M4 phone continues. New client/old server: no setup requests, existing M4 UI and unavailable persistent-preferences notice. No CapPhone: existing unavailable phone behavior. Capability absence does not prove a trusted client or constitute anti-spoof attestation.

SMS receipts deduplicate `(authenticated owner, API session, requestId)` atomically with message insertion, returning the original message ID and avoiding a second push. **There is no reconnect-stable M4 idempotency token.** A new request in a new session is a new message; do not claim exactly-once delivery across arbitrary reconnect retries. Push-ID suppression is bounded to the 128 most recent received IDs per session. Messages stay in SQLite; the client keeps its existing bounded 48-message conversation window and 16-thread list. It is not an unbounded archive viewer.

## Provenance and validation boundaries

Read the master plan, dependency lock and compatibility implementation before editing. Actual local official open.mp source inspected:

- SDK checkout `08cc69a32d96c0eb8fe063d26c78259e54ca853f`, `deps/omp-sdk/include/Server/Components/Databases/databases.hpp`: verified `executeQuery`, result access and `freeResultSet`; no invented prepared-statement API.
- Server checkout `3d20f3dc0e26917264cdc8585c65913e080a143c`, `server-build/open.mp/Server/Components/Databases/database_connection.cpp`: verified SQLite execution failures return null, and non-row statements return result objects.
- GPS integration uses local existing `gps_engine.h`, `gps_route.h`, `gps_destination.h` and `pause_input.h`; no offsets or pathfinder added.

The global dependency lock still contains UNKNOWN server/SDK release fields. This report records inspected source revisions; it does not silently certify that lock or R2 compatibility. Production mock-SKRAC policy remains unchanged. The isolated client build is not a production security certification.

Final commands, counts, hashes and previews are recorded beside this report in `PHONE-M5-VALIDATION.md` and `PHONE-M5-HASHES.json`.

## Remaining release blockers

- No GTA/runtime two-player certification, actual reconnect/crash input test, GPS/map interaction test or notification/modal coexistence test was run.
- Voice/dialer, multiple translations and a notification-center app are not implemented. No unsupported app is presented as working.
- Authenticated-name identity requires the operator's persistent-name/no-reuse contract; immutable account-ID, rename and character deletion integration remain unresolved for installations needing those features.
- Existing UI bounds limit thread/archive browsing; durable receipt/history tables now grow and need an explicit operator retention policy that does not silently discard player history.
- Dependency release provenance and compatibility requirements remain independent blockers. Do not overwrite OP215 or active runtime binaries with these candidate artifacts automatically.
