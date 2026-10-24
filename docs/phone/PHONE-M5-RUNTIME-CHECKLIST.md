# PHONE M5 — operator-controlled GTA checks

**Not executed.** This procedure does not authorize publishing, signing, changing the production database or replacing certified OP215 binaries. The operator starts GTA manually.

## Prepare a private test installation

- Use a copy of the runtime and a private server/database. Preserve OP215 and the active VEHICLES installation.
- Record hashes of GTA, SA-MP/open.mp, test ASI/DLL and server component; compare candidate hashes in `PHONE-M5-HASHES.json`. Verify that the client/server bitness matches the intended host (candidate GTA module and Gate build here are Win32).
- Resolve the UNKNOWN release dependency pins separately before production. Retain the required account authentication and client admission checks. Do not enable mock SKRAC in production or disable TLS, signatures or sandboxing.
- With all users of the test database stopped, run the migration tool's default dry run. Apply only to the private DB after retaining its full backup. Confirm old numbers, contacts and messages match the backup.
- Use two distinct, persistent test account names A and B that are never reused for other accounts. Create both through the real account workflow. Ensure the gamemode enables phone and authenticates only after successful account login.
- Operator installs candidates into the private tree and starts server/GTA manually. These candidates include the shared working tree's VEHICLES state and must be tested together with that state.

## Test matrix

For each row record PASS/FAIL/NOT RUN, exact binary hashes, account identities (no credentials), observations and logs. A crash, foreign account data, gameplay lock, duplicated allocation or historical data loss stops certification.

| # | Operator action | Expected evidence |
|---|---|---|
| 1 | Log in with new A; press L. Continue Hello and English, acknowledge number, choose both wallpapers, finish. Rapid-click Continue during a request. | Server assigns one canonical 555 number. UI displays `555 ####`. At most one save in flight; setup advances only after acknowledgement. DB has one number/profile for A, stage 4 after completion. |
| 2 | On a separate new account, close/disconnect at each stage, restart launcher, reconnect. Repeat with operator-controlled process interruption after an acknowledged stage. Then reconnect A. | Incomplete setup resumes at the last committed stage and retains number/wallpaper. Completed A enters Home with no onboarding. Unacknowledged saves may have committed; profile sync reconciles them. |
| 3 | Activate B concurrently with another test account; reconnect A and B. Query the private DB read-only. | Distinct numbers in the inclusive pool, stable ownership, no duplicate rows. Legacy owners retain exact numbers. A repeated same-account request returns its existing number. |
| 4 | From Messages on A send B text directly through the native UI; reply from B while A keeps the conversation open. Send several messages in the same second. | No `/sms` needed. Sending becomes Sent only after server storage acknowledgement. Incoming text appears without reopening; no duplicates and message-ID order is stable. Opening history while a push arrives does not lose it. |
| 5 | Add B as A's contact; reconnect A; open contact and Message; remove the contact; reconnect again. Force a controlled DB write failure on the private copy. | Contact persists/removes for A only; messages remain. Write failure does not report a successful save. Duplicate add remains idempotent; no contact rename is promised. |
| 6 | Open GPS → Open map / set waypoint. Set a reachable waypoint on the existing map, return to gameplay and reopen phone. Move to trigger recalculation, then arrive. Repeat with an unsupported/off-road destination and Clear route. | Existing route/minimap/status is reused. Remaining distance appears only while navigating; no invented ETA. Clear removes the actual waypoint and route. Unavailable route is explicit; arrival cleanup follows existing engine. No phone/cursor left over pause. |
| 7 | Close phone, receive SMS, reopen Messages, read it. Reconnect after receiving another unread SMS. Toggle banners off/on. Repeat during ESC, another native dialog and chat input. | Unread persists across reconnect and clears for displayed incoming IDs. Duplicate pushes do not increment twice. Banner preference is honored. Toasts/banners do not steal keyboard focus or require closing other overlays. Record any overlay overlap as a failure. |
| 8 | Change size, wallpaper and banners; disconnect and restart launcher. Switch to a different persistent account on the same installation. | Preferences return only after the correct server profile; another identity does not inherit them. L remains the existing fixed binding. No fake sound/vibration controls. |
| 9 | Test L, ESC, home/back, T, F6, typing `l`, alt-tab, death/respawn, driving, resolution changes and device reset. Repeat at 1024×768, 1280×720, 1360×768, 1920×1080 and ultrawide. | Existing focus policy holds; chat text is not interpreted as phone shortcuts; gameplay controls return on close/service failure; no phone over pause; HUD/GPS/ESC remain intact. Render previews alone do not establish this. |
| 10 | Connect an old M4 client to the new private server; new client to a server without CapPhoneSetup; then to one without CapPhone. Repeat migration with an existing activated account. | No M5 calls without negotiation. M4 direct SMS remains available where CapPhone exists; preferences explain unavailable M5 support. No phone support produces safe unavailable UI. Legacy activated owner never gets silently renumbered or reset. |

## Additional failure checks

- In a disposable DB only, fill the 555 pool. A new account receives unavailable service and can close the phone immediately; existing owners still sync. Do not fill or delete production ownership rows.
- Hold a competing SQLite write lock on the test copy: the game/server must not wait indefinitely. Release it and retry setup/profile; no second number is created.
- Capture/replay a same-session SMS request in a controlled protocol harness: one stored message and the same message ID in acknowledgements, one push. A new-session send is intentionally a new request under the unchanged M4 contract.
- Submit malformed/stale-session/unnegotiated requests only through an operator-approved test harness. Confirm no foreign identity/profile changes and no server deadlock. Offline codec/SQL tests are not proof of real server dispatch behavior.
- Restore the full matching backup and previous test binary together, then verify legacy numbers, SMS and contacts. Preserve a safety backup of post-migration writes first.

## Release decision

Record all ten rows and failure checks. Any unrun row remains NOT RUN. Current verdict is **NO-GO for production** until runtime evidence and the identity/dependency blockers in `PHONE-M5-REPORT.md` are resolved. Voice/dialer, a second translation, notification center and unrestricted archive browsing remain outside the implemented feature set; do not advertise them as complete.
