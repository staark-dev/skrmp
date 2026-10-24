# SKR Model Slot & Engine Limit Audit — M0

**Date:** 2026-10-23 · **Scope:** GTA SA 1.0 US x86 + SA-MP 0.3.7-R5 + SKR Enhanced client/server
**Mode:** READ-ONLY. Nothing was hooked, patched, registered, built, merged, committed or deployed.
No ID is reserved. The Dacia Logan proof of concept was not started.

Companion files in this folder:

| File | Contents |
|---|---|
| `skr_model_slots.csv` | one row per model ID 0–19999 (type, name, source line, registration, candidate status) |
| `skr_model_store_capacity.json` | 19 stores/limits with declared vs. verified capacity, usage, and constraints |
| `SKR_MODEL_EXTENSION_OPTIONS.md` | Options A, B and C compared |
| `SKR_OPENMP_MODEL_AUDIT.md`, `skr_openmp_*.{csv,json}` | M0-S server-side audit (open.mp limits, artwork, `/v` path, runtime state) |
| `SKR_MODEL_CLIENT_SERVER_MATRIX.md` | Merged client/server capability matrix |

Evidence tags: **SOURCE_CONFIRMED** (data or source file, with a line), **BINARY_CONFIRMED**
(disassembly of the exact binary, with an address), **RUNTIME_OBSERVED** (an existing log from a real run),
**EXTERNAL_REFERENCE** (a third-party project; read as evidence only, no code copied), **INFERRED**,
**UNKNOWN**.

Disassembly method: Python + capstone 5.0.7, a linear sweep of `.text` (samp.dll: 339,941
instructions; gta_sa.exe: 3,587,923 instructions), plus raw-byte searches for absolute
references. Function names come from `D:\SA-MP Reverse\RE\client\data\idb_sampR5_functions.tsv`
(they are EXTERNAL_REFERENCE names). Every address cited here was checked by disassembly.
Scripts are in the session scratchpad and are not part of the repo.

---

## 1. Decision table

| Question | Answer | Tag / evidence |
|---|---|---|
| Global model ID limit | **20000** (IDs 0–19999). Hard limit: `CStreaming::ms_aInfoForModel[20000]` is TXD slot 0. | BINARY_CONFIRMED 0x4C6810 (`mov ecx,0x4E20; mov edi,0xA9B0C8; rep stosd`); 0x408A28 range constants |
| R5 vehicle model range | **400–611** only. `ScrWorldVehicleAdd` (RPC 164) silently drops anything else. open.mp checks the same range on the server. | BINARY_CONFIRMED samp.dll 0x1000E741–0x1000E751; SOURCE_CONFIRMED `SDK/include/Server/Components/Vehicles/vehicles.hpp:469-471` |
| Vehicle store capacity | **212 / 212 used, 0 free.** R5 does **not** relocate it. | BINARY_CONFIRMED layout 0xB1F654..0xB478F8 / 0x308; no samp.dll reference |
| Ped store capacity | GTA 278, **relocated by R5 to 319**; 288 used, **~31 free** | BINARY_CONFIRMED samp.dll 0x100AA9C0 (patches 0x4C67AD); used count SOURCE_CONFIRMED, runtime UNKNOWN |
| Object (atomic) store capacity | GTA 14000, **relocated by R5 to 20000**; ≤15486 used, **≳4500 free** | BINARY_CONFIRMED samp.dll 0x100AAA10 + 14 patched sites |
| Candidate unused IDs | **3725 IDs** have no IDE line and no code registration. None is declared free: each still needs runtime probe P1. Largest runs: 11754–12799 (1046), 15065–15999 (935), 13891–14382 (492), 6526–6862 (337). Near the vehicle range: 612–614. | SOURCE_CONFIRMED (59 IDE files); runtime UNVERIFIED |
| ID 621 | **Not free, not a vehicle.** It is `veg_palm02` (objs, TXD `gta_tree_palm`), declared at `data/maps/generic/vegepart.ide:8`, `veg_palm02.dff` in gta3.img, placed **231 times** in the streamed binary IPLs. `/v 621 → Dacia Logan` needs a **vehicle-only virtual ID** mapped to an engine vehicle slot. | SOURCE_CONFIRMED |
| One extra vehicle without replacing one | **NO on stock GTA + R5** (needs an engine extension). Needs all of: vehicle store relocation (GTA), a bypass or mapping around R5's 400–611 check, and server-side model data. | BINARY_CONFIRMED |
| One extra skin | **YES in principle, client-side registration only.** There are 31 free ped-store entries. The R5 skin check is "ID ≤ 30000 and it is a `CPedModelInfo`", so any registered ped ID ≤ 19999 passes. open.mp does not range-check skins. Registration must happen during IDE load (before the level loads). | BINARY_CONFIRMED 0x100B3DD0; SOURCE_CONFIRMED `Server/Source/player.cpp:287` |
| One extra object | **YES in principle**: free atomic entries, free IDs and free streaming entries exist. **Bottlenecks:** the COL store (~2 of 255 free, INFERRED) and IMG archives (~2 of 8 free, INFERRED). | BINARY_CONFIRMED capacities; usage INFERRED |
| Dormant R5 code | **YES, substantial.** R5 contains a complete 0.3.DL custom-model client: 4 RPC handlers, a model manager, a DFF/TXD cache, a 65535-entry model-pointer table, a 707-site `ms_modelInfoPtrs` relocator, streaming/TXD redirections and a COL pool resizer. Nothing references the entry points, so none of it runs. | BINARY_CONFIRMED (§12); independently corroborated by EXTERNAL_REFERENCE customskin-0.3.7-R5 |
| Multiplayer protocol work | **Required for a virtual vehicle ID.** RPC 164 carries a raw `int32 ModelID` that R5 range-checks. A custom vehicle needs per-entity identity outside that field (Gate side channel, ordered with 164) or a patch to the client check. Late joiners and re-streams receive 164 again, so the identity must be re-sent each time. | BINARY_CONFIRMED + SOURCE_CONFIRMED (§15) |

