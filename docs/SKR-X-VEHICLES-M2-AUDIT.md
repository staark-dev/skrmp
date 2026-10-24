# SKR-X VEHICLES M2 — Architecture & Feasibility Audit

**Date:** 2026-10-24
**Prerequisite:** SKR-X M1 verified (CORE Ping/Pong roundtrip confirmed)
**Scope:** Design-only audit. No binaries modified, no server changes, no GTA launch.

---

## A. Architecture Audit — Existing Vehicle Infrastructure

### A.1 Server-side: open.mp vehicle pool

**Source:** `deps/omp-sdk/include/Server/Components/Vehicles/vehicles.hpp`

| Item | Detail |
|------|--------|
| Pool interface | `IVehiclesComponent : IPoolComponent<IVehicle>` |
| Create API | `create(bool isStatic, int modelID, Vector3 pos, float Z, int col1, int col2, Seconds respawnDelay, bool addSiren)` |
| Model validation | `isValidVehicleModel()` at `vehicles.hpp:469` — **rejects anything outside 400–611** |
| Model count constant | `MAX_VEHICLE_MODELS = 212` (611 − 400 + 1) in `values.hpp:13` |
| Models array | `StaticArray<uint8_t, 212>` — indexed by `modelID - 400`, tracks instance counts |
| Vehicle ID | `uint16_t` pool slot, max 2000 (SA-MP limit) |
| Sync packets | `VehicleDriverSyncPacket`: VehicleID(u16) + Position + Velocity + Rotation + Health + Keys |

**Critical:** open.mp's `create()` validates model IDs against 400–611. Custom model IDs (612+) **cannot** pass through `create()` as-is. The server will reject them.

### A.2 Client-side: SA-MP R5 vehicle pool (samp.dll)

SA-MP R5's client-side vehicle pool (internal to samp.dll, not in SKR source) works as follows:

1. Server sends `WorldVehicleAdd` RPC (SA-MP internal RPC ~164) with: model ID, position, rotation, colors, health, interior, etc.
2. samp.dll's `CVehiclePool::Add()` stores the model ID and parameters.
3. When the vehicle enters the player's streaming range, samp.dll calls `CStreaming::RequestModel(modelId, flags)` then constructs the appropriate vehicle class (CAutomobile, CBike, etc.) via GTA's constructor.
4. The model ID determines which `CVehicleModelInfo` entry in the GTA `CModelInfo::ms_modelInfoPtrs` array (at `0xA9B0C8`) is used.

**Model validation in samp.dll:** SA-MP R5 accepts whatever model ID the server sends in `WorldVehicleAdd`. It does not independently validate 400–611 — it trusts the server. However, if the model has no `CVehicleModelInfo` entry in the GTA model array, `CStreaming::RequestModel` will read uninitialized memory and likely crash.

### A.3 Client-side: OP215 custom vehicle system (local-only)

**Source:** `client/vehicle/custom_vehicle.cpp`

| Item | Detail |
|------|--------|
| Model IDs | 612 (skr_xc60, donor=400), 613 (skr_m4gts, donor=475) |
| Registration | Clones donor's `CVehicleModelInfo` (0x308 bytes), patches hash/txd/modelId/rwObject |
| TXD | Allocates new `CTxdStore` slot, links to model's TXD name |
| Streaming | Registers `CStreamingInfo` entries for both DFF and TXD at model ID's streaming slots |
| IMG | Adds `SKR\op215\skr_vehicles.img` via `CStreaming::AddImageToList()` |
| Spawning | `malloc(0x988)` + direct `CAutomobile::CAutomobile(modelId, createdBy)` via inline asm |
| World add | `CWorld::Add()` directly |
| **Multiplayer** | **NONE.** Completely bypasses SA-MP vehicle pool. Other players cannot see these vehicles. |

**Key OP215 addresses (GTA SA 1.0 US x86):**

