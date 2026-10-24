# SKR Model Extension Options (M0 audit companion)

This compares three ways to add models. **No option is selected.** Gheorghe and Staark decide
after reviewing `SKR_MODEL_SLOT_AUDIT.md`. Evidence references point into that report (§n).

Goal used for comparison: `/v 621` → Dacia Logan, `/v 411` → Infernus, multiplayer-safe, with
SA-MP 0.3.7-R5 / open.mp compatibility preserved and 0.3.DL still rejected.

## Option A: use existing unused engine slots

Register new models in IDs that have no IDE line (3725 candidates, §4), during GTA's IDE pass.

| Model kind | Feasible? | Why |
|---|---|---|
| Vehicle | **No** (not without replacing one) | Vehicle store 212/212 full, not relocated by R5 (§6, §8). R5 drops RPC 164 for models outside 400..611. |
| Skin | **Yes** | Ped store 319 (R5) with 288 used. The R5 check is "is a CPedModelInfo" (§9). The server does not range-check. |
| Object | **Yes, limited** | The atomic store has ~4500 free entries (R5). Limits: COL slots (~2 free) and IMG archives (~2 free). Pack all SKR objects into **one** SKR IMG and **one** COL archive, or use DFF-embedded collision. |

- Engine work: SKR registration code that runs inside or right after the IDE pass, plus one SKR IMG. There are no store relocations and no R5 patches.
- Network: none for skins and objects, as long as every receiving client has the model. Non-SKR clients get an
  invalid-model fallback: R5 shows object 18631, and rejects the skin with a chat warning. **The server must
  substitute base models for non-SKR players.** The Gate knows each client type.
- Risk: low to medium. Every candidate ID needs probe P1 first. An ID used by a future open.mp/SA-MP data update would collide.
- **For `/v 621`: only as a replacement.** The Logan would replace an existing 400–611 vehicle for SKR clients.
  That is an existing-slot remap: Option B's mapping with zero new engine slots.

## Option B: virtual IDs + internal mapping

SKR defines a separate, **vehicle-only virtual namespace**. Example: virtual 621 = "Dacia Logan". The server keeps,
per vehicle entity: `virtualId`, `engineSlot` (what the SKR client creates) and `baseModel` (a valid 400..611
model sent in RPC 164).

- R5 compatibility: RPC 164 always carries `baseModel` (400..611), so R5's check at 0x1000E741 passes unchanged. Non-SKR
  players see the base model (e.g. a Premier for the Logan).
- SKR clients: the Gate sends `{vehicleId → engineSlot}` on the 0xE0 envelope (a new step after 8), **on the same ordering
  channel as 164**, before every stream-in. The SKR client swaps the model at creation: it hooks the R5 vehicle create
  path or applies `SetModelIndex` before the entity is shown. Late joiners and re-streams are handled by re-sending
  on every 164 (§15).
- Engine slot choices: (B1) an existing vehicle slot, remapped client-side for SKR clients only. This replaces
  that model **for SKR clients** for every vehicle that uses it, so a dedicated, rarely used slot must be chosen.
  (B2) a new slot from Option C.
- Pawn: new natives such as `SKR_CreateVehicleEx(virtualId, …)` and `SKR_GetVehicleVirtualModel`, plus `/v` command
  handling. This is an API change and needs approval (CLAUDE.md §4).
- Server data: open.mp's per-model tables (`MAX_VEHICLE_MODELS = 212`; seats, sizes, components) stay keyed by
  `baseModel`. Gameplay (seats, component validity) follows the base model, so the base must be chosen to match.
- Risk: medium. It needs protocol work, client hooks in R5's vehicle create path, and an ordering guarantee.
- Reuse: Gate transport, the API handshake and the CEF-style manifest (§14).

## Option C: extended pools + hooks

Relocate GTA's `ms_vehicleModelInfoStore` (212 → N) the way R5 already relocates the ped and atomic stores
(patch the store base and count sites in AddVehicleModel 0x4C6770, ShutDown 0x4C64EC/0x4C64F7 and any other
references). Optionally, raise the global 20000 limit too.

- Additional vehicles (213th+) become possible **inside** IDs < 20000 (e.g. 612–614 or any unused ID), with no change
  to `ms_modelInfoPtrs` or `ms_aInfoForModel`.
- Raising the 20000 limit is **much larger work**: 707 + 59 references to `ms_modelInfoPtrs`, all CStreaming range
  constants (TXD base 20000, COL 25000…), and R5's own 0..20000 checks. R5's dormant relocator covers only the
  `ms_modelInfoPtrs` sites, **not** streaming. **Not recommended.**
- It still needs Option B's network layer: R5 rejects models > 611 in RPC 164. Without a client patch to
  0x1000E741, new vehicles must travel as a base model + SKR mapping.
- Other per-vehicle limits to verify: VehicleStruct pool 127 (R5); GTA (id−400) switch tables such as
  0x47D230 (400–499 only, so a new id falls through to the default path); handling lines; carcols, carmods and
  the vehicle audio settings tables (UNKNOWN until a Ghidra pass); open.mp static tables.
- Risk: high. It is a GTA memory patch that coexists with R5's own patches, it needs anti-cheat whitelisting, and
  it is fragile with exe variants.
- Do **not** activate R5's dormant DL relocator. It requires DL RPC registration, objects and skins only, and DL masquerade.

## Comparison

| Criterion | A (unused slots) | B (virtual + mapping) | C (extended pools) |
|---|---|---|---|
| Extra vehicle without replacing | no | only with B2 (= needs C) | **yes** (with B's wire layer) |
| Extra skin | **yes** | n/a (not needed) | n/a |
| Extra object | **yes** (COL/IMG-bounded) | n/a | yes |
| `/v 621 → Logan` | replacement only | **yes**, as virtual→existing slot (B1) or →new slot (B2) | yes with B |
| GTA memory patches | none | client create-path hook | store relocation |
| R5 patches | none | none (base model on the wire) | none if B is used; otherwise 0x1000E741 |
| Protocol / Gate changes | server-side base substitution | **new envelope step** + natives | same as B |
| Non-SKR clients | fallback needed | see base model | see base model |
| 0.3.DL policy | unaffected | unaffected | unaffected |
| Effort / risk | low / low-medium | medium / medium | high / high |

## Suggested sequencing (for discussion, not a decision)

1. Probe P1 (read-only census) → confirm candidate IDs and the COL/IMG/TXD headroom.
2. Skins/objects via A, if wanted.
3. Vehicles via B1 for the PoC (virtual 621 → a chosen existing slot, base model on the wire), so the network design
   is validated without engine surgery.
4. Option C only if "add without replacing" becomes a hard requirement.
