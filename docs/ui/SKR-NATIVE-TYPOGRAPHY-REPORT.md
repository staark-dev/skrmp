# SKR Native ImGui Typography Report

**Date:** 2026-10-24
**Status:** COMPILED, TESTED, RUNTIME VERIFIED

## 1. Summary

Replaced system fonts (Bahnschrift, Segoe UI, Arial) with Inter typeface (v4.1,
SIL Open Font License) across all ImGui-rendered UI: notifications, confirm
dialogs, iFruit phone, and pause menu. No layout or geometry changes.

## 2. Inter Font Files

| File | Weight | Size |
|---|---|---|
| `assets/fonts/Inter-Regular.ttf` | 400 (Regular) | 411 KB |
| `assets/fonts/Inter-Medium.ttf` | 500 (Medium) | 417 KB |
| `assets/fonts/Inter-SemiBold.ttf` | 600 (SemiBold) | 419 KB |
| `assets/fonts/Inter-Bold.ttf` | 700 (Bold) | 420 KB |
| `assets/fonts/Inter-LICENSE.txt` | — | SIL OFL 1.1 |

Runtime location: `SKR\fonts\` relative to `SKR.Client.dll`.

## 3. Font Loading Architecture

**File:** `client/native-pause/pause_renderer.cpp`

The `loadFont()` lambda resolves Inter from the DLL directory (`GetModuleHandleExA`
→ `GetModuleFileNameA` → strip to directory → `SKR\fonts\<file>`). If the Inter
file is missing or fails to load, it falls back to Windows system fonts in order:

| Role | Inter file | System fallback |
|---|---|---|
| Heading (SemiBold) | `Inter-SemiBold.ttf` | `bahnschrift.ttf` → `arialbd.ttf` |
| Body (Regular) | `Inter-Regular.ttf` | `segoeui.ttf` → `arial.ttf` |

### Rasterisation sizes

| Face | Pixel size | Usage |
|---|---|---|
| `g_fontHeading` | 40 px | Pause menu headings, confirm dialog title (via `fontLarge`) |
| `g_fontBody` | 40 px | Pause menu body text |
| `pf.heading[0]` | ~12 px (phoneSm) | Phone small heading text |
| `pf.heading[1]` | ~22 px (phoneMd) | Phone medium heading, notification heading |
| `pf.body[0]` | ~12 px (phoneSm) | Phone small body text |
| `pf.body[1]` | ~22 px (phoneMd) | Phone medium body, notification body |

phoneSm = `max(10, round(12 × displayH/768))`, phoneMd = `max(16, round(22 × displayH/768))`.

Glyph ranges: Latin (U+0020–00FF), Latin Extended-A (U+0100–017F), Romanian
diacritics (U+0218–021B: Ș, ș, Ț, ț).

## 4. Typography Scale

### Notification toasts

| Element | Font | Draw size (at s=1) | Before |
|---|---|---|---|
| Category label | Inter SemiBold (heading) | 15 px | 15 px (Bahnschrift) |
| Body text | Inter Regular (body) | 15 px | 14 px (Segoe UI) |

### Confirm dialog (Native API)

| Element | Font | Draw size (at s=1) | Before |
|---|---|---|---|
| Title | Inter SemiBold (fontLarge, 40px) | 20 px | 18 px |
| Message | Inter Regular (body) | 16 px | 14 px |
| Button labels | Inter Regular (body) | 15 px | 13 px |
| Countdown | Inter Regular (body) | 14 px | 13 px |

### Phone (unchanged layout, new typeface)

| Element | Font face | Raster size |
|---|---|---|
| Screen title | Inter SemiBold heading[1] | ~22 px |
| Primary text | Inter Regular body[1] | ~22 px |
| Secondary/small text | Inter Regular body[0] | ~12 px |
| App labels | Inter SemiBold heading[0] | ~12 px |

All sizes scale linearly with `displayH / 768`.

## 5. Modified Source Files

| File | Change |
|---|---|
| `client/native-pause/pause_renderer.cpp` | Rewrote font loading: `loadFont()` tries Inter from `SKR\fonts\` first, falls back to system fonts. DLL path resolved via `GetModuleHandleExA`. Removed separate `load()` and `loadSize()` lambdas. |
| `client/native-api/native_api_ui.cpp` | Bumped `kBodySize` 14→15, confirm title 18→20px, confirm message 14→16px, button labels 13→15px, countdown 13→14px. |

## 6. Geometry Verification

No layout files modified. Verified in-game:

| Component | Status |
|---|---|
| Phone position/size | UNCHANGED |
| Phone body anchor (bottom-right) | UNCHANGED |
| Notification position (bottom-right) | UNCHANGED |
| Notification card width (282px base, 340px cap) | UNCHANGED |
| Confirm dialog position (centred) | UNCHANGED |
| Radar dimensions | UNCHANGED |
| ESC menu layout | UNCHANGED |
| GPS panel | UNCHANGED |
| GTA HUD elements | UNCHANGED |

## 7. Build Artifacts

| Artifact | SHA-256 | Size |
|---|---|---|
| SKR.Client.dll | `D5895E348862DD6A3BC7882E3CC0FE0E4D3E1ABBB0877B8BBC7558FECCDD974E` | 916,480 B |

## 8. Test Results

### Client-side (10 suites, 0 failures)

| Suite | Result |
|---|---|
| SKRTests | 26/26 passed |
| SKRLobbyTests | 43 checks, 0 failures |
| SKRLaunchIpcTests | 18 checks, 0 failures |
| SKRIntroTests | 98 passed, 0 failed |
| SKROpsTests | 17/17 passed |
| SKRPauseControlsTests | passed |
| SKRFpsD3D9Tests | passed |
| SKRNativeInitTests | passed |
| SKRPreflightTests | passed |
| SKRHandoffTests | 85 checks, 0 failed |

### Gate-side

Server-side tests are unaffected (no font/rendering code on server).

## 9. Runtime Verification (in-game)

| Check | Result |
|---|---|
| INFO toast renders with Inter | VERIFIED |
| SUCCESS toast renders with Inter | VERIFIED |
| Confirm dialog title (Inter SemiBold) | VERIFIED |
| Confirm dialog body + buttons (Inter Regular) | VERIFIED |
| Phone SMS conversation (Romanian text) | VERIFIED |
| Phone Messages list | VERIFIED |
| Phone home screen (clock, labels) | VERIFIED |
| Phone Settings screen | VERIFIED |
| Phone GPS screen | VERIFIED |
| Pause menu confirm dialog | VERIFIED |
| No geometry regressions | VERIFIED |
| FPS stable (94-95) | VERIFIED |

## 10. Protected Components

| Component | Status |
|---|---|
| Phone layout (`phone_layout.h`) | NOT MODIFIED |
| Radar dimensions | NOT MODIFIED |
| ESC Menu | NOT MODIFIED |
| GPS routing | NOT MODIFIED |
| Native API protocol | NOT MODIFIED |
| Gate UID 0x534B5247 | PRESERVED |
| SKR Runtime Server 1.0.23 branding | PRESERVED |