| Address | Function |
|---------|----------|
| `0xA9B0C8` | `CModelInfo::ms_modelInfoPtrs` — model info pointer array |
| `0x4087E0` | `CStreaming::RequestModel(int, int)` |
| `0x40EA10` | `CStreaming::LoadAllRequestedModels(bool)` |
| `0x407610` | `CStreaming::AddImageToList(const char*, bool)` |
| `0x4044C0` | `CStreaming::ConvertBufferToObject(int, ...)` (for CStreamingInfo setup) |
| `0x4089A0` | `CStreaming::HasModelLoaded(int)` |
| `0x6B0A90` | `CAutomobile::CAutomobile(int modelId, uint8_t createdBy)` |
| `0x563220` | `CWorld::Add(CEntity*)` |
| `0x731C80` | `CTxdStore::AddTxdSlot(const char*)` |

---

## B. Proxy Model Feasibility Analysis

### B.1 The proxy approach

Instead of passing custom model IDs (612+) through SA-MP's vehicle pool (which both open.mp and samp.dll would reject or crash on), use a **proxy model**:

1. **Server creates a stock vehicle** (e.g., model 560 = Sultan) using standard `IVehiclesComponent::create()`.
2. **SA-MP sync works normally** — all position, velocity, driver, passenger, health, damage sync uses pool-slot IDs, NOT model IDs. `VehicleDriverSyncPacket` carries `VehicleID(u16)`, not model.
3. **SKR-X VEHICLES namespace** tells enhanced clients: "vehicle pool ID `X` is actually custom model 612."
4. **Enhanced clients swap the visual model** client-side: replace the proxy's rendering with the custom model.
5. **Non-enhanced clients see the proxy** (Sultan) — graceful degradation.

### B.2 Why this works

| Sync mechanism | What it carries | Model-dependent? |
|----------------|----------------|-------------------|
| `VehicleDriverSyncPacket` | VehicleID, Position, Velocity, Rotation, Health, Keys | **No** — pool slot only |
| `VehiclePassengerSyncPacket` | VehicleID, SeatID, Position, Keys | **No** |
| `VehicleUnoccupiedSyncPacket` | VehicleID, SeatID, Position, Velocity, AngularVelocity, Health | **No** |
| `VehicleTrailerSyncPacket` | VehicleID, PlayerID, Position, Quat, Velocity, TurnVelocity | **No** |
| `streamInForPlayer` / `streamOutForPlayer` | Sends model ID to client | **Yes** — but one-time |

The only model-dependent sync is the initial `WorldVehicleAdd` RPC when the vehicle first streams in. All ongoing sync (every tick) is model-agnostic — it only references the vehicle pool slot ID.

### B.3 Model swap mechanism (client-side)

When an enhanced client receives `VehicleAssign` from VEHICLES namespace saying "pool ID 42 = custom model 612":

1. **Before the vehicle streams in:** Register custom model 612 in `CModelInfo::ms_modelInfoPtrs[612]` using OP215's existing `RegisterVehicle()` cloning. This makes model 612 a valid GTA model.

2. **On stream-in:** samp.dll creates the vehicle with model 560 (Sultan). We have two options:

   **Option A — Post-creation model swap:** Let samp.dll create the Sultan normally, then immediately swap the vehicle's model info pointer and RenderWare object to model 612. This requires:
   - Hook `CVehiclePool::NotifyVehicleCreated` or similar callback
   - Or: poll new vehicles in `Tick()` and swap before the next frame renders
   - Risk: brief visual flicker (one frame of Sultan before swap)

   **Option B — Pre-creation hook:** Hook samp.dll's `WorldVehicleAdd` RPC handler to replace the model ID before it reaches `CAutomobile::CAutomobile`. This requires:
   - Finding and hooking samp.dll R5's `WorldVehicleAdd` handler
   - Rewriting the model ID in the bitstream before samp.dll processes it
   - Risk: samp.dll internal offset dependency (R5-specific), but R5 offsets are already known and pinned

   **Recommendation: Option B (pre-creation hook).** No flicker, cleaner, and the model is correct from construction. R5 offsets are already stable in the SKR codebase.

### B.4 Donor model selection

The proxy model the server creates must be carefully chosen per custom vehicle:

| Custom | Donor (OP215) | Recommended Proxy | Rationale |
|--------|---------------|-------------------|-----------|
| 612 (skr_xc60) | 400 (Landstalker) | 400 (Landstalker) | Same vehicle class (4-door SUV), similar physics |
| 613 (skr_m4gts) | 475 (Sabre) | 475 (Sabre) | Same vehicle class (2-door coupe), similar physics |
| 614+ (future) | TBD | Same class as donor | Must match seat count and vehicle type |

