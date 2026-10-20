# SKR Enhanced — checkpoint 2026-10-20

Stopped implementation at the owner's request. All edits are retained, including
pre-existing changes. No commits were created; nothing was published or signed.
This is a development checkpoint, not an accepted release.

## A. FPS

Files changed/added for FPS:

- `client/render-d3d9/fps_engine.cpp` and `.h`: scene bracketing, explicit render
  state, backbuffer viewport, state/RT0/depth restoration, render HRESULT and
  timing diagnostics, reset sampling. Existing ShowCounter and Cap retained.
- `client/render-d3d9/d3d9_hook.cpp`: hook diagnostics and optional SKR-owned
  before/after Present and before Reset exports.
- `deps/omp-cef/src/client/core/rendering/render_manager.cpp`: calls that optional
  bridge around its real-device Present; preserves its original Present result.
- `tests/unit/test_fps_d3d9.cpp` and root `CMakeLists.txt`: actual D3D9 render/readback
  and restoration test.

The isolated test passed twice across a Reset: visible backbuffer pixels, restored
RT0, viewport and pipeline state, successful device Reset (10 assertions total).
This does not test all possible MRT configurations or the GTA/CEF render chain.

Runtime evidence from the earlier build: Present and Reset hooks ran during
loading; periodic Present diagnostics stopped after connection. The supplied
`skr-005.png` shows CEF HUD but no FPS. Missing BeginScene was a concrete defect in
the isolated drawing code; the exact runtime hook replacement/bypass is still
being investigated. The optional CEF bridge is a compiled candidate correction.

**FPS is not resolved/accepted.** The final bridge has not been installed or
tested in GTA. Must verify visible counter, plausible measured FPS, ShowCounter,
Cap, CEF/chat/radar, Alt+Tab, lost device and Reset together.

## B. Server hybrid compatibility

Files:

- `server/skr-gate-omp/skr_gate_component.cpp`, `.hpp`, `CMakeLists.txt`
- `shared/protocol/enhanced.h`, `join_validation.h`, `query_rules.h`
- `tests/unit/test_enhanced.cpp`

Removed prefix-based rejection and automatic authorization on connect. Standard
connections do not receive the old authentication challenge automatically.
Capability mode is separate from protected `IsAuthorized`. Existing standalone
`server/skr-gate/gate.cpp` authentication logic was not weakened or changed.

RPC25 structural validation preserves read offset, checks every length against
remaining bits, and leaves semantic validation/optional open.mp fields to core.
No version-string copy into a 65-byte buffer remains.

New SKR-owned envelope on existing RPC240/241: 8 bytes, marker E0, step
(Hello=1/Offer=2/Confirm=3), protocol 1, capability mask 07, little-endian u32
connection token. Hello uses token zero; server offers a generation token; client
echoes it; server then sends existing session info. This is not authentication.
Candidate expires after 10 seconds to Standard; disconnect discards state.
Existing v1 data RPC240–243 remain gated on negotiated capabilities.

Discovery uses existing query rule `skr_client=2`. No additional UDP discovery
protocol. Old value 1 is incompatible with this negotiated version. A configurable
`skr_only` policy is not implemented at this checkpoint.

Pinned provenance checked locally:

- open.mp `3d20f3dc0e26917264cdc8585c65913e080a143c`,
  `Shared/NetCode/core.hpp` PlayerConnect.
- SDK `08cc69a32d96c0eb8fe063d26c78259e54ca853f`.
- network `f223fbd90e414f6c35f1d20cf916314ea5dcff6f`.
- 154 distinct literal RPC IDs found in pinned open.mp NetCode, maximum 185;
  none collide with 240–243. This is not a guarantee for unrelated plugins.
  Full client/third-party collision validation remains pending.
- omp-cef base `fb5097b866c7cf51db774f96b7402cb260f8bb16`, with existing local
  modifications plus the two changes listed in this report.

`SKREnhancedTests`: 588301 checks, zero failures. Uses real pinned NetworkBitStream
for lengths 0–255, every bit truncation and read-offset restoration; also tests
incompatible version/capabilities, premature/wrong confirmation, duplicate hello,
timeout and cleared reconnection state. These are component tests, not actual
standard-client interoperability tests.

New SKRGate was deployed to the already-authorized local runtime, with backup:
`out/rollback/enhanced-20261020-150219/SKRGate.dll`.
Runtime logged a hybrid connection without automatic authentication, then a
manual user disconnect. This did not validate a completed Enhanced handshake.

## C. Client and launcher compatibility

Files:

- `apps/launcher/game_launcher.h`, `.cpp`, `lobby_window.cpp`
- `shared/ipc/launch_params.h` (version 4, discovery hint)
- `client/bootstrap/dllmain.cpp`
- `client/game-state/game_state.h`, `.cpp`
- `client/lobby/server_query.cpp`, `lobby.js`, `lobby_manager.h`, `.cpp`,
  `external_lobby_manager.cpp`
- `client/intro/launch_cover.cpp`
- `deps/omp-cef/src/client/core/app.cpp`

Fresh native query before launch controls whether to attempt negotiation. Server
Details gains Runtime text using existing layout. In-game session info is accepted
only after negotiation. Disconnect resets mode. Standard loading can finish
without requiring a CEF-ready signal. The modified omp-cef App gates its separate
extension connection on negotiated SKR mode when the SKR export is present.
Protected launcher authentication is not supplied by these changes.

Known gaps: complete end-to-end negotiation unverified; endpoint changes within
one game process need explicit testing (discovery hint is from launcher startup);
legacy embedded-lobby path has not been runtime-tested; existing disabled radar
geometry transmission was not re-enabled. Version 4 IPC requires matching
launcher and client. Configuration/lockfile still contain pre-existing UNKNOWN
release fields; no release certification is claimed.

## Build and remaining validation

Successfully compiled Release:

- `build-external-lobby/Release/SKRLauncher.exe`
- `build-external-lobby/Release/SKR.Client.dll` (x86)
- `build-gate/Release/SKRGate.dll` (separate server build)
- `deps/omp-cef/build/client-x86/src/client/core/Release/client.dll` (x86)

Additional tests: IPC 18/18, lobby settings 43/43. No final launcher visual test
or complete protocol integration matrix was executed.

Final CEF build SHA256:
`1FAB44D51E9F75275E2328891625E06C22D7D118AB1C3C906A92241475EAB499`.
Installed GTA CEF DLL still differs:
`743293994745F3AB46E810735ACA78D50FBB224D6747EA171826BF4C774EAEE6`.
The final CEF build was **not deployed**. Do not test only the new launcher/client
and infer that the new CEF bridge or network gate is running.

Requires GTA and real client/server sessions:

- standard SA-MP → standard open.mp;
- SKR → standard open.mp;
- standard SA-MP → hybrid runtime;
- SKR → hybrid runtime with completed negotiation;
- incompatible client, absent/interrupted handshake;
- reconnect, timeout, invalid/premature RPCs on the actual transport;
- visible FPS plus functional CEF after Alt+Tab/Reset.

Build logs: `out/enhanced-client-build.log`, `out/enhanced-cef-build.log`,
`out/enhanced-gate-build.log`. Server build emitted warnings from dependency
headers. No build errors were observed.
