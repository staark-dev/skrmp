# TASK #21.1 — RELEASE REPORT
## SKR Enhanced Client 1.0.0

**Date:** 2026-10-22  
**Author:** Claude (automated analysis) + Ionut (project lead)  
**Scope:** SA-MP runtime distribution, dependencies, packaging & validation

---

## 1. Changes Made

### 1.1 Prior Sessions (Verified on Disk)
- **`samp_loader.cpp`** — `SelfDir()` resolves paths relative to `SKR.Client.dll` via `GetModuleHandleExW(FROM_ADDRESS)`. `FindSampAt()` validates candidate samp.dll with SHA-256. `PreloadSamp()` search order: (1) `<self_dir>\samp\samp.dll` bundled, (2) `<gta_dir>\samp.dll` legacy fallback.
- **`dllmain.cpp`** — External lobby path reads `LaunchParams` via named pipe, calls `PreloadSamp(&selected)` with server params.
- **`game_launcher.cpp`** — `StartupFiles` RAII pattern hides/restores existing `samp.dll` and `d3d9.dll` in GTA directory during launch.
- **`escape_menu.cpp`** — `PatchGtaMainMenu()` and `RestoreGtaMainMenu()` confirmed present (lines 120, 166, 204, 240).

### 1.2 This Session
- Created `build-release.bat` — automated VS 2022 build script for SKR.Client.dll + SKRLauncher.exe.
- Created `package-release.bat` — assembles the release directory with correct layout.
- Created `validate-release.py` — automated 8-check validation script with nonzero exit on failure.
- Created `TASK21-RELEASE-REPORT.md` — this document.

---

## 2. Source Paths and Build Configurations

| Component | Source | Build Config |
|-----------|--------|-------------|
| SKRLauncher.exe | `apps/launcher/` | CMake, Release, x86, `/MD`, `-DSKR_EXTERNAL_LOBBY=ON` |
| SKR.Client.dll | `client/` | CMake, Release, x86, `/MD`, `-DSKR_EXTERNAL_LOBBY=ON` |
| omp-cef client.dll | `deps/omp-cef/src/client/` | CMake, Release, x86, `/MT` (static CRT) |
| omp-cef renderer.exe | `deps/omp-cef/src/client/` | CMake, Release, x86, `/MT` |
| CEF 148 binaries | `deps/omp-cef/deps/cef/cef_binary_148.0.10+...windows32/` | Pre-built |

**Build directory:** `build-external-lobby/` (NOT `build/`)  
**Build script:** `build-release.bat`

---

## 3. Loaded SA-MP Path

The loader resolves samp.dll in this order:

1. **Bundled:** `<SKR.Client.dll dir>\samp\samp.dll` — primary, verified by SHA-256
2. **Legacy fallback:** `<GTA dir>\samp.dll` — for developer/existing installations

The bundled path matches the release layout: `SKR Enhanced Client 1.0.0\samp\samp.dll`

SA-MP uses `GetModuleHandleA("samp.dll")` internally — module name lookup works regardless of load path. SA-MP uses `GetModuleFileName(NULL)` (GTA exe) for its own file paths, not its own module path. SKR bypasses `sa-mp.cfg` via direct `R5Settings` struct writes at RVA `0x26DFE8`.

---

## 4. Dependency Audit

### 4.1 System DLLs (Present on all target Windows)
`kernel32.dll`, `user32.dll`, `gdi32.dll`, `ws2_32.dll`, `wininet.dll`, `advapi32.dll`, `shell32.dll`, `bcrypt.dll`, `d3d9.dll`, `imm32.dll`, `version.dll`, `comctl32.dll`, `msvcrt.dll`

### 4.2 Redistributable — Required
| Dependency | Provider | Required By |
|-----------|----------|-------------|
| `d3dx9_25.dll` | DirectX End-User Runtime (June 2010) | SA-MP (font rendering via `D3DXCreateFont`) |
| `vcruntime140.dll` | Visual C++ 2015-2022 Redistributable (x86) | SKR.Client.dll (`/MD` linking) |
| `msvcp140.dll` | Visual C++ 2015-2022 Redistributable (x86) | SKR.Client.dll (`/MD` linking) |

**Action required:** Implement safe preflight check for `d3dx9_25.dll` presence. If missing, prompt user to install Microsoft's official DirectX End-User Runtime. Do NOT redistribute the DLL directly.

### 4.3 Optional / Feature Dependencies
| Dependency | Status | Impact if Missing |
|-----------|--------|-------------------|
| `bass.dll` | Optional | Audio streaming disabled; not critical |
| `d3dcompiler_47.dll` | Bundled with CEF | GPU shader compilation |
| `libEGL.dll`, `libGLESv2.dll` | Bundled with CEF | ANGLE rendering backend |

