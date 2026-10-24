# PHONE M5 — offline results

Date: 2026-10-24. No GTA runtime or live-server test was performed.

## Build results

| Target | Result | Evidence |
|---|---|---|
| `SKRClient`, Release Win32 | PASS, complete DLL linked | `out/phone-m5-client/final.log` |
| `SKRGateOMP`, Release Win32 | PASS, complete DLL linked | `out/phone-m5-server/release-candidate-build.log` |
| Phone/GPS test executables | PASS | `out/phone-m5-server/final-build.log`, `m5-final-build.log`, `results.txt` |
| Native D3D9 phone preview | PASS, final build and execution | `out/phone-m5-server/preview-final.log` |

Both candidate DLL PE headers report machine `0x14c` (x86). This does not establish compatibility with an arbitrary server installation or certify any GTA/SA-MP version.

The isolated CMake builds used Visual Studio 18 2026 and `-A Win32`. The server SDK's old dependency policy declarations required the configure argument `'-DCMAKE_POLICY_VERSION_MINIMUM=3.5'` with the installed CMake. No vendored dependency was edited. Existing SDK/VEHICLES compiler warnings remain. A transient compilation failure in the independently changing VEHICLES source and a preview executable link lock occurred during intermediate builds; both are resolved in the final successful build logs. No VEHICLES source correction was made by the phone work.

Reproduction from repository root:

```powershell
cmake -S . -B out/phone-m5-client -G 'Visual Studio 18 2026' -A Win32 '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
cmake --build out/phone-m5-client --config Release --target SKRClient
cmake -S server/skr-gate-omp -B out/phone-m5-server -G 'Visual Studio 18 2026' -A Win32 '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
cmake --build out/phone-m5-server --config Release --target SKRGateOMP SKRPhoneM5Tests SKRPhoneTests SKRPhoneM2Tests SKRPhoneM3Tests SKRPhoneM4FTests SKRPhoneProtocolTests SKRPhoneBackendTests SKRNativeApiTests SKRGpsTests SKRGpsNavigationTests SKRGpsRoutingTests SKRPhonePreview
python tests/test_phone_m5_sqlite.py
```

Run each named test executable from `out/phone-m5-server/Release`. Avoid running the preview executable while rebuilding it.

## Executed C++ checks

All listed executables exited 0; **1,891 checks passed** in total.

| Suite | Passed |
|---|---:|
| Phone M5 actual network adapter with test doubles | 49 |
| Phone layout/controller/model | 648 |
| Phone M2 input policy | 241 |
| Phone M3 body coordinator with mock backend | 116 |
| Phone M4F text and navigation | 59 |
| Phone protocol | 134 |
| Phone backend contract tests | 48 |
| Native API | 70 |
| GPS basic | 23 |
| GPS navigation | 443 |
| GPS routing | 60 |

The existing `SKRPhoneBackendTests` suite checks codecs/contracts, not a running SDK database component. The new network test links actual `phone_net.cpp`, replacing the transport, model access and notification endpoints with explicit test doubles. It covers authoritative profile loading, malformed/truncated/stale profiles, no optimistic setup advance, double-click suppression, settings acknowledgement, timeout/retry, M4 fallback, recipient normalization, duplicate SMS pushes, history/live-push ordering and queued read state. It does not execute WndProc/GTA memory access.

## Executed real SQLite tests

**10 test cases passed**, including:

- Exactly 10,000 candidates, lower/upper bounds, duplicate constraint, exhausted pool and existing-owner reconnect.
- Eight concurrent connections: 16 same-owner requests return one number; 32 distinct owners receive distinct numbers.
- Interrupted transaction rolls back both number and profile.
- Additive/idempotent migration preserves legacy identity, contacts, SMS and incomplete profile progress.
- Migration failure rolls back; full backup restore restores the matching database contents.
- Server SQL prevents skipping/resetting activation and scopes preferences to one owner.
- Read watermark persists, never regresses and does not mark later unseen messages.
- Receipt uniqueness survives reconnect of the database connection.
- Operator utility's dry run, explicit apply with backup and explicit restore, on disposable fixtures only.

Tests parse SQL constants from `phone_m5_sql.h` and execute them in Python's real SQLite, with a binding adapter for formatted parameters. They do not prove the installed open.mp SQLite runtime version or runtime threading. The temporary fixtures are created under `out/phone-m5-sqlite` and removed after tests. Initial sandbox-temp access failures were resolved by placing fixtures in the writable workspace; the final run is recorded in `out/phone-m5-server/sqlite-results.txt`.

## Visual checks

The final native D3D9/ImGui preview generated **144 scene images with zero render failures**, including 1024×768, 1280×720, 1360×768, 1920×1080 and selected ultrawide scenes. Output: `out/phone-m5-previews`. Sample data remains visibly marked PREVIEW.

Inspected generated crops for personalization, unavailable service at 1024×768, settings at 1920×1080 and GPS idle at 1360×768. Setup, number, controls and explanatory text fit those samples. The first-boot breathing greeting/stage dots were added to the same native renderer and the preview was rebuilt/rerun. These are offscreen UI previews, not GTA screenshots or input/overlay certification.

## Hashes

Full hashes, sizes, PE architecture and the phone source snapshot are in `PHONE-M5-HASHES.json`.

| Candidate | SHA-256 |
|---|---|
| `out/phone-m5-client/Release/SKR.Client.dll` | `0c557d82390afb615aef88987d9f1682bd856c635840518bc498f803bf22c465` |
| `out/phone-m5-server/Release/SKRGate.dll` | `768318ba1a3ec691274f4a8fd3b005f3ec1f41246903f3575213a7d232e614ba` |

These are unsigned local candidates from a shared dirty workspace, not published artifacts or an attestation of client integrity. Certified OP215 output was not replaced.

## What was not run

No GTA launch, two-player session, actual server database adapter integration, runtime crash/reconnect, live migration, production SKRAC validation, signing/publishing, immutable-account rename/deletion test, legacy binary interoperability session or complete compatibility matrix. The controlled procedure is `PHONE-M5-RUNTIME-CHECKLIST.md`.

**Verdict: offline candidate passes the checks above; production NO-GO.** Runtime evidence and the remaining feature/identity/dependency limitations in `PHONE-M5-REPORT.md` are still required.