The proxy model determines:
- **Physics/handling data** (for non-enhanced clients and server-side collision)
- **Seat count** (must match: if custom vehicle has 4 seats, proxy must too)
- **Vehicle type** (automobile, bike, boat, etc. — determines which constructor samp.dll calls)

---

## C. Protocol Specification — VEHICLES Namespace

### C.1 Namespace allocation

Already reserved in `shared/protocol/skrx_gateway.h:27`:
```
Namespace::Vehicles = 1
```

### C.2 Version

```cpp
constexpr uint8_t kVehiclesVersion = 1;
```

### C.3 Operations

| Op | Direction | Name | Payload |
|----|-----------|------|---------|
| 1 | S → C | `VehicleAssign` | vehiclePoolId(u16) + customModelId(u16) + proxyModelId(u16) + donorModelId(u16) + flags(u8) |
| 2 | S → C | `VehicleRevoke` | vehiclePoolId(u16) |
| 3 | S → C | `VehicleRegistry` | count(u8) + array of { customModelId(u16) + proxyModelId(u16) + donorModelId(u16) + internalName(u8 len + chars) } |
| 4 | C → S | `VehicleReady` | customModelId(u16) + status(u8: 0=loaded, 1=failed) |
| 5 | S → C | `VehicleBatchAssign` | count(u8) + array of { vehiclePoolId(u16) + customModelId(u16) } |

#### Op 1 — VehicleAssign (S → C)

Sent when the server creates or re-assigns a custom vehicle for the player's streaming range. The client:
1. Checks if `customModelId` is in its local registry.
2. If not registered yet: registers it using `donorModelId` as the clone source (OP215 `RegisterVehicle` path).
3. If model assets not loaded: begins streaming (`CStreaming::RequestModel`).
4. When the vehicle's pool-slot vehicle streams in via samp.dll, intercepts and swaps model to `customModelId`.
5. Sends `VehicleReady` (Op 4) to confirm.

#### Op 2 — VehicleRevoke (S → C)

Sent when the vehicle should revert to its proxy model (e.g., player-specific permission revoked, custom model unloaded). The client swaps back to the proxy model.

#### Op 3 — VehicleRegistry (S → C)

Sent once after gateway negotiation. Tells the client which custom model IDs exist on this server, their proxy models, and donor models. The client pre-registers `CVehicleModelInfo` entries for all listed models. This avoids the latency of register-on-demand.

#### Op 4 — VehicleReady (C → S)

Confirmation that the client has the custom model loaded and ready. The server can use this to track which players have which models available.

#### Op 5 — VehicleBatchAssign (S → C)

When a player joins or teleports to an area with many custom vehicles, batch-assign avoids a flood of individual Op 1 messages. Uses the same logic as Op 1 per entry, but `proxyModelId` and `donorModelId` are looked up from the registry (Op 3).

### C.4 Wire encoding (example: VehicleAssign)

```
[gateway header: E0 0A 01 01 0009]   -- marker, step=10, ns=Vehicles, ver=1, payloadLen=9
[01]                                   -- op = VehicleAssign
[2A 00]                                -- vehiclePoolId = 42
[64 02]                                -- customModelId = 612
[30 02]                                -- proxyModelId = 560
[90 01]                                -- donorModelId = 400
[00]                                   -- flags = 0
```

Total: 6 (gateway header) + 9 (payload) = 15 bytes per assignment.

### C.5 Capability gating

VEHICLES namespace requires:
- `CapGateway` (0x10) in API Welcome (already wired in M1.1)
- Server must have custom vehicles configured
- Client must be Enhanced + API Ready + Gateway Ready

Non-enhanced clients: see proxy models only, no protocol overhead.

---

## D. File-by-File Implementation Plan

### D.1 Shared protocol (no build target — header only)

| File | Action | Content |
|------|--------|---------|
| `shared/protocol/skrx_gateway.h` | **Modify** | Add `kVehiclesVersion`, VEHICLES ops enum, `VehicleAssign`/`VehicleRevoke`/`VehicleRegistry`/`VehicleReady`/`VehicleBatchAssign` structs, encode/decode functions |
| `shared/protocol/skrx_vehicle.h` | **New** | Vehicle-specific types: `CustomVehicleDescriptor` (shared between server config and protocol), model ID validation helpers |