### 4.4 Self-Contained (No External Dependencies)
- omp-cef (`client.dll`, `renderer.exe`) — statically linked (`/MT`), no CRT dependency
- CEF binaries — self-contained distribution

### 4.5 SA-MP Resource Analysis
- **Fonts:** `D3DXCreateFont` uses system fonts, no custom font files needed
- **Cursors:** Standard Windows cursors
- **Config:** SKR bypasses `sa-mp.cfg` entirely via struct writes
- **Conclusion:** samp.dll is self-contained as a single DLL file

---

## 5. Runtime Package Contents

```
SKR Enhanced Client 1.0.0/
    SKRLauncher.exe              (~330 KB)
    SKR.Client.dll               (~264 KB)

    samp/
        samp.dll                 (SA-MP 0.3.7-R5, ~3.8 MB)

    cef/
        client.dll               (~2.8 MB, omp-cef core)
        renderer.exe             (omp-cef subprocess) ← NOT BUILT
        libcef.dll               (~216 MB, CEF 148)
        chrome_elf.dll           (~1.7 MB)
        d3dcompiler_47.dll       (~4 MB)
        libEGL.dll               (~413 KB)
        libGLESv2.dll            (~6.8 MB)
        v8_context_snapshot.bin   (~714 KB)
        icudtl.dat               (ICU data)
        resources.pak            (CEF resources)
        chrome_100_percent.pak   (UI scale resources)
        chrome_200_percent.pak   (HiDPI resources)
        locales/
            en-US.pak            (English locale)

    SKR/
        lobby/
            lobby.html
            lobby.js
            lobby.css
            assets/
                background.png
                logo.png

    LICENSES/
        [applicable notices]

    release-manifest.json
```

---

## 6. SHA-256 Verification

### Pinned SA-MP R5 Hash (from `samp_loader.cpp`)
```
B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2
```

**PE Signature:**
- TimeDateStamp: `0x6372C39E`
- SizeOfImage: `0x27E000`

**Status:** Hash is hardcoded in `kR5Sha256` (line 41-43). `HashMatchesPinnedR5()` verifies at load time using BCrypt SHA-256. `IsPinnedR5Image()` validates PE headers post-load.

**Verification result:** CANNOT VERIFY — samp.dll binary not found in connected project folders. Must be provided separately for packaging.

---

## 7. Automated Test Results

### validate-release.py
**Status:** Script created and ready. Cannot execute against a populated release directory until:
1. Builds are completed (SKR.Client.dll is stale, renderer.exe missing)
2. samp.dll binary is placed at `samp\samp.dll`
3. `package-release.bat` is run to assemble the directory

**Checks implemented:**
1. ✅ Required file existence check
2. ✅ SA-MP SHA-256 verification (via `--samp-hash`)
3. ✅ x86 PE architecture validation
4. ✅ Import dependency analysis
5. ✅ Stale binary detection (build vs release hash comparison)
6. ✅ GTA file exclusion check
7. ✅ Release manifest accuracy check
8. ✅ Nonzero exit code on failure

---

## 8. Manual / Runtime Test Results

### Section 6 Runtime Tests — ALL BLOCKED

| # | Test | Status | Reason |
|---|------|--------|--------|
| 1 | Launcher starts | BLOCKED | Build stale, needs rebuild |
| 2 | SKR.Client.dll injection | BLOCKED | Build stale |
| 3 | Bundled samp.dll loads from intended dir | BLOCKED | samp.dll not available, build stale |
| 4 | Connect to SKR Runtime Server 1.0.23 | BLOCKED | Depends on 1-3 |
| 5 | Player spawning | BLOCKED | Depends on 4 |
| 6 | SA-MP networking | BLOCKED | Depends on 4 |
| 7 | /q returns to launcher | BLOCKED | Depends on 4 |
| 8 | Repeated launches don't damage GTA | BLOCKED | Depends on 1-3 |
| 9 | Missing dependency error messages | BLOCKED | Depends on 1-3 |
| 10 | Works without preinstalled SA-MP | BLOCKED | Depends on 1-3 |

### Reproduction Instructions

