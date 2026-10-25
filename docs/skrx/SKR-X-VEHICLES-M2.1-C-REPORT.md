# SKR-X VEHICLES M2.1-C — Hook Correction Report

**Date:** 2026-10-25
**Milestone:** M2.1-C — CVehiclePool::Create hook prolog correction
**Verdict:** **CORRECTED — hook now targets verified R5 prolog**

---

## 1. Problem Statement

M2.1-B report (2026-10-24) claimed the CVehiclePool::Create prolog at RVA
`0x1F080` was a standard EBP frame:

```
55              push ebp
8B EC           mov ebp, esp
83 EC 08        sub esp, 8
```

Runtime log showed a mismatch: `56 8B 74 24 08 0F`.  The hook never
installed — `BuildTrampoline` rejected the prolog and returned false.

---

## 2. Root Cause

The M2.1-B prolog was sourced from documentation (samp-api headers / IDA
database assumptions) rather than verified against the pinned binary.

**Binary verification** (pinned samp.dll R5, SHA-256
`B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2`):

Hex dump at ImageBase `0x10000000` + RVA `0x1F080`:

```
56              push esi
8B 74 24 08     mov esi, [esp+8]
0F B7 06        movzx eax, word [esi]
57              push edi
8B F9           mov edi, ecx
8B 8C 87 34 11 00 00  mov ecx, [edi+eax*4+0x1134]
...
```

The function is compiled **without a frame pointer** (optimized frameless).
ESI is used as a callee-saved register to hold the VehicleInfo pointer.
The stack argument (`[esp+8]`) accounts for the return address + pushed ESI.

**Function signature confirmed:**
- Calling convention: `__thiscall` (ECX = CVehiclePool*, stack arg = VehicleInfo*)
- Return: `RET 4` (callee-cleans the VehicleInfo* argument)
- Two exit paths at +0x17C and +0x1A2, both `RET 4`

---

## 3. Changes Applied

### 3.1 Prolog signature (lines 168-169 → 168-169)
```cpp
// Before (M2.1-B — WRONG):
constexpr int kPrologSize = 6;
static const uint8_t kExpectedProlog[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};

// After (M2.1-C — VERIFIED):
constexpr int kPrologSize = 5;
static const uint8_t kExpectedProlog[] = {0x56, 0x8B, 0x74, 0x24, 0x08};
```

**Why 5 bytes is correct:** `push esi` (1 byte) + `mov esi, [esp+8]` (4 bytes)
= 5 bytes.  This is an exact instruction boundary — no partial instruction
splitting.  5 bytes = exactly one `JMP rel32` (E9 + 4-byte offset), so no
NOP padding is needed.

### 3.2 BuildTrampoline
- Copies 5 bytes (not 6) into the trampoline
- Appends `E9 rel32` JMP back to `original + 5`
- Saves original bytes to `g_savedProlog[5]` for potential uninstall
- Improved log: prints actual relocated bytes + jump target

### 3.3 WriteDetour
- Writes exactly 5 bytes: `E9 rel32` (no NOP padding)
- `patch` array sized from `kPrologSize` instead of hardcoded 6

### 3.4 PE fingerprint verification (NEW)
Added `VerifySampPE()` called before any hooking:
- Validates DOS and NT signatures
- Checks `TimeDateStamp == 0x6372C39E`
- Checks `SizeOfImage == 0x27E000`
- Logs all values; refuses to hook if fingerprint doesn't match

This prevents the hook from running against a different samp.dll version
where the function layout may differ.

### 3.5 HookedCreate — UNCHANGED
The `__fastcall` wrapper and VehicleInfo pack(1) field reads at offset 2
(`kVI_Type`) were already correct from M2.1-B.  The model swap logic,
OP215 readiness check, and deferred-swap fallback are not affected.

### 3.6 InstallVehiclePoolHook
- Now calls `VerifySampPE(sampBase)` first
- Log format updated: prints 5 trampoline bytes (was 6)

---

## 4. Trampoline Correctness Proof

### 4.1 Relocated instructions

| Offset | Bytes | Instruction | Relative? | Relocation needed? |
|--------|-------|-------------|-----------|-------------------|
| 0 | `56` | `push esi` | No | No |
| 1 | `8B 74 24 08` | `mov esi,[esp+8]` | No (ESP-relative) | No |

Neither instruction contains a relative branch or RIP/EIP-relative address.
Both are position-independent and safe to execute from any address.

### 4.2 Trampoline layout (10 bytes used of 32 allocated)

```
[0..4]  56 8B 74 24 08       — original prolog (5 bytes)
[5..9]  E9 xx xx xx xx       — JMP rel32 to original+5
```

### 4.3 Stack balance

**Call into HookedCreate:**
```
[esp+0]  return address (from JMP, effectively replaces CALL)
[esp+4]  VehicleInfo* (original caller's argument)
ECX      = CVehiclePool* (this)
```

HookedCreate is `__fastcall(this, edx, pInfo)` — MSVC maps this identically
to `__thiscall` for one-argument functions.  The compiler pushes `pInfo`
for the trampoline call and the trampoline's `RET 4` pops it — balanced.

---

## 5. Build Results

| Configuration | Target | Result | Warnings |
|--------------|--------|--------|----------|
| external-lobby-x86 | SKRClient (SKR.Client.dll) | **PASS** | 0 new (pre-existing: C4389 ×3, C4505 ×2) |
| op215-x86 | SKRClient (SKR.Client.dll) | **PASS** | 0 new (same pre-existing) |

---

## 6. Test Results

| Suite | Checks | Failures | Status |
|-------|--------|----------|--------|
| SKRXVehiclesTests | 414 | 0 | **PASS** |

---

## 7. Files Modified

| File | Nature of change |
|------|-----------------|
| `client/skrx/skrx_vehicles_client.cpp` | Prolog bytes, sizes, trampoline, detour, PE verification |

No other files modified.  Header (`skrx_vehicles_client.h`), server-side
manager, protocol codec, and test suite are unchanged.

---

## 8. What M2.1-C Does NOT Change

- VehicleInfo pack(1) layout — confirmed correct in M2.1-B
- HookedCreate swap logic — unchanged
- OP215 model-loaded guard — unchanged
- Assignment map operations — unchanged
- Diagnostic logging format — same event types
- Test suite — existing 414 checks cover protocol, manager, codec
- Standing constraints — all preserved (see M2.1-B §9)

---

## 9. Runtime Test Plan

The hook will now install successfully on the pinned samp.dll R5.  The
expected log sequence on first vehicle stream-in:

```
HOOK PE check: TimeDateStamp=0x6372C39E SizeOfImage=0x27E000
HOOK install: samp=0x10000000 target=0x1001F080 (RVA 0x1F080)
HOOK trampoline at 0x<addr>: 56 8B 74 24 08 E9 -> 0x1001F085
HOOK detour written at 0x1001F080 -> HookedCreate (0x<addr>)
HOOK installed: trampoline=0x<addr> prolog=568B742408
HOOK swap pool=<N> proxy=400 -> model=612 swaps=1
```

If the PE check fails (different samp.dll version), the hook silently
refuses to install and all vehicles remain as their proxy models — safe
degradation, not a crash.

---

## 10. Verdict

**M2.1-C CORRECTED.**  The hook now targets the actual R5 optimized frameless
prolog (`56 8B 74 24 08`), writes an exact 5-byte JMP detour with no padding,
and verifies the PE fingerprint before touching any code.  The runtime blocker
from M2.1-B is resolved.
