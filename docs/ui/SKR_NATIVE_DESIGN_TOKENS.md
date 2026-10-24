# SKR Enhanced — Design Tokens for Native UI (ESC menu, radar frame, HUD)

Source of truth: launcher `client/lobby/lobby.css` (identical structure to the reviewed
`lobby.css` attachment). Values below are copied from CSS properties, not class names
(`.button--gold` is green).

## 0. OPEN DECISION — two palettes exist in the CSS

`lobby.css` declares `:root` twice. Both are unconditional; the later one wins in the browser.

| Token | Block A — line 3 (base) | Block B — line 326 "SKR ENHANCED 1.0 / NEW VISUAL SYSTEM" (effective) |
|---|---|---|
| --bg | #090a0a | #070b11 |
| --tile | #18191a | #101821 |
| --tile2 | #202122 | #17212b |
| --panel | #141515 | #101821 |
| --text | #f2f3f2 | #f4f8fa |
| --muted | #999e9b | #94a3b2 |
| --dim | #626966 | #6d8391 |
| --line | #2a2c2b | #263747 |
| --green | #75ba52 | #a8ee65 |
| --green-hover | #8ace63 | #c1ff84 |
| --danger | #b96969 | #f08a83 |

Block B is followed by ~280 lines of overrides in the same navy/lime system (e.g. body radial
`#17364b → #070b11`, header border `#253544`, `.button--gold` `#aeef72` / hover `#ccff9c`,
`.nav-button.active` text `#baff82` on `#a8ee6516` + 1px `#a8ee6535`, `.category.active`
`#213932` / border `#42675d` / inset 3px `#a8ee65`, `.setting-row` border `#233743`,
switch off `#334653`/`#49606b`, on `#678f4c`/`#9ccf74`).
The task brief listed Block A as "verified".

**DECISION (user, 2026-10-23): native UI follows Block B — the palette the launcher actually renders.**

Brand mark (Block B): `.brand-symbol` 34–42px skewed (-15deg) tile, gradient `#adf179 → #66bd8e`,
three dark bars `#0b1822` (heights 17/25/12px, width 5px, skew -8deg), glow `#a8ee6530`.
Eyebrow `#90bfb4` (letter-spacing 2px); settings title `#e5eef2`; hint `#829aa7`;
panel-index `#698291`; details panel `#121e28` / border `#2e4956` / radius 12.

## 1. Typography (both blocks)
- Body: Inter → 'Segoe UI' → Arial, 14px/1.45. Display/brand: Bahnschrift (900, tight tracking).
- Eyebrow/kicker: 10px, weight 800, letter-spacing 1.5px, uppercase, muted-green
  (A `#7d8f7d`, B `#90bfb4`).
- Page title: 37px/850, -1.3px tracking; trailing `.title-dot` in --green.
- Section heading: 14px/850. Setting title 12px/750; hint 10px muted.
- Nav button: 11–12px/650, radius 5–7px.
- Footer: 10px muted; `kbd` chip 9px/700, 1px border `#40463f`, radius 3px.
Native mapping: headings/nav/buttons → Bahnschrift fallback face; body → Segoe UI face
(already loaded by the ImGui fallback path).

## 2. Components → native primitives
| Launcher component | CSS | Native primitive |
|---|---|---|
| Nav tab active | text near-white, bg tile2-ish, (B: lime tint + 1px lime inset) | `DrawMainTab` |
| Settings category active | inset 3px green bar, green-tinted bg/border | `DrawSubTab` / row focus |
| Primary button (`.button--gold`) | bg --green, text near-black `#0b120b`, radius 5–7 | `StyledButton(kPrimary)` |
| Secondary button | bg `#242624`, border `#454a45`, text `#d2d5d2` | `StyledButton(kSecondary)` |
| Outline/pill | transparent, border `#565b56`, radius 40px | not used natively |
| Danger | --danger | `StyledButton(kDanger)` for Exit/Disconnect |
| Setting row | 2-col grid: label/hint left, control 140–240px right, 1px bottom line | `MakeRow/BeginRow` |
| Range | track neutral, accent --green, value text --green | `DrawSettingSlider` |
| Switch | pill 43×24, off neutral, on green-tinted | `DrawSettingToggle` |
| Focus ring | `outline: 2px solid --green` | keyboard focus outline |
| Modal | backdrop `#050705d9`, panel `#1c201c`, border `#455044`, radius 8, eyebrow + h2 24px + p 12px muted, actions right-aligned (Cancel secondary, confirm primary) | `DrawModal` |
| Exit confirm | Cancel = green primary, Exit = neutral outline | Exit dialog |
| Footer | 51px, top border, kbd chips + muted labels | footer band |

Radius: ImGui DrawList supports rounded rects; native uses 4–6 px (scaled) to match 5–8px CSS.

## 3. Native usage rules (both palettes)
- Accent (--green) only for: active tab indicator, focus ring, primary button, slider fill,
  ON state, status "connected", brand dot. Inactive = neutral greys.
- Red (--danger) only for Exit/Disconnect and error text. Health stays red, armour blue (HUD).
- No gold anywhere.

## 4. Responsive layout rules (ESC)
- Shell padding: clamp(20px, 2.55vw, 50px) (A) / clamp(22px, 3vw, 62px) (B) → native:
  `clamp(20, 2.6% of width, 62) * uiScale`.
- Header full width (86–90px), footer full width (51px).
- Content uses the full width between shell paddings; individual controls keep max widths
  (setting control column 140–240px CSS → scaled), multi-column panels on wide screens.

## 5. Implementation status (2026-10-23) — syntax-checked, NOT MSVC-built, NOT runtime-tested
Checkpoints: _backups/theme-pre, _backups/theme-syntaxchecked.
- pause_renderer.cpp: `colors::` now holds Block B tokens only (no gold); `Btn::{Primary,
  Secondary,Danger}` roles; rounded buttons with 2px accent focus ring; launcher switch
  (pill + knob + ON/OFF), accent slider, input-style stepper box; `DrawCategory` (settings
  categories), pill `DrawSubTab`; `PageHeading` (eyebrow + title + accent dot), `DrawPanel`,
  `SplitColumns`, `DrawInfoCard`, `DrawVitalsCard`, `DrawBrandMark`.
- Shell: full width (padding clamp(3% width, 16..62 scaled)); header 88 with brand mark, pill
  nav, real connection state + player chip (dropped when space is short); footer with kbd chips
  and EXIT GAME (danger).
- Pages: GAME (heading + Resume/Disconnect | SESSION + PLAYER cards with live health/armour via
  radar_hud::ReadPlayerVitals), SETTINGS (category column | panel with heading, index, rows,
  VANILLA SETTINGS; narrow = pill sub-tabs), SERVERS (heading + Return to lobby | CURRENT SERVER),
  ABOUT (brand + version | CONTROLS + CLIENT). Dialog: CONFIRMATION eyebrow, Cancel primary
  (default focus), destructive red.
- radar_hud.cpp: panel #101821/200, border #293d4a, divider #263747, accent line #a8ee65.
Open: launcher shows "V1.0.0"; client build version (CMake PROJECT_VERSION) is 0.1.0.
