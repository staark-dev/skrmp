# PHONE M6 — UI implementation report

Date: 2026-10-24  
Status: **GO for operator-controlled GTA runtime testing; NO-GO for production certification**

## Scope and baseline

M6 was implemented on the completed PHONE M5.8 source state. The work is limited to the native ImGui/D3D9 presentation, layout constants, offline preview harness and its layout matrix. SKR-X PHONE v1, RPC dispatch, capability negotiation, SQLite, activation state, SMS/contact/GPS/Settings business logic, input ownership and VEHICLES were not changed.

The working tree already contained unrelated launcher, pause-menu, GPS, VEHICLES and server work. Those files were left intact. No GTA process was launched, no Steam installation was changed, and no build was deployed or signed.

## Concise visual audit

The M5.8 UI already had correct screen clipping, one responsive scale system, fixed-size model storage, vector icons and usable navigation. Its main visual issues were an oversized home grid, weak app-label readability over the wallpaper, closely related navy surface tones, a sparse setup screen and text-heavy empty/service states. The lower `PREVIEW` watermark also looked like product UI.

## Implemented visual changes

- Refined the charcoal/navy palette, metal rim and lime accent while preserving the established SKR Enhanced identity.
- Added one reusable native draw-list card treatment with a restrained shadow, navy depth, border highlight and optional state accent. It performs no file I/O or heap allocation.
- Reduced home icons from 46 to 34 layout units and reduced the clock from 46 to 42 units.
- Reorganized Home like a real phone: application icons use an evenly spaced wallpaper grid; Phone, Messages and Settings use a smaller, separate three-icon dock with labels omitted.
- Added distinct native vector placeholders for Camera, App Store and Wallet in a balanced two-column layout with Wallet centred below. They are deliberately non-interactive because those applications are outside M6 and have no verified backend yet; no fake screen or request is exposed.
- Removed the in-phone `PREVIEW` label completely, including the obsolete model comment. Preview provenance remains outside the phone in the generated image caption and report.
- Reworked first activation with an SKR identity card, stronger title/subtitle hierarchy, a clear server-assigned-number card and a full-width primary action. The same persisted M5 stages/actions remain in use.
- Improved empty, loading, offline and retry states with a compact status glyph and stronger actionable retry button. Error wording remains truthful.
- Improved incoming message bubbles with subtle depth and edge definition; outgoing state colours and server acknowledgement wording remain unchanged.
- Applied the common card treatment to the real GPS state and home GPS shortcut. No route data is fabricated and no second planner was added.
- Preserved the honest Phone screen: it presents the server-assigned number and states that voice calls are unavailable. No fake dial action was added.

## Files changed

- `client/phone/phone_renderer.cpp` — M6 palette, reusable surfaces, setup, empty/service states, home proportions and text contrast.
- `client/phone/phone_layout.h` — coherent body/small/clock type scale.
- `client/phone/phone_model.h` — documentation corrected after removing the watermark; no model or behavior change.
- `tests/phone_preview.cpp` — M6 caption, representative long/Unicode content and requested resolution matrix.
- `tests/unit/test_phone.cpp` — layout coverage for 1600×900 and 2560×1440.

No renderer hook, transport, RPC, backend, GPS-engine or Settings persistence file was changed.

## Before and after previews

The eight files in `docs/phone-previews/m6/before/` are preserved baseline captures. The final native renderer produced 418 PNG files in `docs/phone-previews/m6/after/` (full viewport and 2× phone crops) plus `report.txt`.

Representative pairs:

- Home: [before](../phone-previews/m6/before/03_home_1920x1080.png) / [after](../phone-previews/m6/after/03_home_1920x1080.png) / [after crop](../phone-previews/m6/after/03_home_1920x1080_zoom2x.png)
- Messages list: [before](../phone-previews/m6/before/05a_messages_threads_1920x1080.png) / [after](../phone-previews/m6/after/05a_messages_threads_1920x1080.png)
- Conversation: [before](../phone-previews/m6/before/05b_messages_conversation_1920x1080.png) / [after](../phone-previews/m6/after/05b_messages_conversation_1920x1080.png)
- Contacts: [before](../phone-previews/m6/before/04_contacts_1920x1080.png) / [after](../phone-previews/m6/after/04_contacts_1920x1080.png)
- Phone number / unsupported calls: [before](../phone-previews/m6/before/09_my_number_1920x1080.png) / [after](../phone-previews/m6/after/09_my_number_1920x1080.png)
- GPS: [before](../phone-previews/m6/before/07_gps_1920x1080.png) / [after](../phone-previews/m6/after/07_gps_1920x1080.png)
- Settings: [before](../phone-previews/m6/before/08_settings_1920x1080.png) / [after](../phone-previews/m6/after/08_settings_1920x1080.png)
- Service unavailable: [before](../phone-previews/m6/before/23_messages_no_service_1920x1080.png) / [after](../phone-previews/m6/after/23_messages_no_service_1920x1080.png)
- Welcome: [final crop](../phone-previews/m6/after/m5_0_welcome_1920x1080_zoom2x.png)

