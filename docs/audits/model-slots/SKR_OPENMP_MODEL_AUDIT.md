# SKR open.mp Server Model Audit — M0-S

**Date:** 2026-10-23 · **Mode:** READ-ONLY. The server was not restarted or interrupted. No config was
changed, no filterscript was loaded, and no RCON or in-game command was issued. No vehicle was spawned.
Nothing was committed. The audit read logs, configs, source, file hashes and a passive process snapshot.

Companion to `SKR_MODEL_SLOT_AUDIT.md` (client, M0). Files:
`skr_openmp_vehicle_usage.csv`, `skr_openmp_runtime_limits.json`, `SKR_MODEL_CLIENT_SERVER_MATRIX.md`.
Evidence tags are the same as in M0.

## Answers

| # | Question | Answer | Evidence |
|---|---|---|---|
| 1 | Standard vehicle models supported | **212** (400–611) | SOURCE_CONFIRMED `SDK/include/values.hpp:13` `MAX_VEHICLE_MODELS = 611 - 400 + 1`; `vehicles.hpp:469-471` |
| 2 | Vehicle instances | **2000** (`VEHICLE_POOL_SIZE`); streamed per player 700 | `values.hpp:8,55` |
| 3 | Distinct models in use now | **0 or 1** (only model 560 can exist). The exact value was **not observed** | INFERRED from source (§S3); a runtime query needs approval |
| 4 | Vehicles existing now | **0 to 1** (one `/gpscar` Sultan per connected player; 1 player connected). Not observed | INFERRED |
| 5 | Is 621 rejected by the current creation path? | **Yes, at two layers.** (a) The running gamemode `skr_runtime_test` has **no `/v` command at all**, so `/v 621` gives the "unknown command" reply. (b) The `/v` in `includes/skr/commands/dev.inc:45` (used only by `skr_dev`, which is not running) rejects `model > 611` before creation. (c) `CreateVehicle` → `IVehiclesComponent::create` → `isValidVehicleModel` returns `nullptr` → `INVALID_VEHICLE_ID` | SOURCE_CONFIRMED `Server/Components/Vehicles/vehicles_impl.hpp:387-391` |
| 6 | Custom skin registration | **Yes, server-side**: `AddCharModel(baseid, newid 20001–30000, dff, txd)` or `artconfig.txt`. It is delivered **only to 0.3.DL clients**, which SKR policy denies; everyone else gets the base skin | `Server/Components/CustomModels/models.cpp:600-660,666-690`; `values.hpp:63-64` |
| 7 | Custom object registration | **Yes, server-side**: `AddSimpleModel` / `AddSimpleModelTimed` (newid −30000…−1000). DL clients only | `models.cpp:532-536,609`; `values.hpp:65-66` |
| 8 | Extra custom vehicle registration | **No.** `ModelType` has no vehicle type; there is no vehicle registration native; `create()` hard-rejects non-400–611 models | `SDK/include/Server/Components/CustomModels/custommodels.hpp:7`; `vehicles_impl.hpp:389` |
| 9 | Server capabilities unavailable to stock R5 | All artwork (RPCs 179/181–185 and the DL `CustomSkin` field). R5 has these handlers but never registers them (M0 §12). Any vehicle model > 611. Objects 1000–1999 (R5 pool 1000; open.mp caps at `OBJECT_POOL_SIZE_037` only when 0.3.7 compat mode is enabled, `objects_impl.hpp:336`; the mode is not set in this config, so its effective value is UNKNOWN). Negative object IDs (R5 → 18631) | M0 §12; `values.hpp:10-11` |
| 10 | Can the SKR Gate map virtual 621 → a base transport model? | **Yes, technically.** open.mp exposes per-RPC **outbound** handlers (`getPerRPCOutEventDispatcher`, `SDK/include/network.hpp:148,152,301`) and entity extensions. The Gate can keep `virtualModel` per vehicle and emit an SKR side message (or rewrite RPC 164 per player) while the server entity keeps a valid base model (e.g. 405). **The Gate has no outbound hooks today**; it only filters inbound RPC 25/240/243 (`server/skr-gate-omp/skr_gate_component.cpp:99-104,300`). This is new code and needs approval | SOURCE_CONFIRMED |
| 11 | Reusable without engine hooks | Server: the vehicle pool, base-model creation, the Gate transport (RPC 240/241 envelope, API handshake), Pawn native registration, the omp-cef manifest/PAK/SHA-256/chunked download, `getBaseModel`-style mapping logic as a pattern. Client: skins and objects in **unused** engine IDs (M0 Option A), registered at IDE load | M0 §14 |
| 12 | Minimum engine + protocol changes for multiplayer support | **Protocol:** a Gate envelope step carrying `{vehicleId, virtualModel, engineSlot}`, sent on the same ordering channel as RPC 164 at every stream-in, plus Pawn natives to create a vehicle with a virtual model. **Client engine:** a hook on R5's vehicle-create path to apply `engineSlot`; client registration of the model (IDE/IMG) before level load. **Additional (213th) vehicle:** also relocate GTA's vehicle model-info store (M0 Option C). **Replacing an existing slot for SKR clients:** no store relocation | M0 §8, §15 |

