# P1 runtime verification — 2026-10-19

Development build only; acceptance incomplete.

Implemented: bounded launch IPC v2 with receipt acknowledgement; remote thread result checks; non-overwriting temporary startup-file moves with restoration before initialization; external launcher returns to lobby after game exit. IPC v2 requires matching launcher/client binaries.

Validation: Win32 Release build succeeded; IPC 18 checks passed; lobby settings 43 checks passed; packaged WebView2 smoke exited 0. Snapshot before update: out/external-lobby-before-p1. Updated pair: out/external-lobby.

Real probe: GTA installation selected by user, localhost:7777 using server-runtime/skr-server.exe. PID 37644, 19:14 local time. LaunchGame returned Success. Server recorded SKR_Test join, client initialized and spawn; GTA exited with code 0.

USER OBSERVATION OVERRIDES EXIT-CODE INTERPRETATION: SA-MP displayed "SA-MP has crashed", but gameplay continued. This is NOT a clean successful launch.

Evidence: GTA SKR/crash_trace.log recorded access violation C0000005 at 60B188C6 (no mapped module), thread 2440, tick 424189250. CEF client.log identifies the same initialization thread and ends after the D3D9 module path message, before the expected hooks-installed message. This narrows investigation to CEF rendering bootstrap; it does not establish the root cause. Raw stack-word scans are not trustworthy symbolic stack traces. HUD/CEF functioning remains unverified.

Open: diagnose startup exception; verify lobby return visually; repeat real launch/exit cycles; recover interrupted startup-file transactions; reconcile dependency lockfile with dirty local checkouts. No R2 certification or production authentication claim. Existing gate authorizes development account 0; ticket validation is still TODO. No GTA files or CEF binaries replaced by this probe.
