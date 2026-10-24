# SKR Model Client/Server Matrix (M0 + M0-S)

Legend: **CONFIRMED** · **PARTIALLY SUPPORTED** · **UNSUPPORTED** · **UNKNOWN**. A cell describes capability on the
current builds (open.mp 1.6.7.0 @ 3d20f3d as SKR Runtime 1.0.23; SA-MP 0.3.7-R5 `b72b5dbe…`). Test success alone would
not prove multiplayer compatibility. References: client report `SKR_MODEL_SLOT_AUDIT.md` (§), server report
`SKR_OPENMP_MODEL_AUDIT.md` (S§).

| Capability | open.mp server | SA-MP R5 client | SKR extension required |
|---|---|---|---|
| Original vehicles (400–611) | **CONFIRMED**: 212 types, 2000 instances (S2) | **CONFIRMED**: RPC 164 accepts 400–611 (§8) | None |
| Additional vehicles (> 212 types) | **UNSUPPORTED**: `isValidVehicleModel`, no vehicle custom type (S2, S4) | **UNSUPPORTED**: vehicle store 212/212, RPC 164 drops > 611 (§6, §8) | Server: virtual-ID registry + Gate per-vehicle mapping on a base model. Client: create-path hook + vehicle store relocation (Option C), or a replacement slot (B1) |
| Custom skins | **PARTIALLY SUPPORTED**: registration CONFIRMED (AddCharModel 20001–30000), delivery to DL clients only (S4) | **PARTIALLY SUPPORTED**: ped store has ~31 free entries and the R5 check accepts any registered ped < 20000 (§9); no runtime download/registration path (dormant DL code unreachable, §12) | Client registration at IDE load in an unused ID; server substitutes a base skin for non-SKR clients |
| Custom objects | **PARTIALLY SUPPORTED**: registration CONFIRMED (AddSimpleModel −30000…−1000), DL only; plain objects not model-validated (S2) | **PARTIALLY SUPPORTED**: atomic store ~4500 free (R5-relocated); COL ~2 and IMG 2 slots left (INFERRED); negative IDs → 18631 (§10) | Client registration in unused IDs (one SKR IMG + one COL archive); server base-model fallback for non-SKR |
| Custom model names | **PARTIALLY SUPPORTED**: CustomModels keeps DFF/TXD names; no vehicle name table (S4) | **UNKNOWN**: no in-game name use found for custom IDs; vehicle names come from GXT keys in the IDE (INFERRED) | SKR registry metadata (display name per virtual ID) |
| DFF/TXD/COL streaming | **PARTIALLY SUPPORTED**: HTTP/CDN for DL clients only (S4); CEF PAK transfer exists for UI assets (§14) | **PARTIALLY SUPPORTED**: IMG streaming CONFIRMED for registered models; TXD streamable slots < 5000; COL pool 255 near full; dormant DL cache unused (§7, §12) | Pre-join download (reuse the CEF chunked/SHA-256 path) into an SKR IMG; registration before level load |
| Network model mapping | **PARTIALLY SUPPORTED**: `getBaseModel` exists for skins/actors/classes; per-RPC outbound handlers exist but the Gate does not use them (S Answers #10) | **UNSUPPORTED**: R5 has no mapping layer for vehicles; RPC 164 carries a raw ID (§15) | New Gate envelope step `{vehicleId, virtualModel, engineSlot}` ordered with RPC 164 at every stream-in + client apply hook |

Decision owners: Gheorghe and Staark.
