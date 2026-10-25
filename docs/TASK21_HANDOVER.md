# Task #21 Handover — Release Packaging

**Written:** 2026-10-21 21:17 UTC+2
**Session:** session_01HLjPkxH3YEzfWfeybFkZuR

---

## 1. What Is Completed

### GitHub Publication (Tasks #22–#26) — DONE
- `staark-dev/skrmp` published: main at `6f4434c` (78 commits)
- `staark-dev/omp-cef` published: main at `79fb79e` (3 SKR commits)
- Clean clone verification PASSED — submodule resolves, 310 tracked files, no sensitive files
- Tag `skr-client-1.0.0-rc1` NOT published (session proxy blocked; see manual step below)
- Co-Authored-By trailers in omp-cef: postponed per user decision

### Task #21 Code Changes — CODE COMPLETE, NOT BUILD-VERIFIED
- **GTA path resolution** (`apps/launcher/main.cpp`): `FindGtaSa()` → `ResolveGtaCandidate()` → folder picker → persists to `%APPDATA%\SKR\launcher.ini`. 14/14 settings tests passed.
- **CEF self-contained loader** (`client/cef-local/cef_local_loader.cpp`): `SelfDir()`, `GameDir()`, `LoadCefModule()` with bundled/classic fallback. 16/16 path-resolution tests passed.
- **omp-cef fork** (`deps/omp-cef/src/client/core/browser/manager.cpp`): `cef_dir` derived from module path. Already committed as `79fb79e`.