### D.2 Server (`server/skr-gate-omp/`)

| File | Action | Content |
|------|--------|---------|
| `skrx_gateway_server.cpp` | **Modify** | Add `case Namespace::Vehicles:` dispatch in `OnGatewayMessage` switch |
| `skrx_vehicles_server.cpp` | **New** | VEHICLES namespace handler: `OnVehicleReady`, vehicle registry management, assignment tracking per player |
| `skrx_vehicles_server.h` | **New** | Server vehicle registry: maps custom model IDs → proxy models + descriptors. Loaded from config. |
| `skr_gate_component.hpp` | **Modify** | Add VEHICLES handler declarations, vehicle registry member |
| `skr_gate_component.cpp` | **Modify** | Wire vehicle events: `onVehicleStreamIn` → send `VehicleAssign`, `onPlayerConnect` → send `VehicleRegistry` |
| `CMakeLists.txt` | **Modify** | Add new .cpp files |

### D.3 Client (`client/`)

| File | Action | Content |
|------|--------|---------|
| `client/skrx/skrx_vehicles.h` | **New** | Client-side vehicle model registry, pool-slot → custom-model mapping, swap state tracking |
| `client/skrx/skrx_vehicles.cpp` | **New** | `OnVehicleAssign`, `OnVehicleRevoke`, `OnVehicleRegistry` handlers; model registration (reuses OP215's `RegisterVehicle` logic); pool-slot swap manager |
| `client/skrx/skrx_client.cpp` | **Modify** | Add `Namespace::Vehicles` dispatch in `OnGatewayMessage` |
| `client/vehicle/custom_vehicle.h` | **Modify** | Extract `RegisterVehicle` into a reusable API: `RegisterCustomModel(int modelId, int donorId)` that sets up CVehicleModelInfo + CStreamingInfo without spawning |
| `client/vehicle/custom_vehicle.cpp` | **Modify** | Refactor: split registration from spawn. The F9 local spawn path calls both; the network path calls registration only (spawn handled by samp.dll + model swap) |
| `client/samp-hooks/samp_hooks.cpp` | **Modify** | Add `WorldVehicleAdd` RPC hook: intercept model ID, check vehicle registry, swap to custom model before samp.dll constructs the vehicle |
| `client/samp-hooks/samp_hooks.h` | **Modify** | Declare vehicle RPC hook API |
| `CMakeLists.txt` | **Modify** | Add new .cpp files |

### D.4 Server configuration

| File | Action | Content |
|------|--------|---------|
| `server-runtime-branded-test/skr-runtime.json` | **Modify** | Add `"skrx": { "vehicles": { "enabled": true, "models": [...] } }` |

### D.5 Tests

| File | Action | Content |
|------|--------|---------|
| `tests/unit/test_skrx.cpp` | **Modify** | Add VEHICLES codec tests: encode/decode all ops, registry round-trip, batch encoding |
| `tests/unit/test_skrx_vehicles.cpp` | **New** | Vehicle-specific tests: model registry, pool-slot mapping, proxy→custom resolution, edge cases (revoke nonexistent, double-assign) |

### D.6 Assets (client-side)

No new asset work needed for M2. OP215's existing `skr_vehicles.img` with models 612/613 DFF+TXD files are reused. The M2 protocol simply ensures these models are loaded when the server assigns them, rather than on F9 keypress.

---

## E. Risks and Mitigations

### E.1 Critical Risks

| # | Risk | Impact | Mitigation |
|---|------|--------|------------|
| 1 | **samp.dll R5 WorldVehicleAdd hook fragility** | Hook depends on samp.dll R5 internal offsets. SA-MP updates would break it. | SKR targets R5 only. open.mp + SKR Enhanced is the long-term client. Document the exact R5 offsets and add a version check at DLL attach. |
| 2 | **Model swap visual artifacts** | If model swap happens after first render frame: 1-frame flicker of proxy model | Use pre-creation hook (Option B). Register models in `CModelInfo` during `VehicleRegistry` (Op 3), before any vehicle streams in. |
| 3 | **Physics mismatch** | Custom vehicle visual doesn't match proxy's handling data | Use matching donors: SUV proxy for SUV custom, coupe proxy for coupe custom. Document that proxy must match vehicle type and seat count. Server-side enforcement. |
| 4 | **open.mp model array overflow** | `StaticArray<uint8_t, 212>` in `models()` can't track IDs 612+. Calling `models()[612-400]` = out of bounds. | Server NEVER calls `models()` for custom IDs. The server only knows proxy model IDs for its pool. Custom model IDs exist only in the VEHICLES namespace protocol, not in open.mp's vehicle API. |
| 5 | **Anti-cheat false positives** | Model swap on vehicle creation could trigger client-side AC. | SKR's own AC (`ProcessAcHeartbeat`) already knows about custom vehicles. Document that third-party ACs may flag this — SKR Enhanced is its own client. |

### E.2 Moderate Risks

| # | Risk | Impact | Mitigation |
|---|------|--------|------------|
| 6 | **Streaming distance mismatch** | samp.dll streams out the proxy vehicle, but custom model assets remain loaded | Track stream-in/out events. On stream-out: revert slot mapping. On stream-in: re-apply swap. |
| 7 | **Vehicle respawn** | Server respawns vehicle → new stream-in → needs re-swap | `VehicleAssign` is per-pool-slot, persists across respawns. Client re-applies on each stream-in. |
| 8 | **Component/mod incompatibility** | Tuning parts for proxy model don't apply to custom model | Disable vehicle component slots for custom vehicles (server-side: reject `addComponent` for proxy-swapped vehicles). |
| 9 | **Seat count mismatch** | Custom model has different seat layout than proxy | Enforce same seat count in server config validation. Proxy model must match custom model's seat count. |
| 10 | **IMG streaming contention** | Multiple custom models loading from `skr_vehicles.img` simultaneously | OP215's `CStreaming::RequestModel` is serial by design (GTA's streaming is single-threaded). No change needed; just monitor load times. |

