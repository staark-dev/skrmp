# T01 Runtime Build Verification Report

**Date:** 2026-10-25
**Verdict:** Previous test ran a pre-M2.1 binary. Corrected M2.1-C + OP215 binary now staged.

---

## 1. Problem

The supplied `skrx_vehicles.log` showed five failed hook installations using
the old prolog signature (`55 8B EC 83 EC 08`).  Every run reported
`SKR_OP215=OFF`.  The M2.1-C `HOOK PE check` diagnostic was absent.

---

## 2. Root Cause

The test environment `C:\Users\Staark\Desktop\SKR-Portable-Test` contained a
**pre-M2.1** binary from 2026-10-21:

| Property | Value |
|----------|-------|
| Path | `SKR-Portable-Test\SKR.Client.dll` |
| Size | 263,680 bytes |
| Modified | 2026-10-21 15:11:49 |
| SHA-256 | `d015365552da7d3fcdb6c70d8b0d1f8cdb57c9625db4a933ef07419c34781330` |
| Features | No VEHICLES hook, no OP215, no M2.1 code at all |

The launcher loads `SKR.Client.dll` from the same directory as `SKRLauncher.exe`
(or from `preflightReport.skrClientPath`).  The M2.1-C build was never deployed
to the test environment.

---

## 3. Available Build Artifacts (M2.1-C, built 2026-10-25)

| Build preset | SKR_OP215 | Size | SHA-256 |
|-------------|----------|------|---------|
| external-lobby-x86 | OFF | 904,192 | `6d22c20015dda3a367c83ee677544439f35e97c77a7ce9ea60ee749109935d12` |
| **op215-x86** | **ON** | **908,288** | **`4b55429b222865fbe9a9493afd8369d322ec2e01cf9c18791bef97e147eea36a`** |

Both artifacts contain the corrected 5-byte R5 prolog (`56 8B 74 24 08`) and
the PE fingerprint verification (`HOOK PE check: TimeDateStamp=0x6372C39E`).

---

## 4. Feature Verification (op215-x86 artifact)

String markers confirmed present in the staged binary:

| Feature | Marker string | Present |
|---------|--------------|---------|
| M2.1-C hook | `HOOK PE check` | YES |
| M2.1-C hook | `HOOK install` | YES |
| M2.1-C hook | `HOOK trampoline` | YES |
| Corrected prolog | `expected 56 8B 74 24 08` | YES |
| VEHICLES namespace | `skrx_vehicles` | YES |
| OP215 | `OP215` | YES |
| Native API | `native_api` | YES |
| Native Pause | `native_pause` | YES |
| PHONE | `PHONE` | YES |
| GPS | `GPS` | YES |
| Launcher exports | `SkrRemoteInit` | YES |
| Launcher exports | `SkrGetInitializationState` | YES |
| PE check | `SizeOfImage=0x` | YES |

---

## 5. Actions Taken

### 5.1 Backup
- Old DLL backed up to: `SKR.Client.dll.pre-m21c-20261025-082901.bak`
- SHA-256 verified matches original: `d015365552da...`

### 5.2 Log archive
- All existing logs moved to: `SKR/logs-archive-20261025-082901/`
- Files: `cef_local.log`, `escape_menu.log`, `handoff.log`, `intro.log`, `samp_loader.log`
- No `skrx_vehicles.log` existed (old DLL predates VEHICLES hook)

### 5.3 Binary staged
- Source: `build-op215/Release/SKR.Client.dll`
- Destination: `SKR-Portable-Test/SKR.Client.dll`
- SHA-256: `4b55429b222865fbe9a9493afd8369d322ec2e01cf9c18791bef97e147eea36a`

### 5.4 Preserved (not touched)
- GTA archives: none in test env (portable test without GTA)
- User settings: `SKR/lobby/` assets unchanged
- SQLite databases: `server-runtime-branded-test/skr.db` (405,504 bytes, 2026-10-24)
- Server runtime: untouched
- OP215 assets: not present in test env (expected — see §6)

---

## 6. Remaining Operator Steps

1. **Deploy OP215 assets** to `SKR-Portable-Test/SKR/op215/`:
   - `skr_vehicles.img` (DFF/TXD for model 612)
   - Without these, the hook installs but defers all swaps (vehicles appear as proxy 400)

2. **Launch GTA test session** and verify log output:
   ```
   HOOK PE check: TimeDateStamp=0x6372C39E SizeOfImage=0x27E000
   HOOK install: samp=0x10000000 target=0x1001F080 (RVA 0x1F080)
   HOOK trampoline at 0x<addr>: 56 8B 74 24 08 E9 -> 0x1001F085
   HOOK installed: trampoline=0x<addr> prolog=568B742408
   ```

3. **Verify model swap** with OP215 assets deployed:
   ```
   HOOK swap pool=<N> proxy=400 -> model=612 swaps=1
   ```

---

## 7. Identification

The next operator-controlled GTA run can unambiguously identify the M2.1-C
binary by:

1. **SHA-256** of the loaded DLL: `4b55429b2228...`
2. **File size**: 908,288 bytes (vs 263,680 for old)
3. **Log diagnostic**: presence of `HOOK PE check:` line (absent in any pre-M2.1-C binary)
4. **Log diagnostic**: prolog logged as `568B742408` (5 bytes, not `558BEC83EC08`)
5. **OP215 markers** in the log: OP215-specific paths will log when `SKR_OP215=ON`

**Operator deployment is still required** — the binary is staged but GTA was
not launched and OP215 assets are not yet in the test environment.