### Existing Builds on Disk
- `build-external-lobby/Release/SKRLauncher.exe` — 330KB, built 2026-10-21 17:36 (contains Task #19 icon + Task #20A stage keys; may NOT include Task #21 GTA path changes — rebuild needed)
- `build-external-lobby/Release/SKR.Client.dll` — 263KB, built 2026-10-21 13:11
- `server-runtime-branded-test/components/Gate.dll` — 160KB, built 2026-10-21 12:55 (Task #18 client policy)

---

## 2. What Is Blocked

| Blocker | Detail |
|---|---|
| Windows build | Task #21 changes to `main.cpp` and `cef_local_loader.cpp` need rebuilding. No Windows toolchain in agent environment. |
| CEF binaries | NOT downloaded. Target: `cef_binary_148.0.10+g7ee53f5+chromium-148.0.7778.218_windows32` from `cef-builds.spotifycdn.com`. ~100MB download needed. |
| License texts | `licenses/` directory empty except `.gitkeep`. Need: CEF/Chromium, open.mp components, crashdetect, mapandreas, sscanf, streamer, pawnraknet, openal-soft (LGPL check), WebView2, MSVC redist. |
| Tag push | `skr-client-1.0.0-rc1` — must be pushed from Windows (see Section 5). |

---

## 3. Release Packaging Directories (Both Currently EMPTY)

```
C:\Users\Staark\Desktop\SKR Enhanced — Release Packaging\SKR Runtime Server 1.0.23\
C:\Users\Staark\Desktop\SKR Enhanced — Release Packaging\SKR Enhanced Client 1.0.0\
```

Both directories exist and are accessible. Folder access was granted this session.

---

## 4. Exact Remaining Steps

### Step A — Rebuild Launcher (REQUIRED, Windows)
```
cd "C:\Users\Staark\Desktop\SKR Platform"
cmake --build build-external-lobby --config Release --target SKRLauncher
```
Then verify: run `SKRLauncher.exe` from a folder without GTA — confirm folder picker appears, validates, persists to `%APPDATA%\SKR\launcher.ini`.

### Step B — Package SKR Runtime Server 1.0.23

Source: `server-runtime-branded-test/`
Destination: `SKR Enhanced — Release Packaging/SKR Runtime Server 1.0.23/`

**Include:**
- `skr-server.exe`
- `skr-runtime.json`
- `SKR-Runtime-1.0.23-README.md`
- `server.cfg`
- `components/` — ALL `.dll` files EXCEPT `Gate.dll.pre-task18.bak`, `SKRGate.dll.bak`, `Cef.so`
- `plugins/` — ALL `.dll` files + `pawnraknet.cfg` (exclude `.so` files unless Linux support intended)
- `gamemodes/` — `skr_dev.amx`, `skr_dev.pwn` (exclude `.pre-rpc`, `.pre-ysi` backups, `skr_runtime_test.*`, `zones.inc`)
- `filterscripts/` (empty, include directory)
- `includes/` (all `.inc` files)
- `models/` (include directory)
- `npcmodes/` (include directory)
- `pawnc-3.10.11/` (Pawn compiler)
- `qawno/` (Pawn editor)
- `scriptfiles/cef/` — ONLY `.pak` and `.pak.manifest` files (exclude unpacked source dirs `apps/`, `skr-auth/`, `skr-hud/`)

**Exclude:**
- `config.json` — contains RCON dev password `skrdev2026`
- `config.json.bak`
- `skr-server.pdb` (29MB debug symbols)
- `log.txt` (1.7MB runtime log)
- `bans.json` (runtime state)
- `scriptfiles/skr.db` (gamemode recreates via CREATE TABLE IF NOT EXISTS)
- `server-runtime/` subdirectory (stable runtime copy, protected)
- `.vscode/`
- All `.bak` files in components/

**Create:** Production `config.json` template — same structure but:
- `"rcon": { "enable": false, "password": "" }`
- `"name": "SKR Enhanced Server"`
- `"mode": "SKR Enhanced"`
- Remove or neutralize dev-specific values

### Step C — Download CEF Binaries (REQUIRED)
```
Target: cef_binary_148.0.10+g7ee53f5+chromium-148.0.7778.218_windows32
URL: https://cef-builds.spotifycdn.com/
```
Extract and place in client package under `cef/` directory.
Required files: `libcef.dll`, `renderer.exe`, `icudtl.dat`, `resources.pak`, `locales/`, plus CEF support DLLs.

### Step D — Package SKR Enhanced Client 1.0.0

Destination: `SKR Enhanced — Release Packaging/SKR Enhanced Client 1.0.0/`

**Include:**
- `SKRLauncher.exe` (from Step A rebuild)
- `SKR.Client.dll` (from `build-external-lobby/Release/`)
- `cef/` directory with:
  - `client.dll` + `renderer.exe` (from omp-cef build — ALSO NOT BUILT YET)
  - CEF runtime binaries (from Step C)
- `licenses/` directory with third-party notices

**Also not built:** omp-cef `client.dll` and `renderer.exe` — these must be built from the fork.

### Step E — Gather License Texts
Required notices for: CEF/Chromium (BSD), open.mp components, crashdetect, mapandreas, sscanf, streamer, pawnraknet, openal-soft (LGPL verification), WebView2 Runtime, MSVC redistributables.

### Step F — Independent Verification
- Test server package: run `skr-server.exe` from the release directory (outside dev workspace)
- Test client package: run `SKRLauncher.exe` from the release directory, verify GTA launch, CEF init, `/q` returns to launcher
- Record PASS/FAIL/BLOCKED per deliverable

### Step G — Push Tag (Manual, from Windows)
```
cd "C:\Users\Staark\Desktop\SKR Platform\deps\omp-cef"
git remote add skr-fork https://github.com/staark-dev/omp-cef.git
git push skr-fork skr-client-1.0.0-rc1
```

---

## 5. Modified Files NOT Committed (Working Tree)

These 13 files have CRLF→LF line-ending changes only — no code changes:
```
HUD_SPEC.md
client/cef-bridge/cef_bridge.cpp
client/cef-bridge/cef_bridge.h
client/cef-bridge/cef_plugin_exports.cpp
client/hud/hud.html
client/hud/icons/icons.css
client/hud/icons/manifest.json
client/hud/phone.html
client/lobby/menu_patch.cpp
client/lobby/menu_patch.h
client/lobby/settings_io.cpp
client/lobby/settings_io.h
docs/lobby-continuation.md
```

Task #21 source changes (also uncommitted):
- `apps/launcher/main.cpp` — GTA path resolution
- `client/cef-local/cef_local_loader.cpp` — self-contained CEF loader

These must be preserved. Do not run `git checkout` or `git clean` on the working tree.

---

## 6. Temporary Files to Clean Up

On the user's Desktop (can be deleted at any time):
- `skr-omp-cef.bundle` (17KB) — git bundle used for publication
- `skrmp-full.bundle` (12MB) — git bundle used for publication

---

## 7. Constraints Still In Effect

- Do not rewrite existing commit history
- Do not force-push
- Do not publish omp-cef local commits to upstream (aurora-mp/omp-cef)
- `server-runtime/` and `C:\Users\Staark\Desktop\SKR-Backups\` are protected
- `config.json` RCON password `skrdev2026` must NEVER be in a release package
- Do not start Task #20B/#20C or Next Gen development