### E.3 Low Risks

| # | Risk | Impact | Mitigation |
|---|------|--------|------------|
| 11 | **Gateway payload size** | `VehicleRegistry` with many models exceeds `kMaxGatewayPayload` (400 bytes) | Each entry is ~20 bytes. 400/20 = 20 models per message. If more: split into multiple registry messages or raise the limit. |
| 12 | **Backward compatibility** | Older SKR clients without VEHICLES support receive `VehicleAssign` | Gateway already checks `CapGateway`. VEHICLES messages are only sent to gateway-ready clients. Non-capable clients never see these messages. |

---

## F. Testing Strategy

### F.1 Unit tests (no GTA required)

1. **Protocol codec**: encode/decode all 5 VEHICLES ops, verify wire format byte-for-byte.
2. **Vehicle registry**: add/remove/lookup custom models, duplicate detection, capacity limits.
3. **Pool-slot mapping**: assign, revoke, re-assign, batch assign, verify no leaks.
4. **Edge cases**: revoke nonexistent, assign to full registry, malformed payloads, version mismatch.
5. **Regression**: all existing `SKRXTests` (158 checks), `SKRNativeApiTests` (70), `SKRClientPolicyTests` still pass.

### F.2 Integration tests (GTA + server required)

| Test | Procedure | Pass criteria |
|------|-----------|---------------|
| **F2.1: Registry delivery** | Connect enhanced client. Server sends `VehicleRegistry` after gateway ready. | Client logs `VehicleRegistry` received with correct model list. |
| **F2.2: Single vehicle assign** | Server creates proxy vehicle (560). Sends `VehicleAssign` mapping pool slot → 612. | Client swaps model. Visual: custom model appears, not Sultan. |
| **F2.3: Enter/drive custom vehicle** | Player enters the custom vehicle. Drive around. | `VehicleDriverSyncPacket` works normally. No desync. Other enhanced players see custom model. |
| **F2.4: Non-enhanced client** | Connect vanilla SA-MP client to same server. | Sees Sultan (proxy model). No errors, no crashes. Normal gameplay. |
| **F2.5: Vehicle respawn** | Destroy custom vehicle. Wait for server respawn. | Vehicle re-appears with custom model (client re-applies swap). |
| **F2.6: Revoke** | Server sends `VehicleRevoke`. | Client reverts to proxy model instantly. |
| **F2.7: Stream in/out** | Drive away from custom vehicle (beyond streaming distance), then return. | Vehicle correctly shows custom model on re-stream. |
| **F2.8: Batch assign** | Join area with 5+ custom vehicles. | All vehicles display correct custom models. No crashes, reasonable load time. |
| **F2.9: Disconnect/reconnect** | Disconnect and reconnect to server. | Fresh `VehicleRegistry` + `VehicleAssign` cycle. No stale state from previous session. |

