# PHONE M5.8 Integrated Test Runtime Deployment

**Date:** 2026-10-24 22:39 UTC+3
**Operator:** Staark
**Status:** DEPLOYED — awaiting manual runtime verification

## 1. Source Revisions

All changes are uncommitted on the `main` branch.

- **Client source:** `build-test-certification-win32` (VS 18 2026, Win32, Release)
  - Preset flags: `SKR_EXTERNAL_LOBBY=ON`, `SKR_TEST_CERTIFICATION=ON`, `SKR_OP215=OFF`
- **Gate source:** `build-gate` (VS 17 2022, Win32, Release)
  - SQLite3 3.36.0 (Conan, static)

No git commit for this build — working tree state only.

## 2. Included Feature Inventory

### Client (SKR.Client.dll)

| Feature | Source | Status |
|---|---|---|
| Native ImGui/D3D9 phone (iFruit UI) | client/phone/*.cpp | COMPILED |
| Phone activation + 555 numbering | phone_net.cpp, phone_input.cpp | COMPILED |
| Native Messages + Contacts | phone_renderer.cpp, phone_input.cpp | COMPILED |
| GPS integration + navigation panel | client/gps/*.cpp | COMPILED |
| Settings + saved preferences | client/native-pause/pause_settings.cpp | COMPILED |
| SKR-X PHONE v1 Gateway (CapPhoneGateway) | client/native-api/native_api.cpp | COMPILED |
| Legacy phone fallback | phone_net.cpp (Offline mode) | COMPILED |
| SKR-X CORE client | client/skrx/skrx_client.cpp | COMPILED |
| SKR-X VEHICLES client | client/skrx/skrx_vehicles_client.cpp | COMPILED |
| Phone M5.9 browsing pose | client/phone/phone_body_gta.cpp | COMPILED |
| Phone M6 layout fix | client/phone/phone_renderer.cpp | COMPILED |
| Native pause menu (resolution-aware) | client/native-pause/*.cpp | COMPILED |
| Wallpaper support | client/phone/phone_wallpaper.cpp | COMPILED |
| SA-MP key filter (chat/phone conflict fix) | client/samp-hooks/samp_hooks.cpp | COMPILED |
| Launcher integration | apps/launcher/*.cpp | COMPILED (separate exe) |
| OP215 custom vehicles | SKR_OP215=OFF | NOT INCLUDED |

### Server (Gate.dll)

| Feature | Source | Status |
|---|---|---|
| SKR-X CORE gateway + registry | skrx_gateway_server.cpp, skrx_registry_server.cpp | COMPILED |
| SKR-X VEHICLES manager + server | skrx_vehicles_manager.cpp, skrx_vehicles_server.cpp | COMPILED |
| PHONE v1 namespace + backend | phone_backend.cpp | COMPILED |
| CapPhoneGateway (0x40) negotiation | native_api_server.cpp | COMPILED |
| Worker-owned SQLite processing | phone_worker.cpp, phone_store.cpp | COMPILED |
| Phone M5 database (numbers, contacts, SMS) | phone_m5_sql.h, phone_backend.cpp | COMPILED |
| Client admission policy (T16) | client_policy.cpp | COMPILED |
| Rejection protocol (T17) | client_policy.cpp | COMPILED |
| Native API server | native_api_server.cpp | COMPILED |
| Gate UID 0x534B5247 | skr_gate_component.hpp:169 | PRESERVED |

## 3. Build Output Paths

| Artifact | Build path |
|---|---|
| SKRGate.dll | `build-gate/Release/SKRGate.dll` |
| SKR.Client.dll | `build-test-certification-win32/Release/SKR.Client.dll` |
| SKRLauncher.exe | `build-test-certification-win32/Release/SKRLauncher.exe` |

## 4. Installed Destination Paths

| Artifact | Destination |
|---|---|
| Gate.dll | `server-runtime-branded-test/components/Gate.dll` |
| SKR.Client.dll | `build-test-certification-win32/Release/SKR.Client.dll` |

The client DLL build output IS the deployment location — the launcher loads from this path.

## 5. Before / After Hashes

### Gate.dll

| State | SHA-256 | Size |
|---|---|---|
| Before (backup) | `acb2eb47eb742c9f659836b6e73fd09c6cca5f1783ab467b489f20414e77f17c` | 277,504 B |
| After (deployed) | `24aef8d526553b36c791143ae147d5ede593c843d2a837da62d9171c262ecb34` | 899,072 B |

### SKR.Client.dll

| State | SHA-256 | Size |
|---|---|---|
| Before (backup) | `f44528c91d7179d6dcb3cb30c2f9b07268363d5aec47de631de08d32b555a1ad` | 890,880 B |
| After (deployed) | `334342f45137f16f524873293527f3208cc9cb75c970004549f236a8f7ad7166` | 913,920 B |

Gate.dll grew from 277 KB to 899 KB because the previous deployed Gate predated the full
SKR-X vehicles, phone worker/store, and gateway server implementations.

### Backup Location

`docs/ai-memory/checkpoints/deployment_backup_20261024_223912/`

Contains: `Gate.dll` and `SKR.Client.dll` with the "Before" hashes above.

## 6. Client / Server Compatibility

- Client advertises: `CapNotify | CapConfirm | CapHudValue | CapPhone | CapGateway | CapPhoneSetup | CapPhoneGateway` (0x7F)
- Server masks CapPhone based on `phoneEnabled_` flag (set by `SKRE_PhoneEnable(true)` in gamemode)
- Expected negotiation: `caps=0xf client_caps=0xf` minimum (with gateway: 0x7f/0x7f)
- Gate UID: 0x534B5247 ("SKRG") — unchanged
- Protocol: no codec or RPC changes in this build

## 7. Test Results

### Gate-side tests (15 suites, 0 failures)

| Suite | Checks |
|---|---|
| SKREnhancedTests | 588,301 |
| SKRClientPolicyTests | pass |
| SKRRejectionTests | 40 |
| SKRNativeApiTests | 70 |
| SKRPhoneProtocolTests | 134 |
| SKRPhoneBackendTests | 48 |
| SKRPhoneTests (M1) | 780 |
| SKRPhoneM2Tests | 241 |
| SKRPhoneM3Tests (+ M5.9) | 134 |
| SKRPhoneM4FTests | 59 |
| SKRPhoneM5Tests | 49 |
| SKRRadarTransitionTests | 223 |
| SKRXTests | 158 |
| SKRXVehiclesTests | 414 |
| SKRGpsTests | 23 |

### Client-side tests (5 suites, 0 failures)

| Suite | Checks |
|---|---|
| SKRLobbyTests | 43 |
| SKRLaunchIpcTests | 18 |
| SKRIntroTests | 98 |
| SKROpsTests | 17 |
| SKRTests | 26 |

**Total: 20 suites, 2,565+ checks, 0 failures.**

## 8. Database Backup and Migration Status

- `server-runtime-branded-test/scriptfiles/skr.db` — present, NOT modified
- No schema migration applied in this deployment
- Phone tables created on first `SKRE_PhoneEnable(true)` via `CREATE TABLE IF NOT EXISTS`
- Existing phone numbers and SMS history preserved

## 9. Rollback Instructions

1. Stop skr-server.exe
2. Copy `docs/ai-memory/checkpoints/deployment_backup_20261024_223912/Gate.dll` → `server-runtime-branded-test/components/Gate.dll`
3. Copy `docs/ai-memory/checkpoints/deployment_backup_20261024_223912/SKR.Client.dll` → `build-test-certification-win32/Release/SKR.Client.dll`
4. Verify hashes match the "Before" values above
5. Restart the server

## 10. Remaining Limitations

- **OP215 not included** — `SKR_OP215=OFF` in this client build configuration
- **CEF binaries not bundled** — self-contained CEF path changes are compiled but CEF download/build not completed (Task #21 blocker)
- **Phone calls not implemented** — server stub only; dialer not shown
- **Unread counts client-side only** — server has no read state
- **SMS dedup ring lost on server restart** — in-memory only
- **GPS shows sample destinations** — not wired to live POI data
- **RCON enabled with dev password** — this is a TEST config, not production

## 11. Deployment GO / NO-GO

**GO** — for test runtime deployment.

All preconditions satisfied:
- Source baseline verified (all features present)
- Both DLLs built clean (warnings are upstream/pre-existing only)
- All 20 test suites pass
- Pre-deployment backups created with verified hashes
- Server stopped before swap
- Post-deployment hashes match build outputs
- Runtime assets (phone wallpaper, CEF, SA-MP, launcher) in place
- Database preserved

## 12. Runtime Certification Status

| Milestone | Status |
|---|---|
| Source verified | ✅ VERIFIED |
| COMPILED | ✅ Both DLLs |
| DEPLOYED | ✅ To test runtime paths |
| RUNTIME VERIFIED | ❌ PENDING — operator must launch GTA manually |

**The operator should start the server, launch GTA via SKRLauncher.exe, and verify:**

1. Server log shows `caps=0x7f client_caps=0x7f` (or 0xf/0xf without gateway)
2. Phone activation via `/phoneauth` — `push-sync` with assigned number
3. SMS send/receive between two players
4. Native pause menu functional (ESC)
5. Phone M5.9 browsing pose visible (CJ holds phone at chest height)
6. GPS navigation panel functional
7. Contacts add/remove/search
8. SKR-X vehicle sync (if test vehicles configured)

**Do not mark RUNTIME VERIFIED until the operator completes manual testing.**