**Not done:** runtime usage counts for the damage-atomic, COL and TXD pools, and the full
runtime null-state of `ms_modelInfoPtrs`, were **not** observed. They need probe P1 (§17), and
the user must approve it. Capacities are verified; some "used" counts are INFERRED.

---

## 2. Binary fingerprints (Phase A)

| Module | Path in use | Size | SHA-256 | PE timestamp / image size | Tag |
|---|---|---|---|---|---|
| GTA SA 1.0 US (HOODLUM) | `C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto San Andreas\gta_sa.exe` | 14,383,616 | `a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26` | 0x427101CA (2005-04-28) / 0x1177000 | BINARY_CONFIRMED. **Byte-identical** to `D:\SA-MP Reverse\gta_sa.bak.exe`. Sections include `.HOODLUM`; some functions are SecuROM jmp stubs (e.g. 0x407610 → 0x1567B90). |
| (other exe present) | same folder `gta-sa.exe` | 5,971,456 | not hashed | — | Not the launch target (INFERRED from launcher path logic); not analysed. |
| SA-MP 0.3.7-R5 | `build-test-certification-win32\Release\samp\samp.dll` (the launcher's bundled copy; **no samp.dll in the GTA folder**) | 1,204,224 | `b72b5dbe725f81864ca3f78bc7063bda56cc05fc7188af822fa7a754432553a2` | 0x6372C39E / 0x27E000; PDB `c:\sampcvs037R4\sampcvs\main\saco\Release\samp.pdb` | BINARY_CONFIRMED. Identical to `D:\SA-MP Reverse\samp.dll`; matches the pin in `apps/launcher/preflight/samp_inspector.cpp:170` and `client/game-state/game_state.cpp:31`. |
| SKRLauncher.exe | `build-test-certification-win32\Release\` | 507,904 | `0aa8fe5c39a33f28766ba526a39ade26739d9844828bbb832dc9c68248f57438` | 0x6ADBBC7A (2026-10-23 19:58 UTC) | BINARY_CONFIRMED |
| SKR.Client.dll | same | 764,416 | `9967964e77110d422d5981dcdad205218db93d70589a8f511f726b0bc226bbf6` | 0x6ADBD376 (2026-10-23 21:36 UTC) | BINARY_CONFIRMED (a test-certification build; contains the uncommitted working tree) |
| SA-MP data | `Release\samp\SAMP\` | SAMP.ide 89,347; SAMP.img 47,261,696; SAMPCOL.img 3,305,472; custom.img 8,192; **CUSTOM.ide 0 bytes** | see §4 | — | SOURCE_CONFIRMED; identical to `D:\SA-MP Reverse\SAMP\` |
| plugin-sdk | `D:\GCMP\deps\plugin-sdk-sa-llm` | — | git `7f36b083ff9d57e8d86c0b3440b1228a69ba1a93` (2026-10-15, "Fix AddTrace hang…") | — | SOURCE_CONFIRMED. `CModelInfo.cpp:9-10` reads `ms_modelInfoPtrs`/count through 0x40CD67/0x40CD58 ("limit adjusters support"). |
| open.mp server source | `server-build/open.mp` | — | git `3d20f3dc0e26917264cdc8585c65913e080a143c` | — | SOURCE_CONFIRMED |
| omp-cef | `deps/omp-cef` | — | git `79fb79e` | — | SOURCE_CONFIRMED |

No limit adjuster is installed: there is no `.asi`, no ASI loader, no modloader and no fastman92
or open-limit-adjuster in the GTA folder (SOURCE_CONFIRMED by listing the folder). The only active
"limit adjuster" is **samp.dll itself** (§3).

## 3. Existing hooks, patches and init order (Phase A)

### 3.1 SA-MP R5 live engine patches relevant to models (BINARY_CONFIRMED)

These were cross-checked against `RE/client/data/patches.csv` and disassembled in both binaries.

| GTA site | Original | R5 value | Meaning | Installed by |
|---|---|---|---|---|
| 0x731F60 | `push 0x1388` (5000) | 0x4E20 (20000) | CTxdStore pool size | `CGame::InitGame` 0x100A0890 → 0x100AA710 (0x100AA99D) |
| 0x5B8E6A | 0x3200000 | value of [0x1026B3DC] | CStreaming memory budget (`0x8A5A80`) | 0x100AA710 (0x100AA93F) |
| 0x4083C0 | function | `ret` | disables a CStreaming routine that clears `ms_files` names (INFERRED role) | 0x100AA710 |
| 0x4C67AD | `add esi,0xB478FC` | 0x101C9718 | **ped store → 319 entries in samp.dll** | `CGame::StartGame` 0x100A08E0 → 0x100AAEB0 → 0x100AA9C0 |
| 14 sites 0x4C63F2…0x4C6A2E | 0xAAE954 | 0x101CEBD8 | **atomic store → 20000 entries in samp.dll** | 0x100AAEB0 → 0x100AAA10 (site table 0x10116720) |
| 0x5B8FE4 | `push 0x32` | 0x7F | VehicleStruct pool 50 → 127 | 0x100AAF43 |
| CPools::Initialise 0x550F10.. | see JSON | see JSON | entity pools (Buildings 20000, Objects 3000, Peds 240, …) | 0x100AAEB0 → 0x100AA590 |
| 0x40936A | 0xB7D0BC (array disp) | EBX (samp.dll table) | an additional relocated CStreaming-side table (INFERRED role) | 0x100AAA80 |

**Not patched by R5:** the vehicle store, the clump, time, weapon and damage-atomic stores,
`ms_modelInfoPtrs` itself, `ms_aInfoForModel`, the COL pool and the IMG table (BINARY_CONFIRMED:
no samp.dll reference).

### 3.2 R5 dormant (unreferenced) patchers — BINARY_CONFIRMED

| Function | What it would do | Why it is dead |
|---|---|---|
| 0x100A7A00 | zeroes 65535 dwords at 0x101625B0, copies 20000 pointers from 0xA9B0C8, calls 0x100A7970 and 0x100A6FF0 | only caller is 0x1000BD60 (custom-model manager init) |
| 0x100A7970 | rewrites the imm32 at **707 GTA sites** (table 0x10114B10 for 1.0 US, 0x101158E0 for another build) to the new table base 0x101825AC, then sets flag [0x101A25AC]=1 | same chain |
| 0x100A6FF0 | redirects ≥10 GTA call sites (0x4087D7/0x4087EA, 0x742495, 0x7424EC, 0x742548, 0x740721, 0x740B69, 0x736247, 0x7424CB, 0x73AC4B, 0x6D7C90) into samp.dll: streaming and RW TXD paths (INFERRED role) | same chain |
| 0x100AA4E0 | resizes the COL pool (0x411458) and 7 related sites (table 0x101166F8) | no code or data reference at all |

The 707-entry US table was validated against `gta_sa.exe`: **707/707** sites carry the expected
opcode byte and imm32 = 0xA9B0C8. The second table matches 677/707, so it targets a different exe
build. Independently, linear disassembly of `gta_sa.exe` finds 679 instruction-level
references to the exact base, plus 59 constant-index references (`ms_modelInfoPtrs[k]`,
k ∈ {1, 7, 370–397, 425, 460, 530, 1001, 5070, 7375, 7387, 13203}) that **no** table covers.

### 3.3 SKR client hooks — none in model systems (SOURCE_CONFIRMED)

`grep` of absolute GTA addresses in `client/**` (excluding gps): pause/settings
(0x49EA40, 0x506DE0, 0x573260, 0x57C660, 0x745C70, 0x7F2CC0…), menu (0x57B210/0x57B440), HUD/radar
(0x5890A0…0x58FAE0, 0x719380…, 0x727B60), zones/text (0x572270, 0x6A0050), the CGame::Process
call sites (0x53BEE0…0x53E981), the radio fix (0x4F145C, `client/gta-fixes/gta_fixes.cpp`), and the SKR
signature at 0xB7CE50 (`client/samp-hooks/samp_hooks.cpp:111`). **No SKR code touches CModelInfo,
CStreaming, CTxdStore, CColStore, pools, IDE/IMG loading or vehicle creation.** The TEST_ONLY
VFS (`client/vfs/samp_vfs.cpp`) hooks kernel32 CreateFileA/W, GetFileAttributesA/W and
FindFirstFileExW, and redirects `SAMP\*` resource paths to `Release\samp\`.

### 3.4 Init order (provenance kept separate)

1. **SKR launcher** starts GTA; SKR.Client.dll is injected (DLL, not ASI).
2. **SKR** `PreloadSamp` installs the VFS hooks, then `LoadLibraryExW(samp.dll)` (`client/vfs/samp_vfs.h`).
3. **SA-MP** DllMain → thread 0x100C3A80 → `CGame::InitGame` 0x100A0890 (TXD pool, streaming
   memory) → waits until `gGameState` [0xC8D4C0] == 7 → `CGame::StartGame` 0x100A08E0 → 0x100AAEB0
   (pools, ped/atomic store relocation) (BINARY_CONFIRMED; RE doc `03-lant-initializare.md`).
4. **GTA** then runs level load: `CModelInfo::Initialise` 0x4C6810 → IDE parsing (default.dat, gta.dat)
   → SA-MP adds `SAMP\samp.IDE` and `SAMP\custom.IDE` → IMG registration and CD directories.
   RUNTIME_OBSERVED order in `Release/SKR/samp_vfs.log`: `SAMP\CUSTOM.IMG, SAMP\SAMP.IMG, MODELS\GTA3.IMG,
   MODELS\GTA_INT.IMG, SAMP\SAMPCOL.IMG, MODELS\CUTSCENE.IMG`, then `SAMP\samp.IDE`, `SAMP\custom.IDE`,
   `SAMP\samp.ipl`.
5. **Third-party:** omp-cef (`cef/`) loads its PAK resources after connecting; it does not touch models.

Any SKR model registration must run **between steps 3 and 4's IDE pass, or inside it**. Registering
after the level load means adding model infos and streaming entries by hand (§11).

## 4. Model ID namespace 0–19999 (Phase B)

Sources parsed: every `IDE` line in `data/default.dat` and `data/gta.dat` (57 files) plus `SAMP.ide` and
`CUSTOM.ide` (59 total). In GTA, a later declaration overwrites `ms_modelInfoPtrs[id]` and still consumes
a store slot (AddXModel has no id or capacity check, BINARY_CONFIRMED 0x4C6620..0x4C67CC).

| Range | Registered | Notes |
|---|---|---|
| 0–399 | 362 IDE + 8 code (374–381) | peds 0–299 (peds.ide, 276 lines); cutscene `hier` 1–… and 300–311; **300–311 are overridden by SAMP.ide peds** (lapdna…dsherna); 374–381 are registered **by code** (MI_CAR_DOOR…MI_BODYPARTB, CModelInfo::Initialise 0x4C687B..0x4C6A47). Unused: 2–6, 8, 42, 65, 74, 86, 119, 149, 208, 265–273, 289, 329, 332, 340, 382–383, 398–399 |
| 400–611 | 212/212 | vehicles.ide (cars) |
| 612–999 | 366 | 612–614 unused; 615+ vegetation (vegepart.ide) |
| 1000–19999 | 15327 | 1435 SA-MP objects (SAMP.ide, 11682–19999) |
| **unused overall** | **3725** | 100 gaps; see CSV |

IMG content (SOURCE_CONFIRMED, VER2 directories): gta3.img 16297 entries (12955 dff, 2750 txd,
216 col, 164 ipl, 64 dat, 148 ifp); gta_int.img 2484; SAMP.img 1673 (1462 dff, 208 txd, 3 ifp);
SAMPCOL.img 1 col; custom.img 1 txd; cutscene.img 634. **CUSTOM.ide is empty (0 bytes)** and custom.img
holds one TXD: SA-MP's "custom" hook exists, but nothing uses it in this install.

SKR manifests: **none** declare models. SKR has no model manifest; the only manifests are the
updater manifest (`shared/updater/manifest.h`) and the CEF PAK manifest (§14). SOURCE_CONFIRMED.

An "unused" ID only means it has no IDE line. It is not free until probe P1 confirms
`ms_modelInfoPtrs[id]==NULL` after level load and that no IMG entry, script or hard-coded MI_* constant
uses it. The 59 constant-index references in §3.2 include 370–397 and 1001, all registered IDs.

## 5. ID 621 (Phase B)

- `data/maps/generic/vegepart.ide:8`: `621, veg_palm02, gta_tree_palm, …` → **CAtomicModelInfo (objs)**. SOURCE_CONFIRMED.
- gta3.img contains `veg_palm02.dff`. The streamed binary IPLs inside gta3.img hold **231 instances** of id 621. SOURCE_CONFIRMED (bnry IPL parse; the record layout follows the public format, INFERRED).
- It is not in the vehicle range, the vehicle store is full, and R5 drops RPC 164 for 621 (0x1000E74C `cmp eax,0x263; jg drop`).

**Conclusion:** `/v 621` can only work as an **SKR virtual vehicle ID**, a name in a vehicle-only namespace
that the server and client resolve to an **engine vehicle slot**: either an existing 400–611 slot
(a replacement) or a new slot after Option C. The engine ID 621 stays a palm tree. See the Options document.

## 6. Store capacities (Phase C)

Full data with every field is in `skr_model_store_capacity.json`. Summary:

| Store | Declared (GTA) | Verified effective (GTA+R5) | Used | Free | R5 changes it? |
|---|---|---|---|---|---|
| ms_modelInfoPtrs | 20000 | 20000 | 16267 IDs (16279 IDE lines) + 8 code | 3725 IDs (unverified) | no (dormant shadow) |
| atomic | 14000 | **20000** | ≤15486 | ≳4514 (INFERRED) | **yes** |
| damage atomic | 70 | 70 | UNKNOWN | UNKNOWN | no |
| lod atomic / lod time | 1 / 1 | 1 / 1 | UNKNOWN | — | no |
| time | 169 | 169 | 160 | 9 | no |
| weapon | 51 | 51 | 50 | 1 | no |
| clump | 92 | 92 | 91 | 1 | no |
| **vehicle** | 212 | **212** | 212 | **0** | **no** |
| ped | 278 | **319** | 288 | ~31 | **yes** |
| CStreaming info | 26316 | 26316 | — | — | no |
| TXD pool | 5000 | **20000 pool / 5000 streamable** | ~3800–3900 | ~1100 streamable (INFERRED) | **yes (pool only)** |
| COL pool | 255 | 255 | ~253 (INFERRED) | ~2 | no (resizer dormant) |
| IMG archives | 8 | 8 | 6 (RUNTIME_OBSERVED) | 2 (INFERRED) | no |
| VehicleStruct | 50 | 127 | — | — | yes |

All "binary-enforced" capacities have **no runtime guard**: AddXModel, AddImageToList (returns 0 =
aliases the first archive) and CPool::New (returns null) fail silently or corrupt memory. A capacity is
a hard ceiling with no error path.

## 7. Streaming, TXD and COL dependencies (Phase G)

- Every model ID n needs `ms_aInfoForModel[n]` (0x8E4CC0 + 20n). CStreaming::LoadCdDirectory fills the CD
  position and size by **name match** between IMG `.dff` entries and model infos (EXTERNAL_REFERENCE gta-reversed;
  consistent with the binary). A model info whose name matches no IMG entry gets size 0 and never loads.
- A model's TXD is a CTxdStore slot named in the IDE. A streamed TXD must be a slot < 5000, because its
  streaming index is 20000+slot (BINARY_CONFIRMED range constants). R5's 20000-slot pool only helps TXDs
  that are created directly (e.g. by SA-MP), not TXDs streamed from an IMG.
- COL: objects take collision from a COL archive (one COL slot per `.col`, max 255) or from DFF-embedded
  collision. Vehicles and peds carry collision in the DFF, so **a custom vehicle or skin needs no COL slot;
  a custom object usually does**.
- IMG: a new custom IMG uses one of the ~2 remaining archive slots. Appending entries to an existing SKR-owned
  IMG costs no slot.

## 8. Vehicle limits (Phase D)

| Layer | Fixed limit / check | Evidence |
|---|---|---|
| GTA model info | vehicle store 212, full; AddVehicleModel 0x4C6770 has no capacity check | BINARY_CONFIRMED |
| GTA streaming | none specific (model < 20000) | BINARY_CONFIRMED |
| GTA VehicleStruct pool | 127 per loaded vehicle model (R5) | BINARY_CONFIRMED |
| GTA model-specific code | a jump table for 400–499 at 0x47D230 (`lea ecx,[eax-0x190]; cmp ecx,0x63`); model 611 special case at 0x6AF292; most vehicle logic is keyed on CVehicleModelInfo type/class/handling, **not** on ID | BINARY_CONFIRMED (sites); a full list of (id−400)-indexed arrays is UNKNOWN (needs a Ghidra pass) |
| Handling | handling.cfg entries referenced by name from the IDE (`cars` line); a new vehicle can reuse an existing handling line | EXTERNAL_REFERENCE (gta-reversed); INFERRED |
| Upgrades / carmods | per model-info lists (carmods.dat); open.mp `componentValidVehicleModel[MAX_VEHICLE_MODELS*6]` indexed by model−400 | SOURCE_CONFIRMED `SDK/include/Server/Components/Vehicles/vehicle_components.hpp:23,253` |
| R5 network create | RPC 164 handler 0x1000E670: model must be 400..611, vehicle id < 2000, else dropped | BINARY_CONFIRMED |
| R5 other | `SetRequiredVehicleModels` 0x100A1610 (212-byte array at +0x6E, loop 400..611); 0x100A1790 (400 ≤ m < 612); 0x100A5B10/5BB0/5C50/5CF0 (entity model 400..611); `CEntity` helpers 0x1009FA60/0x1009FB20; textdraw preview 0x100B34A0; `IsTrailer` 0x100B7DBE | BINARY_CONFIRMED |
| R5 sync | vehicle sync packets carry no model ID (the model is only in RPC 164) | INFERRED from RE wire table `client_rpc_table.csv`; not re-verified packet by packet |
| open.mp server | `isValidVehicleModel` 400..611; `MAX_VEHICLE_MODELS = 212`; static per-model tables (sizes, seats, components); `preloadModels[212]` | SOURCE_CONFIRMED `SDK/include/values.hpp:13`, `vehicles.hpp:469`, `vehicle_models.hpp`, `vehicle_seats.hpp`, `Server/Components/Vehicles/vehicles_impl.hpp:25,389` |

**"Can one extra vehicle be added without replacing one?"** → **NO on the current stack. Class:
ENGINE_EXTENSION_REQUIRED.** Minimum work: relocate the vehicle model-info store (GTA patch, Option C); keep the
wire model inside 400..611 (a base/fallback model) and carry the custom identity separately (Option B), or patch
R5's 164 check; register IDE, IMG and handling at load time; and extend the server-side per-model tables for the
virtual ID. **Replacing** an existing 400–611 slot needs none of the engine work, but it changes that model for
every player using that ID.

## 9. Skin limits (Phase E)

- R5 `IsValidPedModel` 0x100B3DD0: `0 ≤ id ≤ 30000` (0x7530, a DL remnant) **and** `GetModelInfo(id)` non-null **and**
  vtable == 0x85BDC0 (CPedModelInfo). With the DL flag off, GetModelInfo limits id to 0..20000. Callers:
  ScrSetPlayerSkin 0x10019246, ScrCreateActor 0x1000EB44, `CPed::SetModelIndex` 0x100AFF73, textdraw preview 0x100B34E4/0x100B3563. BINARY_CONFIRMED.
- Invalid skin → the chat warning "SetPlayerSkin %d isn't a valid ped model." (0x100E63CC) and no change.
- Ped store 319 (R5) with 288 used. Free IDs for skins: any unused ID < 20000 (§4).
- open.mp `Player::setSkin` (`Server/Source/player.cpp:287`) does not validate; it only maps DL custom skins to base skins.
- **Extra skin class: CLIENT_REGISTRATION_ONLY.** It needs an IDE `peds` line (with pedstats, anim group and voice valid
  for 1.0), a DFF+TXD in an IMG, and registration before level load. It needs no R5 patch and no protocol change,
  **as long as every client that can see that skin has the model**. A vanilla client would reject the ID
  (its validity check fails), so the server must give non-SKR clients a fallback skin.
- **Dormant 0.3.DL artwork in R5: YES** (§12). The DL skin path (AddCharModel, 20001–30000) exists only as
  manager code. R5's native code cannot render custom skins (EXTERNAL_REFERENCE customskin-0.3.7-R5 states the same).

## 10. Object limits (Phase F)

Four separate states that must not be conflated:

| State | Meaning | Count / evidence |
|---|---|---|
| Unused ID | no IDE/code registration | 3725 (SOURCE_CONFIRMED absence) |
| Unallocated model-info entry | `ms_modelInfoPtrs[id]==NULL` at runtime | **UNKNOWN until probe P1** |
| Available descriptor | a free CAtomicModelInfo element in the store | ≳4514 (R5 20000-entry store; INFERRED usage) |
| Fully supported custom object | ID + descriptor + streaming entry with an IMG position + TXD slot < 5000 + collision (COL slot or embedded) + client availability + server fallback | **0 today** (no SKR pipeline) |

R5 object creation: `ScrCreateObject` (RPC 44, 0x1001B340) → `CObjectPool::Create` 0x100128D0. A negative model is
handled only if the DL manager [0x1026EB98] exists (always NULL in R5). Then `CObject::CObject` 0x100A9110 →
`IsValidModel` 0x100B4570 (`GetModelInfo(id)` non-null) → otherwise model **18631** (0x48C7). Object id < 1000.
BINARY_CONFIRMED. *Correction to RE doc `26-pool-uri-jucatori-vehicule-remote.md` §26.2: 0x100128D0 is
`CObjectPool::Create`, not VehiclePool_Create. The RE wire table also names RPC 44 "WorldVehicleAdd";
RPC 44 is CreateObject and **164** is WorldVehicleAdd.*

## 11. What happens with a bad registration (Phase G)

| Case | Result | Tag |
|---|---|---|
| DFF in an IMG, no model info with that name | entry ignored at CD-directory load; never streamable | EXTERNAL_REFERENCE (gta-reversed LoadCdDirectory) |
| model info, no IMG entry | CStreamingInfo size 0; RequestModel never completes; R5 `CEntity::SetModelIndex` 0x1009EF50 **spins Sleep(1) up to 200×** on the GTA thread, then logs "Model %u wouldn't load in time!" and leaves the model unchanged | BINARY_CONFIRMED (loop 0x1009EFB5–0x1009EFC0) |
| RW resource loaded by hand (RpClumpStreamRead etc.) but no model info | the GTA entity cannot reference it (entities hold model IDs); R5 IsValidModel fails → fallback 18631 / "isn't a valid ped model" | BINARY_CONFIRMED (fallback paths); hand-loaded RW objects are INFERRED |
| model info present, wrong type for its use (e.g. atomic used as a vehicle) | R5 rejects vehicles by range, peds by vtable; GTA vehicle creation with a non-vehicle model info dereferences vehicle-only fields → crash | BINARY_CONFIRMED (R5 checks); GTA crash is INFERRED |
| store overflow (213th vehicle, 320th ped…) | silent overwrite of the next global (vehicle → ped count at 0xB478F8) | BINARY_CONFIRMED layout; effect INFERRED |
| 9th IMG archive | AddImageToList returns 0 → the CD positions are attributed to archive 0 → wrong data streamed | BINARY_CONFIRMED 0x1567BA8 |
| COL slot 256 | CPool::New returns null → crash in AddColSlot (INFERRED) | INFERRED |

## 12. R5 custom-model capabilities (Phase H)

**Dormant DL client in samp.dll (BINARY_CONFIRMED):**

| Component | Address | Evidence |
|---|---|---|
| ModelRequest-shaped RPC handler (reads u32,u32,u8,i32,i32,i32,u32,u32,u32,u32,u8,u8 = open.mp `ModelRequest` RPC 179 layout) | 0x10011EB0 | creates the manager (`new 0x206D bytes` → 0x1000DE20) into [0x1026EB98], then 0x1000BD60 init |
| second handler (manager init + `0x1000DB30(0)`, FinishDownload-like) | 0x1000EBA0 | no reference |
| further handlers | 0x100120B0, 0x1000EC30 | no reference |
| cache/download | strings 0x100E5BD8 `%s/0x%X.dff`, 0x100E5D00 `DownloadRedirect`, 0x100ED898 `model_cache`, 0x100ED8C0 `artwork_proxy`; functions 0x1000BE30…0x1000E160 | xrefs only from the chain above |
| loader | 0x100A7C30 ("Failed to load model ID"), 0x100A7CC0 ("model loader failed to allocated a new col", size cap 0x40000) | reached only via the manager |
| model table | 65535 slots at 0x101625B0, base pointer 0x101825AC (−32767..32767), flag [0x101A25AC] | §3.2 |
| object hook | CObjectPool::Create negative-model branch 0x10012900–0x10012956 | guarded by [0x1026EB98] |

The R5 client registers RPC IDs ≤ 178 only (`RE/client/data/client_rpc_table.csv`, 141 RPCs). No code or data
reference exists to 0x10011EB0, 0x1000EBA0, 0x100120B0, 0x1000EC30, 0x100A7A00, 0x100A7970 or 0x100AA4E0
(raw-byte and instruction search). **So in R5 the code is present but unreachable.** Activating it would
mean registering RPCs 179–185 and making the server treat SKR as a DL client. That conflicts with the SKR
policy that rejects 0.3.DL (`server/skr-gate-omp/client_policy.cpp:431,486,514`; CLIENT-PROTOCOL.md).
It also supports only objects (negative IDs) and skins (20001–30000), **not vehicles**.

## 13. External references (Phase H) — evidence only, no code copied

| Lead | Relevance | Tag |
|---|---|---|
| ricardoofnl/customskin-0.3.7-R5 (GitHub README) | Independently states that R5 contains a dormant DL custom-model client. Activates it by registering its RPC handlers and **masquerading as 0.3.DL (version 4062)**; supplies its own HTTP download + CRC32 (R5's worker is stubbed); renders skins by swapping RW clumps. Confirms §12. Masquerading as DL is incompatible with SKR policy. | EXTERNAL_REFERENCE |
| Knogle/libsamp | A drop-in R5 samp.dll replacement project. Claims a custom atomic-object pipeline; no detail in the README. | EXTERNAL_REFERENCE (low detail) |
| DK22Pac/plugin-sdk (local fork `plugin-sdk-sa-llm`) | Addresses for CModelInfo/AddXModel/stores used to cross-check | EXTERNAL_REFERENCE + BINARY_CONFIRMED |
| gta-reversed | Store element types, CStreaming resource ranges (26316 total), LoadCdDirectory behaviour, MI_* names for 374–381 | EXTERNAL_REFERENCE (not fetched this session; from prior knowledge, flagged as such) |
| open.mp (`server-build/open.mp`) | CustomModels component: skins 20001–30000, objects −30000..−1000, DL clients only, base-model fallback for everyone else (`Server/Components/CustomModels/models.cpp:600-690`; `SDK/include/values.hpp:63-66`); RPCs 179/181–185 (`Shared/NetCode/custommodels.hpp`) | SOURCE_CONFIRMED |

## 14. Reuse matrix — existing SKR resource infrastructure (Phase I)

No new Resources Manager is proposed. Reuse targets:

| Need | Existing piece | Where | Reuse? | Gap |
|---|---|---|---|---|
| Resource manifest (path, size, hash) | omp-cef server `ResourceManager` (`FileInfo{relativePath,fileSize,fileHash}`, JSON manifest) | `deps/omp-cef/src/server/common/resource_manager.{hpp,cpp}` | **Yes**, the manifest schema | needs model metadata (type, virtual ID, base ID, IDE line, txd name) |
| Packaging | encrypted `.pak` per resource (`AddResource(name, master_key)`, `GetPakContent`) | same | **Yes** for transport | GTA cannot stream from a PAK; the client must unpack into an SKR-owned IMG/cache |
| Download + chunking | `FileData` chunks, assembly, progress UI | `deps/omp-cef/src/client/core/system/resource_manager.cpp:184-230`, `ui/download_dialog.cpp` | **Yes** | runs after connect, but model registration is needed **before level load**, which forces a reconnect or a pre-join phase (see Options) |
| Cache + integrity | per-server cache `GTA User Files/cef/cache/<ip>/`, SHA-256 verification before use | `resource_manager.cpp:30-47,109-140` | **Yes** | separate model-cache root recommended |
| Updater manifest (SHA-256, size) | `shared/updater/manifest.h` | launcher | **Yes** for shipping a *base* SKR model pack with the client | — |
| Pawn natives | Gate `kNatives[]`, SKRE_* API (6 natives, 3 callbacks) | `server/skr-gate-omp/native_api_server.cpp`, `server/gamemode-integration/skr_enhanced.inc` | **Yes** (e.g. SKR_AddVehicleModel / SKR_CreateVehicleEx) | new natives = API change, needs approval |
| Gate transport | RPC 240 (c→s), 241 (s→c) 0xE0 envelope, steps 5–8 used by the API | `docs/ai-memory/TASK-API-001-STATE.md`, `shared/protocol/native_api.h` | **Yes** for per-entity identity | must be ordered with RPC 164 on the same channel |
| Client capability negotiation | Enhanced handshake (caps 0x07 fixed) + separate API handshake | same | **Yes**, via the API handshake (do not change caps) | — |

## 15. Network identifiers (Phase J)

| Identifier | Domain | Carried in | Validated at |
|---|---|---|---|
| GTA internal model ID | 0..19999 | — | GTA: none; R5 GetModelInfo 0x100A7A40 (0..20000) |
| SA-MP network model ID | vehicle 400..611; skin u32; object i32; pickup i32 | RPC 164 `ModelID` int32; 153 SetPlayerSkin u32 (+CustomSkin for DL); 32 WorldPlayerAdd skin; 68/128 SpawnInfo skin; 171 CreateActor skin; 44 CreateObject model; 95 CreatePickup model; 113 attached object model; 84 material model; 134 textdraw preview model | R5: 0x1000E741 (vehicle), 0x100B3DD0 (ped), 0x100B4570 (object/pickup → 18631), 0x1001B1BE (material ≤ 20000). Server: `isValidVehicleModel` |
| Entity ID | vehicle < 2000, object < 1000, player, actor | every RPC/sync packet | R5 0x1000E769 (`cmp cx,0x7D0`), 0x1001B5E7 (`cmp ax,0x3E8`) |
| SKR virtual model ID | proposal: vehicle-only namespace, e.g. 621 | **must not** travel in RPC 164's ModelID for R5 | SKR Gate (new) |
| Resource ID | manifest name/hash | CEF/SKR resource channel | SHA-256 (omp-cef) |
| Base-model fallback | a valid 400..611 / ped / object ID | the field the R5 client sees | the existing R5 checks |

**Late joiners and re-streams:** open.mp sends `StreamInVehicle` (RPC 164) every time a vehicle streams in for a
player (`Shared/NetCode/vehicle.hpp:105`). R5 destroys and re-creates on stream-out/in (RPC 165). So the custom
identity must be (a) stored server-side per vehicle entity, (b) re-sent with **every** 164 for SKR players, and
(c) delivered **before or atomically with** 164. Use the same RakNet ordering channel (`OrderingChannel_SyncRPC`),
or have the client hold 164 until the mapping arrives. Non-SKR players receive only the base model.

## 16. Performance constraints

Hot paths where file I/O, allocation, linear scans, blocking IPC or downloads are unacceptable:

| Path | Why it is hot | Rule |
|---|---|---|
| `CStreaming::RequestModel` / `LoadAllRequestedModels` / CdStream callbacks (0x4087xx–0x40Exxx) | called many times per frame | virtual→engine ID mapping must be O(1) (a flat array), no locks, no allocation |
| R5 `CEntity::SetModelIndex` 0x1009EF50, `CVehicle` ctor 0x100B83D0, `CreatePickup` 0x100A11F0, `SetMaterial` 0x100A8530, `AddComponent` 0x100B7FC0, `CActor` ctor 0x1009C2B0 | R5 already **blocks the GTA thread with Sleep loops** until the model is resident | every custom model must be fully on disk, registered and streamable **before** any create RPC; never download or decompress here |
| RakClient RPC handlers (GTA thread) | run inside `CNetGame::Process` | no file I/O, hashing or HTTP; queue the work |
| `CGame::Process` tick / `CHud::Draw` hooks | every frame | no scans over 20000 model IDs; precompute |
| IDE/IMG registration | once, during level load | may do file I/O, but bounded (a single IMG directory read) |
| Gate onTick (server) | main thread | per-player mapping lookups O(1); no per-tick re-send of mappings |
| Downloads | — | only pre-join or out-of-game (launcher); never during gameplay, because registration can't happen mid-session |

## 17. Runtime probes (NOT run — each needs approval)

**P1 — read-only engine census (recommended first).** An external process (a small console tool, *not* injected)
that attaches to a running `gta_sa.exe` with `PROCESS_VM_READ` after the SA-MP connect screen (game state 9) and
reads:
`ms_modelInfoPtrs[0..19999]` (0xA9B0C8) → non-null count and vtable per ID (atomic 0x85BBF0…, ped 0x85BDC0,
vehicle, clump…); store counts 0xAAE950, 0xB1BF58, 0xB1C934, 0xB1C960, 0xB1E128, 0xB1E158, 0xB1E958, 0xB1F650,
0xB478F8; the TXD pool [0xC8800C] and COL pool [0x965560] (size and used-flag count); `ms_files` 0x8E48D8 (8×0x30 names);
the samp.dll flag [samp+0x1A25AC] (expected 0).
*Safety:* reads only; no writes, no thread suspension, no injection; detaches immediately. Anti-cheat (SKRAC)
might flag an external handle: test offline only.
*Expected output:* a CSV that updates `skr_model_slots.csv` (`registration_status` per ID) and turns the INFERRED
counts in §6 into RUNTIME_OBSERVED. Expected: vehicle 212, ped 288, IMG 6, COL ≈253, flag 0.

**P2 — ID 612–614 / candidate run check.** Same tool, compare P1 against the IDE census to confirm no hidden registrations.

Neither probe is installed. Approve P1 before any slot decision.

## 18. Open questions, blockers, next experiments

**Blockers (fundamental):**
1. The vehicle store is full (212/212) and R5 does not extend it. Any **additional** vehicle needs Option C engine work.
2. R5 drops every vehicle model outside 400..611 in RPC 164. A virtual vehicle ID needs a side channel or a client patch.
3. Runtime usage of the COL, TXD and damage-atomic stores is UNKNOWN until P1.

**Open questions for Gheorghe & Staark:**
- Is replacing an existing vehicle slot acceptable for the PoC (e.g. Dacia Logan as `/v 621` → engine slot X),
  or must the PoC add a 213th vehicle?
- Should non-SKR clients (R5/open.mp) see the base model (fallback), or should custom vehicles be SKR-only?
- Must models be in the base client package (shipped by the updater), or downloaded from servers? Downloading
  needs a pre-join phase, because registration must happen before level load.
- Is a GTA vehicle-store relocation (Option C) acceptable, given SKR's anti-cheat and the existing R5 store patches?

**Next experiments (each needs separate approval):** P1 census → choose a candidate engine slot → a design review of the
Option B wire format (the Gate envelope step after 8) → only then a Dacia Logan PoC.

**Unrelated note:** the GPS work (API-003.2) in the working tree was not touched.
