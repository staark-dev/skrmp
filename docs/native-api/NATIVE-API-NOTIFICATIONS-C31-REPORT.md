# Native API Notifications — C3.1 Report

**Date:** 2026-10-24
**Status:** COMPILED, TESTED — awaiting manual GTA runtime certification

## 1. Original Defects and Root Causes

### Rendering backend mismatch
The notification toasts used GTA's CFont + CSprite2d in the native 2D HUD pass.
This provided no text wrapping metrics, no rounded corners, and no way to detect
the phone's screen bounds for coexistence. Text height was estimated via
`kPxPerScaleY = 14.0` and `kCharWidthFactor = 0.52` — visual guesses that
produced inconsistent card heights for multiline content.

### Incorrect positioning
Toasts were centred horizontally at the top of the screen (`y = 92 * s`,
`x = (W - w) / 2`). The spec requires bottom-right anchor.

### Excessive width
Card width was `min(560 * s, W - 32)` — approximately 398 px at 1360×768.
The spec requires ~282 px at 1360×768, maximum 340 px absolute.

### No phone coexistence
No detection of phone screen bounds. Notifications could overlap the open phone.

### No entrance animation
Only a fade-in alpha ramp existed; no slide-in transition.

### No rounded corners
CSprite2d::DrawRect draws sharp rectangles.

## 2. Modified Source Files

| File | Change |
|---|---|
| `client/native-api/native_api_ui.h` | New `DrawContext` struct with ImGui draw list, fonts, display size and phone bounds. Replaced `Draw()` with `DrawImGui(const DrawContext&)`. |
| `client/native-api/native_api_ui.cpp` | Full rewrite: ImGui draw-list rendering replacing CFont/CSprite2d. Bottom-right anchor, phone-aware repositioning, Chaikin-smooth entrance animation, proper text wrapping via `CalcTextSizeA`, rounded corners, accurate card heights from measured text bounds. Queue and lifecycle logic preserved unchanged. |
| `client/native-api/native_api.cpp` | `Draw()` stub retained (no-op); rendering moved to ImGui pass. |
| `client/native-pause/pause_renderer.cpp` | Added `#include "native-api/native_api_ui.h"`. Added notification draw call in the gameplay overlay ImGui pass (after phone, before EndFrame), passing phone layout bounds for coexistence. |
| `client/game-state/game_state.cpp` | Removed `native_api::Draw()` call from the GTA 2D HUD pass. |

## 3. Layout Changes

### Before (CFont, GTA 2D pass)
- Position: top-centre, `y = 92 * s`
- Width: `min(560 * s, W - 32)` ≈ 398 px at 1360×768
- No rounded corners
- Estimated text metrics
- No phone awareness

### After (ImGui, D3D9 EndScene pass)
- Position: bottom-right, stacking upward
- Width: `min(282 * s, 340)` ≈ 282 px at 768p, 340 px cap
- 8u rounded corners
- Actual ImGui font metrics (CalcTextSizeA with wrap width)
- Phone-aware: shifts stack left of phone body with 14u gap when phone is open
- 220 ms ease-out slide-in entrance, 200 ms fade-out exit

### Card anatomy
```
┌──────────────────────────────┐
│ ▌ CATEGORY LABEL             │  ← accent bar (4u) + category in accent color
│ ▌ Body text wraps to card    │  ← primary text, measured height
│ ▌ width using ImGui metrics  │
└──────────────────────────────┘
     padY + labelH + gap + bodyH + padY
```

### Confirm dialog
- Centred modal with backdrop overlay
- Accent stripe at top (rounded top corners)
- Title in heading font, message in body font
- [Y] CONFIRM / [N] CANCEL buttons with countdown
- All text measured via ImGui CalcTextSizeA

## 4. Notification / Phone Coexistence

| State | Notification anchor |
|---|---|
| Phone closed | Bottom-right: `W - 16s` from right edge |
| Phone open | Left of phone body: `phone.x0 - 14s` from phone left edge |
| Insufficient space | Falls back to normal bottom-right (phone overlaps acceptably at very narrow widths) |

The phone's `Layout.body.x0` is read from `phone::input::GetLayout()` each frame,
the same struct the phone renderer itself uses. No phone geometry is modified.

## 5. Queue and Lifecycle

Unchanged from the original implementation:
- FIFO queue, `kMaxQueued = 8`, `kMaxVisible = 3`
- Oldest toast dropped on overflow (`memmove` shift)
- `shownAt` assigned on first visible slot
- Expiration: `nowMs - shownAt >= durationMs`
- Confirm dialog: arm delay 400 ms, Y/N/Enter/Escape keys, timeout countdown
- `Reset()` clears all state on disconnect

## 6. D3D9 Rendering Validation