The baseline preview set predates M5 setup screens, so a truthful before capture for Welcome was unavailable. No synthetic “before” image was invented.

## Layout and scale validation

The D3D9 preview harness rendered all states with zero write failures at 1280×720, 1360×768, 1600×900, 1920×1080, 2560×1440 and the retained 1024×768 compatibility case. It also rendered the focused 2560×1080 ultrawide cases. Home was rendered at UI scale 0.80 and 1.25 at 1920×1080. Total: **214 scenes / 418 PNG files, 0 failures**.

The pure layout test covers these resolutions plus 3440×1440, 5120×1440 and 2560×1600 across clamped scale inputs 0.75–1.35. It verifies body bounds, reserved HUD band, nested screen/content regions, contiguous status/content/navigation geometry, ultrawide anchoring and opening/closing translation.

This is offline geometry and rendering evidence. Mouse-coordinate mapping and real GTA resize/fullscreen behavior still require the controlled runtime checklist.

## Performance

The renderer continues to use fixed storage and immediate draw-list geometry. M6 adds no texture creation, network work, file I/O, worker thread or blocking call to the frame path. The shared card helper only emits vertices.

At 1920×1080, the final 2,000-frame CPU draw-list measurement for Home was 50.6 µs per frame including ImGui NewFrame/EndFrame. The complete per-screen and per-resolution measurements are preserved in the generated `report.txt`. These are offline CPU measurements, not GTA frame-time or GPU certification.

## Build and regression results

- Separate Win32/x86 M6 client build: PASS (`PE machine 0x014C`).
- Separate Win32/x86 preview build: PASS (`PE machine 0x014C`).
- Preview generation: 214 scenes, 0 failed.
- 15 offline C++ regression executables: **3,306 checks, 0 failures**.
- Coverage includes PHONE layout/controller/model, M2/M3/M4F/M5, PHONE protocol/backend/worker/Gateway, native API, SKR-X dispatch, VEHICLES and GPS/navigation/routing.
- GPS routing test used the locally available GTA road graph and reported 0 skipped sections. No GTA executable was launched.

Full console evidence is saved in `out/phone-m6-tests/results.txt`; visual measurements are in `docs/phone-previews/m6/after/report.txt`.

## Artifacts and SHA-256

| Artifact | Size | Architecture | SHA-256 |
|---|---:|---|---|
| `out/phone-m6-client/Release/SKR.Client.dll` | 1,030,656 bytes | x86 / 0x014C | `7d53e3d08c53501e7f283577841947de4dff35d53e791a3967116466dd4101da` |
| `out/phone-m6-tests/Release/SKRPhonePreview.exe` | 385,536 bytes | x86 / 0x014C | `aa71cde7bd71caa20d966aacd52226fcfd4716c72bb700fe5dd5bd1a0652f387` |

The build lives in a dedicated `phone-m6-*` directory. It was not copied over an OP215 or certified distribution.

## Known limits and runtime verdict

No clipping or overlap was observed in the generated resolution/scale matrix. The preview harness uses the production renderer and D3D9 ImGui backend, but representative offline data and an operator screenshot as backdrop. It does not exercise actual GTA input ownership, focus, fullscreen transitions, D3D9 lost-device/reset behavior, real server latency, notification timing under gameplay load, or end-to-end M5.8 transport.

**GO** for an operator-controlled two-client GTA/open.mp test of activation, SMS/unreads, contacts, real GPS state, Settings persistence, mouse/keyboard hitboxes, resize/scale, alt-tab/reset and closed-phone gameplay input. **NO-GO** for production certification until those runtime checks pass and the wider release compatibility gates are resolved.