To execute runtime tests:
```
1. Open "x64 Native Tools Command Prompt for VS 2022"
2. cd C:\Users\Staark\Desktop\SKR Platform
3. Run: build-release.bat
   → Builds SKR.Client.dll and SKRLauncher.exe in build-external-lobby\Release\

4. Build omp-cef (client.dll + renderer.exe):
   cd deps\omp-cef
   mkdir build && cd build
   cmake .. -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=Release
   cmake --build . --config Release --target client renderer
   → Produces client.dll and renderer.exe

5. Place SA-MP 0.3.7-R5 samp.dll:
   - SHA-256 must match: B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2
   - Copy to: SKR Platform\samp\samp.dll

6. Run: package-release.bat
   → Assembles release at Desktop\SKR Enhanced — Release Packaging\SKR Enhanced Client 1.0.0\

7. Run: python validate-release.py --samp-hash B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2 --generate-manifest --build-dir build-external-lobby\Release

8. Launch SKRLauncher.exe from the release directory
9. Select a GTA SA installation (1.0 US, no preinstalled SA-MP)
10. Connect to SKR Runtime Server (localhost or configured IP)
11. Verify player spawns, networking works, /q returns to launcher
12. Relaunch and verify GTA files are unchanged
```

---

## 9. Remaining Blockers

### BLOCKER 1: SKR.Client.dll Build is Stale
- **Source:** `samp_loader.cpp` modified Oct 22 08:48 UTC
- **Binary:** `SKR.Client.dll` built Oct 21 13:11 UTC
- **Impact:** Release binary does not contain latest samp_loader changes
- **Fix:** Run `build-release.bat`

### BLOCKER 2: renderer.exe Not Built
- **Component:** omp-cef subprocess renderer
- **Impact:** `BrowserManager::ValidateCefRuntime()` requires `renderer.exe` in `cef/` directory. CEF lobby will fail to initialize without it.
- **Fix:** Build omp-cef with `cmake --build . --config Release --target renderer`

### BLOCKER 3: samp.dll Binary Not Available
- **Component:** SA-MP 0.3.7-R5 client runtime
- **Impact:** Cannot populate `samp/samp.dll` in release package
- **Fix:** Provide the pinned R5 binary (SHA-256: `B72B5DBE...432553A2`)
- **Note:** Redistribution authorization should be confirmed before bundling

### BLOCKER 4: d3dx9_25.dll Preflight Not Implemented
- **Spec requirement:** "Implement a safe preflight and use Microsoft's official DirectX End-User Runtime installer when required, with user consent."
- **Impact:** Users without DirectX End-User Runtime will get an opaque crash from SA-MP font rendering
- **Fix:** Add runtime check in SKR.Client.dll initialization; prompt user with download link if missing

### BLOCKER 5: Runtime Tests Not Executed
- **Impact:** Cannot confirm the acceptance requirement ("A compatible GTA installation without preinstalled SA-MP must launch through SKR Enhanced")
- **Depends on:** Blockers 1-3 resolved
- **Fix:** Execute the reproduction steps in Section 8

---

## 10. GTA Installation Protection — VERIFIED (Code Review)

The `StartupFiles` RAII pattern in `game_launcher.cpp` provides safe protection:

- ✅ `Hide()` renames `samp.dll` → `samp.dll.skr` and `d3d9.dll` → `d3d9.dll.skr`
- ✅ Won't overwrite existing `.skr` backup (returns false)
- ✅ No-op if original doesn't exist
- ✅ Destructor restores files even on failure/crash
- ✅ Uses absolute paths via `GtaDir()` resolution
- ✅ No permanent writes to GTA directory

**Scenarios (code-reviewed, not runtime-tested):**
- GTA without SA-MP: Hide is no-op (no samp.dll to rename), SKR loads bundled copy ✅
- GTA with same SA-MP revision: Hides it, loads bundled, restores on exit ✅
- GTA with incompatible SA-MP: Same behavior as above ✅
- GTA with existing d3d9.dll: Hides it, restores on exit ✅
- Separate directories: SKR resolves paths independently via SelfDir() ✅

---

## 11. Final Release Readiness Verdict

# BLOCKED

**5 blockers prevent release.** The architecture and code are verified and correct. The packaging pipeline (scripts, validation, layout) is complete. What remains is:

1. **Rebuild** SKR.Client.dll (run `build-release.bat`)
2. **Build** omp-cef renderer.exe
3. **Provide** the pinned SA-MP R5 samp.dll binary
4. **Implement** d3dx9_25.dll preflight check
5. **Execute** runtime tests on Windows with GTA SA

Once blockers 1-3 are resolved, `package-release.bat` will assemble the distribution and `validate-release.py` will verify it. Runtime tests (blocker 5) are required before the release can be marked PASS.

**The release cannot be marked ready until critical dependencies are verified at runtime.**

---

## Deliverables Produced

| File | Purpose |
|------|---------|
| `build-release.bat` | Automated VS 2022 build for SKR components |
| `package-release.bat` | Assembles release directory with correct layout |
| `validate-release.py` | 8-check automated validation, nonzero exit on failure |
| `TASK21-RELEASE-REPORT.md` | This report |
