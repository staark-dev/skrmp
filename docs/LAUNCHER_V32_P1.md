# Launcher V3.2 — P1 minimal patch, 2026-10-20

Scope: preserved existing V3.2 and all unrelated local work. No native source, IPC, CMake configuration, or GTA installation files changed in this increment.

Changes:
- client/lobby/lobby.css: Exit panel uses rgba(24,27,25,0.55), subtle border/shadow and backdrop blur for readable text; one canonical component section with its responsive rules; removed redundant 650px text override.
- client/lobby/lobby.js: removed trailing blank lines only to pass diff whitespace validation. No event or behavior changes.
- Backup of original UI: out/ui-backups/v3.2-20261020-094109.

Deployment: existing SKRLobbyAssets always runs and is a dependency of both SKRClient and external SKRLauncher. Build with `cmake --build --preset external-lobby -j 4`; for assets only use `cmake --build build-external-lobby --config Release --target SKRLobbyAssets`. Final SHA-256 comparison confirms HTML/CSS/JS source equals build-external-lobby/Release/SKR/lobby. Other out packages are not updated by this target.

Validation:
- Requested full build: success (sandbox attempt stalled and was stopped; approved unsandboxed build completed).
- Lobby tests: 43 passed; IPC: 18 passed.
- node --check and git diff --check: success (line-ending warnings only).
- SKRLauncher --lobby-smoke: exit 0, real WebView2; no GTA started.
- Chrome browser QA, synthetic native bridge/catalog: public Discover selection excludes provided local/private/invalid candidates; search; favorites filtering and reload persistence; header totals from supplied data including + for partial player counts; one server list scrollbar at 1440x900; Quit emits once, Cancel emits no Quit; loading/idle rendering; zero page errors.
- Exit panel bounds checked at 1920x1080, 1440x900, 800x600 and 390x844. Screenshots and reproducible browser script: out/ui-qa-v32. Desktop screenshot inspected visually after adding blur. Full server layout at phone width still clips horizontally; no unrelated mobile redesign performed.

Limits: synthetic tests do not validate live catalog availability or native personal-server persistence. Existing native settings tests passed, but no new end-to-end profile writes performed. Discover refresh/navigation stability was inspected in source; not a statistical random-selection test. P2/P3 remain open: black flash not investigated dynamically, no repeated GTA runs, CEF/HUD not certified. Latest inspected historical CEF log ends with join retries then Exit SA-MP; this does not establish crash remediation or functioning HUD. Handoff log records frame confirmation and return to lobby, not proof of seamless display.