## S1 — Running server identity

| Item | Value | Tag |
|---|---|---|
| Process | PID 13108 `server-runtime-branded-test\skr-server.exe`, no arguments, working dir `server-runtime-branded-test`, started 2026-10-23 22:42:36 | RUNTIME_OBSERVED (Win32_Process) |
| Banner | `SKR Runtime Server 1.0.23 (based on open.mp 1.6.7.0) from commit 3d20f3dc0e26917264cdc8585c65913e080a143c` | RUNTIME_OBSERVED log.txt |
| Exe | 3,915,776 bytes, SHA-256 `5390964269dd2767669f16e9ccaa5cd87501890823f2837b286d45133600290c`, PE32 i386 console | BINARY_CONFIRMED |
| Source match | **Identical** to `server-build/open.mp/build2/Output/RelWithDebInfo/Server/skr-server.exe`. HEAD 3d20f3d has 5 locally modified files (LegacyNetwork, Source CMake/core/server) with **no model, vehicle, skin or artwork changes** | BINARY_CONFIRMED + SOURCE_CONFIRMED |
| Stable runtime | `server-runtime/skr-server.exe` has the same SHA-256. It differs in components (no CEF side script) and runs no filterscripts | BINARY_CONFIRMED |
| OS | Windows NT 10.0.26200, AMD64 host, 32-bit server | RUNTIME_OBSERVED |
| Components | 24 loaded, including **CustomModels 1.6.7.0** (UID 15e3cb1e7c77ffff), SKR Gate 0.1.0.0 (UID 00000000534b5247), Cef 1.3.0.0, Vehicles, Objects, Actors | RUNTIME_OBSERVED |
| Plugins | crashdetect 4.22-1, mapandreas, sscanf 2.8.2, streamer 2.9.4 (pawnraknet present, not loaded) | RUNTIME_OBSERVED |
| Scripts | main `skr_runtime_test` (amx 2026-10-23 20:16:58); side `filterscripts/skre_test` | config.json + log |
| Client policy | 0.3.7 allowed, SKR Enhanced allowed, open.mp allowed, **0.3.DL denied**; mode hybrid | log.txt |
| Artwork / resources | `artwork` key absent from config.json and server.cfg → source defaults (enable=true, models_path=`models`). `models/` exists and is empty (no `artconfig.txt`). No `[artwork:info]` lines. No HTTP listener (TCP listen list empty). CEF PAKs apps/skr-auth/skr-hud found. The Gate "artwork rules" line is the **server logo/banner** (Task #18), not model artwork | RUNTIME_OBSERVED + SOURCE_CONFIRMED `models.cpp:298-304,428-470` |

Secrets: config.json contains `password` and `rcon` keys. They were not read into this report.

## S2 — Limits from the matching source

Kept separate as requested:

| Category | Value | Source |
|---|---|---|
| Max standard vehicle model **types** | 212 (400–611) | `values.hpp:13`, `vehicles.hpp:469` |
| Max vehicle **instances** | 2000 | `values.hpp:8` |
| Max object instances | 2000 (open.mp) / 1000 for 0.3.7 clients | `values.hpp:10-11` |
| Max actor instances | 1000 | `values.hpp:41` |
| Vehicle model types **in use** | `preloadModels[212]` (`uint8_t` per model; **wraps above 255** instances of one model) | `vehicles_impl.hpp:25,459,484,529,559` |
| Custom registrations | skins 20001–30000, objects −30000…−1000; `FlatHashMap` baseModels | `values.hpp:63-66`, `models.cpp:600-656` |
| Client GTA engine slots | see M0 (vehicle 212/212, ped 319, atomic 20000) | M0 §6 |

Validation paths:
- **Vehicle create:** `isValidVehicleModel` (400..611) → `nullptr`. Models 449, 537 and 538 are static-only, and trains reserve consecutive IDs (`vehicles_impl.hpp:387-420`).
- **RPC 164 `StreamInVehicle`:** writes `ModelID` as a uint32 from `spawnData.modelID`, which is always 400–611 because of create (`Shared/NetCode/vehicle.hpp:105-140`; `vehicle.cpp` streamIn line 38).
- **Objects:** `IObjectsComponent::create` does **no** model validation (`objects_impl.hpp:321`). For DL clients, artwork is sent before objects (`objects_main.cpp:39`).
- **Skins:** `Player::setSkin` does no range check; it maps custom→base (`Server/Source/player.cpp:287-309`). Actors (`actor.hpp:73`) and classes (`classes_main.cpp:70`) also call `getBaseModel`.
- **Negative and extended IDs:** handled only by CustomModels, and only for skins and objects.

## S3 — Runtime data

Installed native signatures (`deps/pawn-stdlib` @ 17c1b01; implementations in `Server/Components/Pawn/Scripting`):

| Native | Header | Implementation note |
|---|---|---|
| `GetVehicleModelsUsed()` | `omp_vehicle.inc:1646` | counts `preloadModels[i] > 0` (`Vehicle/Natives.cpp:432`) |
| `GetVehicleModelCount(modelid)` | `omp_vehicle.inc:1641` | returns 0 outside 400–611 (`Natives.cpp:423`) |
| `GetVehicleModel(vehicleid)` | `omp_vehicle.inc:1116` | `Natives.cpp:270` |
| `bool:IsValidVehicle(vehicleid)` | `omp_vehicle.inc:1469` | `Natives.cpp:358` |
| `GetVehiclePoolSize()` | `omp_core.inc:1283` | **highest occupied ID**, −1 if none, not a count (`Core/Natives.cpp:81`) |
| `bool:IsValidCustomModel(modelid)` | `omp_object.inc:1408` | `CustomModels/Natives.cpp:85` |
| `bool:GetCustomModelPath(modelid, dffPath[], dffSize, txdPath[], txdSize)` | `omp_object.inc:1403` | `CustomModels/Natives.cpp:97` |

Snapshot, using the passive tiers only (logs → existing commands → component APIs). No tier yields live entity counts:
- The logs contain no vehicle create/destroy telemetry. `/gpscar` only sends client messages.
- No existing admin/diagnostic command reports vehicles. `/skrdiag` reports runtime identity; running it would mean acting in-game, so it was not used.
- Component APIs are reachable only from code inside the process.
- **Static analysis:** the running scripts create no vehicles at init; `/gpscar` (`skr_runtime_test.pwn:223-262`) creates one model 560 per player and destroys it on reuse or disconnect. So vehicles ∈ {0, 1}, distinct models ∈ {0, 1}, highest vehicle ID ∈ {none, 1}. INFERRED.
- Custom skin/object definitions: **0** (empty `models/`, no Add*Model calls in the running scripts, no artwork HTTP listener). Registered SKR custom IDs: **0** (no SKR model registry exists). Conflicts: none.
- **Proposed probe (needs approval, not loaded):** an isolated read-only filterscript on a dev server (not this test runtime) that prints `GetVehiclePoolSize`, `GetVehicleModelsUsed`, `GetVehicleModelCount(400..611)` and `IsValidCustomModel` for the candidate IDs once on `/rcon`, with no per-tick work.

## S4 — Artwork

- Config: `artwork.enable` defaults to **true**, `artwork.models_path` = `models`, `artwork.port` = 7777 TCP, `artwork.cdn` empty (`models.cpp:298-304,412-440`). Legacy `useartwork`/`artpath` map onto the same keys (`LegacyConfig/config_main.cpp:79-80,126-127`).
- Load: `onLoad` creates `models/` and parses `artconfig.txt` lines matching AddCharModel/AddSimpleModel/AddSimpleModelTimed. The web server starts only when a model is added. There is no manifest beyond `artconfig.txt` plus CRC32 checksums (`crc32.hpp`).
- Lookup APIs: `IsValidCustomModel`, `GetCustomModelPath`, `getBaseModel`, `getModelNameFromChecksum`.
- Asset inventory: **empty**.
- Server-side registration of skins/objects: **CONFIRMED (source)**. Loading by the stock R5 client: **UNSUPPORTED**. CustomModels sends only to `ClientVersion_SAMP_03DL` (`models.cpp:650,700,728,738`); R5 never registers the receiving RPCs (M0 §12); and SKR denies DL clients. The SKR Enhanced client is classified as 0.3.7, so it would also receive only base models.
- **Vehicles: no mechanism.** Do not infer vehicle support from skin/object support.

## S5 — Gamemode `/v`

| Item | Finding |
|---|---|
| Running gamemode | `skr_runtime_test`: **no `/v`**. Vehicle spawning is only `/gpscar` (fixed model 560) |
| `/v` implementation | `includes/skr/commands/dev.inc:39-81` (identical SHA-256 in `server-runtime/` and the branded test), included only by `gamemodes/skr_dev.pwn:42` (not running) |
| ID validation | `sscanf(params,"iI(-1)I(-1)")` then a hard-coded `model < 400 \|\| model > 611` → usage message (`dev.inc:45-48`) |
| Name resolver | none (numeric only) |
| Spawn natives | `CreateVehicle(model, x, y, z+0.5, a, c1, c2, -1)` → `SetVehicleParamsEx` engine on → `LinkVehicleToInterior`, `SetVehicleVirtualWorld`, `PutPlayerInVehicle` |
| Destroy/respawn | one vehicle per player in `skr_dev_vehicle[]`; destroyed on re-use and on `OnPlayerDisconnect` (y_hooks); respawn delay −1 |
| Pool/cache integration, model registration, Gate integration, resource metadata | **none** |
| Access | `SKR_DEV_RCON_ONLY (0)`: any player |

What would need to change for `/v 621` = Dacia Logan while `/v 411` stays the Infernus (description only, no change made):
1. **Resolver:** replace the 400–611 check with a lookup in an SKR vehicle registry: `411 → {standard, engine 411}`, `621 → {virtual, baseModel B, engineSlot E, resource "dacia_logan"}`. Unknown IDs are rejected.
2. **Creation:** standard IDs keep `CreateVehicle(411, …)`. Virtual IDs call a new Gate native, e.g. `SKR_CreateVehicleEx(621, …)`. Internally it calls `CreateVehicle(B, …)` (B in 400–611, so server checks, sync, seats and components stay valid) and stores `virtualModel = 621` on the vehicle (a Gate entity extension).
3. **Transport:** for SKR clients, the Gate delivers `{vehicleId → engineSlot E}` with every RPC 164 (outbound handler or an envelope message on the same ordering channel). Non-SKR clients see B.
4. **Client:** the SKR client applies E when R5 creates the vehicle. E must hold the Logan model: a replacement slot now, or a new slot after a vehicle-store extension (M0 Options B1/B2/C).
5. **Lifecycle:** clear the extension on destroy/respawn; re-send on every stream-in; preserve it across `SetVehicleToRespawn`.
6. **Natives for scripts:** `GetVehicleModel` returns B, so add `SKR_GetVehicleVirtualModel`. An open decision: should `GetVehicleModelCount(B)` include virtual vehicles? (It will, because `preloadModels` is keyed by base.)

## S7 — Performance baseline (observation only)

| Metric | Value | Tag |
|---|---|---|
| Players | 1 (log) | INFERRED |
| Vehicles / objects / actors | 0–1 / ~0 / 0 | INFERRED (§S3) |
| Process | working set 203.5 MB, private 243.5 MB, 32.9 CPU-s over ~73 min, 0.78 % of one core (2-s sample), 194 handles, UDP 7777 and 7779 | RUNTIME_OBSERVED (Get-Process / perf counter) |
| Gamemode telemetry | none for entities; SKR API events are logged (`[SKR API] …`) | RUNTIME_OBSERVED |
| Create/destroy logs | none | RUNTIME_OBSERVED |
| Resource overhead | CEF PAK scan at boot only; CEF network server on 7779 | RUNTIME_OBSERVED |

Expected overhead of a future model registry:
- Lookups would be O(1) (a ≤ 64K-entry flat array or hash keyed by virtual ID), plus one extension per vehicle (a few bytes).
- The main cost would be one extra Gate message per stream-in per SKR player; stream-ins are bounded by `MAX_STREAMED_VEHICLES` 700 per player.
- No per-tick scans should be needed: hook on create, stream-in and destroy only.
- Asset downloads must not touch the game tick. Reuse the CEF chunked transfer before joining.

## Limits of this audit
- Live entity counts were not queried: no RCON, no in-game commands, no filterscript.
- The effective `artwork.enable` comes from the source default, not a log line.
- Bounds on the `/gpscar` vehicle are inferred from source.
