# Operation 215 × SA-MP CUSTOM.IMG — integration record

Port of SKR-EXP-001 Operation 215 (asset profile DUAL, certified D5 + D3.4) into SKR Platform.
operation213 (relocated CVehicleModelInfo store, capacity 215) is included only as its dependency.
No ninth IMG slot is allocated and `samp.dll` / `gta_sa.exe` are not modified.

## Runtime flow
1. `SkrInitialize`, pre-resume, before `samp.dll` loads: `operation215::PrepareBeforeResume()`
   - verifies the active `SAMP\custom.img` SHA-256 and `models\player.img`;
   - locates the suspended primary thread;
   - applies the operation213 relocation (12 sites);
   - hooks `CStreaming::LoadCdDirectory` at `0x5B6170`.
2. Image-0 scan (`SAMP\CUSTOM.IMG`, main thread):
   - D0 snapshot;
   - D1: `AddVehicleModel` 612→212, 613→213;
   - metadata from Sultan 560;
   - the original scanner runs once;
   - D2B1 allowlist (5 associations).
3. Gameplay: requests go through a queue, one custom model in flight at a time.
   - D2B3 validates the loaded model.
   - PLAYER.IMG remaps are classified by `op215_preconditions`; a change to a protected ID unmaps the model.
4. Any failure: the model stays unmapped, is never requested, and SKR-X keeps the stock proxy.

## Inputs (pinned)
| File | SHA-256 |
|---|---|
| gta_sa.exe 1.0 US | A559AA772FD136379155EFA71F00C47AAD34BBFEAE6196B0FE1047D0645CBD26 |
| samp.dll 0.3.7-R5 | B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2 |
| models\player.img | BD6D43C065FD7767958C5C363E67FDAA92F203D7C766419524D9F220EEC98256 |
| SAMP\custom.img (staged) | C9E4FCE661F3F96F56C6FB1658D150C41F302257A70EDBDC8554697FFF3B1FDB |

## Reproducing CUSTOM.IMG
Sources (binary, gitignored):
- the vanilla SA-MP `custom.img`, 8,192 B, `20DE16A9…`;
- `assets/op215/skr_vehicles.img`, `83C88C9A…`.

```
python -I tools/op215_stage_custom_img.py <SAMP\custom.img> assets/op215/skr_vehicles.img C9E4FCE661F3F96F56C6FB1658D150C41F302257A70EDBDC8554697FFF3B1FDB
```

The tool:
- keeps the existing entries byte-for-byte;
- appends the four pinned spans (xc60 txd/dff, m4gts txd/dff);
- verifies the result, backs up the original and replaces it atomically;
- refuses while `gta_sa.exe` is running.

The output is byte-identical to EXP-001 `evidence/op215-d5-dual/CUSTOM.IMG` (verified 2026-10-25).

## Runtime result (operator, 2026-10-25)
- Operation 215 store, directory interception, D0/D1/D2B1: PASS.
- D2B3: PASS for 612 and for 613.
- SKR-X R5 hook swapped proxy 400 → 612 (twice). 613 rendered.
- 614 is unavailable, so the stock proxy is retained. Sultan 560 is unaffected.