### F.3 Regression tests

| Test | Pass criteria |
|------|---------------|
| Existing enhanced features (radar, HUD, phone, notifications) | No regression |
| OP215 local F9 spawn (if still enabled in dev builds) | Still works independently |
| Vanilla SA-MP server connection | No SKR-X activation, standard gameplay |
| CORE Ping/Pong | Still functional after VEHICLES namespace addition |

---

## G. Implementation Phases (Recommended Order)

| Phase | Scope | Dependencies | Est. files |
|-------|-------|-------------|------------|
| **G.1** | Protocol: define VEHICLES ops in `skrx_gateway.h` + `skrx_vehicle.h` | None | 2 files |
| **G.2** | Unit tests for VEHICLES codec | G.1 | 1-2 files |
| **G.3** | Server: vehicle registry + namespace handler | G.1 | 3-4 files |
| **G.4** | Client: refactor OP215 registration into reusable API | None (parallel with G.1–G.3) | 2 files |
| **G.5** | Client: VEHICLES namespace handler + model swap | G.1, G.4 | 3-4 files |
| **G.6** | Client: samp.dll R5 WorldVehicleAdd hook | G.5 | 1-2 files |
| **G.7** | Integration testing | G.3, G.5, G.6 | Config + test scripts |

**Estimated total:** 12–16 files modified or created.

---

## H. Key Decisions Required Before Implementation

1. **samp.dll hook strategy:** Option A (post-creation swap) vs. Option B (pre-creation hook). Recommendation: Option B. Needs sign-off because it adds a new samp.dll R5 offset dependency.

2. **Proxy model policy:** Hard-coded in server config (per-custom-model) vs. automatic by vehicle class. Recommendation: server config, with validation that proxy matches vehicle type and seat count.

3. **OP215 refactor scope:** Should OP215's local-only F9 spawn path remain for development/testing, or be fully replaced by the network path? Recommendation: keep both — F9 for local testing, VEHICLES namespace for multiplayer.

4. **Model asset distribution:** M2 assumes clients already have `skr_vehicles.img`. Future M3+ may use ASSETS namespace (ID 2) for server-to-client asset delivery. Not in scope for M2.

5. **Unreliable channel (step 11):** Deferred. Standard `VehicleDriverSyncPacket` already uses SA-MP's unreliable vehicle sync RPC. Custom vehicle position sync is handled by SA-MP's existing machinery via the proxy model. Step 11 unreliable is only needed if we add custom sync data beyond what SA-MP provides.

---

## I. Summary

**Feasibility: YES — proxy model approach is viable.**

The core insight is that SA-MP's vehicle sync is pool-slot-based, not model-based. All tick-level synchronization (position, velocity, rotation, health, driver, passengers) references pool slot IDs. The model ID is only used during initial vehicle construction on the client.

By creating stock proxy vehicles on the server and swapping the visual model on enhanced clients via the VEHICLES namespace, we get:

- **Full multiplayer sync** through SA-MP's existing vehicle pool
- **Graceful degradation** for non-enhanced clients (they see the proxy model)
- **No open.mp modifications** (server only creates valid 400–611 model IDs)
- **No samp.dll patches** (model swap happens in SKR's DLL via a hook)
- **Reuse of OP215's model registration** infrastructure (CVehicleModelInfo cloning, CStreamingInfo setup, IMG registration)
- **Protocol infrastructure already in place** (VEHICLES namespace reserved, gateway step 10 operational, CapGateway negotiated)

The main technical risk is the samp.dll R5 `WorldVehicleAdd` hook (new R5 offset dependency). This is mitigated by the existing SKR policy of pinning to R5 only and the availability of the post-creation swap as a fallback.
