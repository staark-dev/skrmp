# T03-M1 — SKR-X VehicleReady per-client acknowledgments

## Root cause (`ready=no` for rendered 612/613)
The acknowledgment was **never sent**. The server side existed and was correlated correctly.
- `client/skrx/skrx_vehicles_client.cpp` `SendVehicleReady()` was a stub. It only logged
  "(queued)", because the gateway send path (`SendOnRpc240`) was private to `skrx_client.cpp`
  (the M2.1-B TODO).
- Nothing called it: MSVC C4505 reported it as unreferenced and removed.
- The server (`VehicleManager::OnPlayerReady` / `IsPlayerReady`) already keyed readiness on player,
  pool ID, model and generation, so `/vm` correctly reported `ready=no`.

Found while tracing:
- Client `vehicles::Reset()` was never called. Assignments from the previous connection survived a reconnect.
- The server kept a binding after the player's instance streamed out, and after `SKRX_DestroyVehicle`.

## Readiness model
- **ResourceReady**: Operation 215 state `ready` (mapped, streamed, D2B3 validated). Client-local only.
- **VehicleReady** (per player, per instance): the client's `CVehiclePool::Create` hook applied the custom
  model, and samp.dll's original `Create` returned success for that pool ID and assignment generation.
  A proxy instance is never VehicleReady.

## Protocol (SKR-X VEHICLES v1, unchanged wire format)
Op 4 `VehicleReady` (C→S): `u16 pool, u16 model, u32 generation, u8 status`. Status values:
- 0 = custom model applied;
- 1 = asset missing (reserved);
- 2 = re-created as proxy (retracts an earlier ack).

Client (`skrx_vehicles_client.cpp`):
- The hook queues an ack after the original `Create` succeeds (status 0), or status 2 only if this
  assignment was previously acked.
- The queue is sent from `skrx::Tick` (game tick, not inside samp.dll's RPC handler), through the new
  `skrx::SendGateway`, with at most 3 attempts.
- A changed model or generation, a revoke, a respawn generation, a reused slot, or a disconnect
  (`skrx::Reset` → `vehicles::Reset`) clears pending and acked state.
- The 614 proxy, and any vehicle created as its proxy, sends nothing.

Server (Gate):
- `OnPlayerReady` returns a `ReadyResult`, logged per message:
  - accepted, duplicate (idempotent), not-ready;
  - invalid-player / -status / -model;
  - unknown-vehicle, model-mismatch, stale-generation.
- An ack for a vehicle that is not streamed in for that player is dropped (`not-streamed-in`).
- `onVehicleStreamOut` clears that player's binding; `UntrackVehicle` clears all bindings for the pool ID
  (reuse); disconnect → `ForgetPlayer`.
- Generations are globally monotonic, so an ack for a reused pool ID can never match.
- `SKRX_IsVehicleReadyForPlayer` and the `/vm` output are unchanged; vanilla clients never ack, so they stay `ready=no`.

## Staging
```
python -I tools/t03_stage_test_runtime.py
```
- Refuses while `skr-server.exe` or `gta_sa.exe` runs.
- Backs up and replaces `server-runtime-branded-test/components/Gate.dll`.
- Verifies the client DLL and certified custom.img in place.
- Pawn: natives unchanged; `skrx_vehicles_test.pwn` compiles cleanly; no .amx change.

## Two-client operator procedure (pending)
1. Stop the test server, run the staging script, start the test server.
2. **A.** Client A spawns 612. `/vm <id>` should show `true=612 stock=400 custom=yes ready=yes`.
   The server log shows `VehicleReady … -> accepted`. No freeze.
3. **B.** Client B connects near the vehicle. Both render the Volvo. Driving syncs on both.
   `/vm` reads `ready=yes` per player, and a player who hasn't streamed it in reads `no`.
4. **C.** Repeat with 613 (proxy 475).
5. **D.** 614: `/vm` shows `ready=no`, the proxy is 560, and there is no VehicleReady in the server log.
6. **E.** A vanilla 0.3.7 client sees stock proxies and `/vm` shows `ready=no` for it.
7. **F. Lifecycle:**
   - drive away → stream-out → `ready=no`; return → `ready=yes`;
   - destroy and re-create (same pool ID) → new generation, `ready=no` until re-acked;
   - reconnect → `ready=no` until the new session acks;
   - duplicate acks log `duplicate`.