- Rendering uses `ImGui::GetBackgroundDrawList()` — no custom D3D9 state changes
- Drawn in the same EndScene pass as phone, GPS panel and pause menu
- SEH guard retained: draw faults clear the toast queue gracefully
- No duplicate rendering (removed from GTA 2D pass)
- No new textures, no per-frame font loading
- ImGui handles device loss/reset through its own backend (ImGui_ImplDX9)

## 7. Typography Metrics

| Element | Font | Size at 768p | Size at 1080p |
|---|---|---|---|
| Category label | body | 13 px | ~18 px |
| Body text | body | 12 px | ~17 px |
| Dialog title | heading | 18 px | ~25 px |
| Dialog message | body | 14 px | ~20 px |
| Button labels | body | 13 px | ~18 px |

All sizes scale linearly via `s = H / 768`. Text wrapping uses `CalcTextSizeA`
with the card's inner width, so card height accurately reflects content.

## 8. Client x86 Build Status

| Artifact | Status | Warnings |
|---|---|---|
| SKR.Client.dll | COMPILED (Release, x86) | 0 new (upstream only) |
| SKRLauncher.exe | COMPILED | — |
| All test executables | COMPILED | — |

## 9. Test Results

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

### Gate-side (16 suites, 0 failures)

| Suite | Result |
|---|---|
| SKREnhancedTests | 588,301 checks, 0 failures |
| SKRClientPolicyTests | passed |
| SKRRejectionTests | 40 checks, 0 failures |
| SKRNativeApiTests | 70 checks passed |
| SKRPhoneProtocolTests | 134 checks passed |
| SKRPhoneBackendTests | 48 checks passed |
| SKRPhoneTests | 780 passed, 0 failed |
| SKRPhoneM2Tests | 241 passed, 0 failed |
| SKRPhoneM3Tests | 134 passed, 0 failed |
| SKRPhoneM4FTests | 59 passed, 0 failed |
| SKRPhoneM5Tests | 49 checks passed |
| SKRPhoneWorkerTests | 153 checks passed |
| SKRRadarTransitionTests | 223 checks passed |
| SKRXTests | 158 checks, 0 failed |
| SKRXVehiclesTests | 414 checks, 0 failures |
| SKRGpsTests | 23 checks passed |

**Total: 26 suites, 590,000+ checks, 0 failures.**

## 10. Build SHA-256

| Artifact | SHA-256 | Size |
|---|---|---|
| SKR.Client.dll | `8DA33E381D668129C98565261E9172C1B52EC237E20DDBCC0EF3B913831459C4` | 916,480 B |

## 11. Performance Observations

- No per-frame allocations (ImGui draw lists are pre-allocated)
- No texture creation or font loading per frame
- No file I/O on the render path
- No database access
- Maximum 3 visible toasts × ~20 draw commands each = ~60 draw commands per frame
- Confirm dialog: ~15 draw commands

## 12. Remaining Runtime Limitations

- **Runtime verification pending**: notifications must be tested in-game with `/skrenotify`
- **No dismiss-on-click**: toasts expire by duration only (no interactive dismissal)
- **CFont confirm fallback removed**: if ImGui init fails, confirm dialogs won't render (the Draw() stub is a no-op); this matches phone/GPS which also require ImGui
- **GPS and System categories**: protocol defines Info/Success/Warning/Error only; GPS and System map to Info accent
- **No progress indicator**: not implemented in the current protocol
- **Minimap overlap**: safe-area uses 2% bottom margin; extreme radar sizes might overlap at very low resolutions

## 13. Protected Components — Verification

| Component | Status |
|---|---|
| Phone position/size | UNCHANGED — `phone_layout.h`, `phone_renderer.cpp` not modified for notifications |
| Radar dimensions | UNCHANGED |
| ESC Menu | UNCHANGED |
| GPS routing | UNCHANGED (GPS polish from earlier this session is separate) |
| SKR-X PHONE | UNCHANGED |
| OP215 / VEHICLES | UNCHANGED |
| Phone SMS logic | UNCHANGED |
| Native API protocol | UNCHANGED — same RPC IDs, Hello/Welcome, session/capability checks |
| Gate UID 0x534B5247 | PRESERVED |

## 14. Runtime Certification Checklist

The operator should start the server, launch GTA via SKRLauncher, and verify:

1. `/skrenotify` produces visible toast in bottom-right
2. Success/Warning/Error categories show distinct accent colors
3. Three simultaneous toasts stack correctly (bottom up)
4. Toasts do not overlap phone when phone is open
5. Toasts reposition when phone opens/closes
6. Cards expire and fade out after their duration
7. `/skrespam` does not cause ghost cards or queue overflow
8. Confirm dialog ([Y]/[N]) renders centred with backdrop
9. Alt+Tab does not break rendering
10. Disconnect clears all notifications

**Do not mark RUNTIME VERIFIED until manual testing is complete.**
