#include "skrx/skrx_vehicles_client.h"
#include "skrx/skrx_client.h"
#include "protocol/skrx_gateway.h"

#ifdef SKR_OP215
#include "vehicle/custom_vehicle.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>

// SKR-X VEHICLES client: model registry, assignment map, CVehiclePool::Create hook.
//
// NATIVE-FIRST: when the server streams in a proxy vehicle that has a custom
// model assignment, the hook inside samp.dll's CVehiclePool::Create swaps
// VehicleInfo::m_nType from the proxy model to the real custom model ID
// BEFORE samp.dll creates the CVehicle and GTA SA CAutomobile.  The vehicle
// is born as model 612, never lives as the proxy.
//
// Hook target: samp.dll R5 CVehiclePool::Create at RVA 0x1F080.
// __thiscall (ECX = CVehiclePool*, stack arg = VehicleInfo*).
//
// CRITICAL LAYOUT NOTE — samp-api uses #pragma pack(push, 1):
//   struct VehicleInfo {
//       ID      m_nId;       // offset 0, uint16_t, 2 bytes
//       int     m_nType;     // offset 2, int32_t, 4 bytes  (MODEL ID)
//       CVector m_position;  // offset 6, 3 floats, 12 bytes
//       float   m_fRotation; // offset 18, 4 bytes
//       ...
//   };
// m_nType is at byte offset 2, NOT 4.

namespace skr::skrx::vehicles {

using namespace skr::protocol;
using namespace skr::protocol::skrx;
using namespace skr::protocol::skrx::vehicles;

namespace {

// ── Diagnostic log (direct file write, same pattern as OP215) ──────
static char g_logPath[MAX_PATH] = {};

static const char* GetLogPath() {
    if (g_logPath[0]) return g_logPath;
    DWORD n = GetModuleFileNameA(nullptr, g_logPath, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    char* slash = strrchr(g_logPath, '\\');
    if (!slash) return nullptr;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - g_logPath), "SKR\\skrx_vehicles.log");
    return g_logPath;
}

static void Log(const char* fmt, ...) {
    const char* path = GetLogPath();
    if (!path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ── State ──────────────────────────────────────────────────────────
VehiclesClientState g_state;

// ── CVehiclePool::Create hook ──────────────────────────────────────
// samp.dll R5 RVA 0x1F080: BOOL __thiscall CVehiclePool::Create(VehicleInfo* pInfo)
//
// VehicleInfo is pack(1).  Layout:
//   offset 0: m_nId   (uint16_t) — SA-MP vehicle pool slot
//   offset 2: m_nType (int32_t)  — GTA SA model ID
//
// The function allocates a CVehicle wrapper, calls GTA SA CAutomobile ctor
// with m_nType, and registers the vehicle in the SA-MP pool.  Our hook
// intercepts m_nType BEFORE the function body runs.
constexpr DWORD kRVA_VehiclePoolCreate = 0x1F080;

// pack(1) offsets within VehicleInfo — derived from samp-api CVehiclePool.h
constexpr int kVI_Id   = 0;  // uint16_t m_nId
constexpr int kVI_Type = 2;  // int32_t  m_nType (model ID)

using CreateOrigFn = int(__thiscall*)(void* thisPtr, void* pInfo);

static uint8_t* g_trampoline = nullptr;

// GTA SA CStreaming::HasModelLoaded (0x4044C0) — quick check without OP215 dependency.
constexpr uintptr_t kHasModelLoaded = 0x4044C0;
using HasModelLoadedFn = bool(__cdecl*)(int);

static bool IsModelStreamedIn(int modelId) {
    auto fn = reinterpret_cast<HasModelLoadedFn>(kHasModelLoaded);
    __try {
        return fn(modelId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int __fastcall HookedCreate(void* thisPtr, void* /*edx*/, void* pInfo) {
    if (pInfo) {
        auto* info = static_cast<uint8_t*>(pInfo);

        // Read VehicleInfo fields (pack(1) layout).
        uint16_t poolId = *reinterpret_cast<uint16_t*>(info + kVI_Id);
        int32_t  modelId = *reinterpret_cast<int32_t*>(info + kVI_Type);

        uint16_t customModel = GetAssignedModel(poolId);
        if (customModel && modelId != static_cast<int32_t>(customModel)) {
            // Check OP215 model readiness before swapping.
            // If the custom model isn't streamed in, the CAutomobile ctor will
            // crash (null RwObject).  In that case, keep the proxy model —
            // the vehicle appears as the proxy until the model loads, and the
            // server's next stream-in or respawn will re-trigger.
            bool modelReady = false;

#ifdef SKR_OP215
            modelReady = skr::vehicle::IsInitialized()
                      && skr::vehicle::IsLoaded(static_cast<int>(customModel));
            if (!modelReady && skr::vehicle::IsInitialized()) {
                // Request asynchronous load for next attempt.
                skr::vehicle::RequestLoad(static_cast<int>(customModel));
            }
#endif

            if (modelReady) {
                // NATIVE-FIRST swap: replace proxy with real model.
                *reinterpret_cast<int32_t*>(info + kVI_Type) = static_cast<int32_t>(customModel);
                ++g_state.swapCount;
                Log("HOOK swap pool=%d proxy=%d -> model=%d swaps=%d",
                    poolId, modelId, customModel, g_state.swapCount);
            } else {
                ++g_state.deferCount;
                Log("HOOK defer pool=%d proxy=%d target=%d (model not loaded) defers=%d",
                    poolId, modelId, customModel, g_state.deferCount);
            }
        } else {
            // Ordinary vehicle (no assignment, or already correct model).
            Log("HOOK pass pool=%d model=%d (no swap needed)", poolId, modelId);
        }
    }

    return reinterpret_cast<CreateOrigFn>(g_trampoline)(thisPtr, pInfo);
}

// ── Trampoline construction ────────────────────────────────────────
// CVehiclePool::Create R5 entry bytes (verified against pinned binary):
//   55              push ebp
//   8B EC           mov ebp, esp
//   83 EC 08        sub esp, 8
// = 6 bytes, no relative branches — safe to relocate.
constexpr int kPrologSize = 6;
static const uint8_t kExpectedProlog[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};

bool BuildTrampoline(uintptr_t target) {
    auto* fn = reinterpret_cast<uint8_t*>(target);

    if (memcmp(fn, kExpectedProlog, kPrologSize) != 0) {
        Log("HOOK prolog mismatch at 0x%p: %02X %02X %02X %02X %02X %02X",
            fn, fn[0], fn[1], fn[2], fn[3], fn[4], fn[5]);
        return false;
    }

    g_trampoline = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_trampoline) {
        Log("HOOK VirtualAlloc failed for trampoline");
        return false;
    }

    // Copy original prolog bytes.
    memcpy(g_trampoline, fn, kPrologSize);

    // JMP rel32 to original + kPrologSize.
    g_trampoline[kPrologSize] = 0xE9;
    int32_t jmpOffset = static_cast<int32_t>(
        (target + kPrologSize) - (reinterpret_cast<uintptr_t>(g_trampoline) + kPrologSize + 5));
    *reinterpret_cast<int32_t*>(g_trampoline + kPrologSize + 1) = jmpOffset;

    Log("HOOK trampoline at 0x%p -> original+%d (0x%p)",
        g_trampoline, kPrologSize, reinterpret_cast<void*>(target + kPrologSize));
    return true;
}

bool WriteDetour(uintptr_t target) {
    auto* fn = reinterpret_cast<uint8_t*>(target);

    // Build the 6-byte patch: E9 rel32 + 90 (NOP).
    uint8_t patch[6];
    patch[0] = 0xE9;
    int32_t detourOffset = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&HookedCreate) - (target + 5));
    *reinterpret_cast<int32_t*>(patch + 1) = detourOffset;
    patch[5] = 0x90;

    DWORD old = 0;
    if (!VirtualProtect(fn, kPrologSize, PAGE_EXECUTE_READWRITE, &old)) {
        Log("HOOK VirtualProtect failed");
        return false;
    }
    memcpy(fn, patch, kPrologSize);
    VirtualProtect(fn, kPrologSize, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, kPrologSize);

    Log("HOOK detour written at 0x%p -> HookedCreate (0x%p)",
        fn, reinterpret_cast<void*>(&HookedCreate));
    return true;
}

// ── VehicleReady sender ────────────────────────────────────────────
// Sends VehicleReady (Op 4, C→S) through the gateway.
static bool SendVehicleReady(uint16_t poolId, uint16_t customModel,
                             uint32_t generation, uint8_t status) {
    VehicleReadyData ready;
    ready.vehiclePoolId = poolId;
    ready.customModelId = customModel;
    ready.generation    = generation;
    ready.status        = status;

    uint8_t inner[kVehicleReadySize];
    size_t innerLen = EncodeVehicleReady(ready, inner, sizeof(inner));
    if (!innerLen) return false;

    uint8_t buf[skr::protocol::skrx::kMaxGatewaySize];
    size_t total = EncodeVehiclesGateway(VehicleOp::VehicleReady, inner, innerLen, buf, sizeof(buf));
    if (!total) return false;

    // SendOnRpc240 is internal to skrx_client.cpp — use the gateway send path.
    // For now, the ready acknowledgement is logged but not sent until
    // the send path is exposed.  This is a known M2.1-B limitation.
    // TODO: expose SendOnRpc240 or add a SendGateway public API.
    ++g_state.readySentCount;
    Log("VehicleReady pool=%d model=%d gen=%u status=%u (queued, count=%d)",
        poolId, customModel, generation, status, g_state.readySentCount);
    return true;
}

// ── VEHICLES namespace message handlers ────────────────────────────

void OnModelRegistry(const uint8_t* data, size_t len) {
    ModelRegistryEntry entries[kMaxCustomModels];
    uint8_t count = 0;
    if (!DecodeModelRegistry(data, len, entries, count)) {
        Log("ModelRegistry decode fail len=%zu", len);
        return;
    }

    g_state.modelCount = 0;
    for (uint8_t i = 0; i < count && i < kMaxCustomModels; ++i) {
        auto& m = g_state.models[i];
        m.customModelId = entries[i].customModelId;
        m.proxyModelId  = entries[i].proxyModelId;
        m.donorModelId  = entries[i].donorModelId;
        memcpy(m.internalName, entries[i].internalName, entries[i].nameLen);
        m.internalName[entries[i].nameLen] = '\0';
        m.registered = false;
        ++g_state.modelCount;
    }
    Log("ModelRegistry received: %d models", g_state.modelCount);

#ifdef SKR_OP215
    // Verify each model against the OP215 engine and request pre-load.
    if (skr::vehicle::IsInitialized()) {
        for (int i = 0; i < g_state.modelCount; ++i) {
            auto& m = g_state.models[i];
            auto diag = skr::vehicle::GetDiag(static_cast<int>(m.customModelId));
            m.registered = diag.registered;
            if (diag.registered) {
                skr::vehicle::RequestLoad(static_cast<int>(m.customModelId));
                Log("ModelRegistry: model %d '%s' registered in OP215, load requested",
                    m.customModelId, m.internalName);
            } else {
                Log("ModelRegistry: model %d '%s' NOT in OP215 (server wants it, client doesn't have it)",
                    m.customModelId, m.internalName);
            }
        }
    } else {
        Log("ModelRegistry: OP215 not yet initialized, deferring registration check");
    }
#else
    Log("ModelRegistry: OP215 disabled at compile time (SKR_OP215=OFF)");
#endif
}

void OnVehicleAssign(const uint8_t* data, size_t len) {
    VehicleAssignData assign;
    if (!DecodeVehicleAssign(data, len, assign)) {
        Log("VehicleAssign decode fail len=%zu", len);
        return;
    }

    // Update or insert assignment.
    for (int i = 0; i < g_state.assignCount; ++i) {
        auto& a = g_state.assignments[i];
        if (a.active && a.vehiclePoolId == assign.vehiclePoolId) {
            a.customModelId = assign.customModelId;
            a.generation    = assign.generation;
            Log("VehicleAssign update pool=%d model=%d gen=%u",
                assign.vehiclePoolId, assign.customModelId, assign.generation);
            return;
        }
    }
    // New entry — find a free slot or append.
    for (int i = 0; i < g_state.assignCount; ++i) {
        if (!g_state.assignments[i].active) {
            auto& a = g_state.assignments[i];
            a.vehiclePoolId = assign.vehiclePoolId;
            a.customModelId = assign.customModelId;
            a.generation    = assign.generation;
            a.active        = true;
            Log("VehicleAssign reuse slot=%d pool=%d model=%d gen=%u",
                i, assign.vehiclePoolId, assign.customModelId, assign.generation);
            return;
        }
    }
    if (g_state.assignCount < kMaxAssignments) {
        auto& a = g_state.assignments[g_state.assignCount++];
        a.vehiclePoolId = assign.vehiclePoolId;
        a.customModelId = assign.customModelId;
        a.generation    = assign.generation;
        a.active        = true;
        Log("VehicleAssign new pool=%d model=%d gen=%u total=%d",
            assign.vehiclePoolId, assign.customModelId, assign.generation, g_state.assignCount);
    } else {
        Log("VehicleAssign FULL: cannot track pool=%d model=%d", assign.vehiclePoolId, assign.customModelId);
    }
}

void OnVehicleRevoke(const uint8_t* data, size_t len) {
    uint16_t poolId = 0;
    if (!DecodeVehicleRevoke(data, len, poolId)) {
        Log("VehicleRevoke decode fail len=%zu", len);
        return;
    }

    for (int i = 0; i < g_state.assignCount; ++i) {
        auto& a = g_state.assignments[i];
        if (a.active && a.vehiclePoolId == poolId) {
            a.active = false;
            Log("VehicleRevoke pool=%d", poolId);
            return;
        }
    }
    Log("VehicleRevoke pool=%d not found (already revoked?)", poolId);
}

void OnBatchAssign(const uint8_t* data, size_t len) {
    VehicleAssignData entries[kMaxBatchEntries];
    uint8_t count = 0;
    if (!DecodeBatchAssign(data, len, entries, count)) {
        Log("BatchAssign decode fail len=%zu", len);
        return;
    }

    for (uint8_t i = 0; i < count; ++i) {
        uint8_t buf[kVehicleAssignSize];
        if (EncodeVehicleAssign(entries[i], buf, sizeof(buf)))
            OnVehicleAssign(buf, kVehicleAssignSize);
    }
    Log("BatchAssign count=%d total_active=%d", count, g_state.assignCount);
}

void OnVehicleRespawn(const uint8_t* data, size_t len) {
    uint16_t poolId = 0;
    uint32_t generation = 0;
    if (!DecodeVehicleRespawn(data, len, poolId, generation)) {
        Log("VehicleRespawn decode fail len=%zu", len);
        return;
    }

    for (int i = 0; i < g_state.assignCount; ++i) {
        auto& a = g_state.assignments[i];
        if (a.active && a.vehiclePoolId == poolId) {
            a.generation = generation;
            Log("VehicleRespawn pool=%d gen=%u", poolId, generation);
            return;
        }
    }
    Log("VehicleRespawn pool=%d not found", poolId);
}

} // anonymous namespace

// ── Public API ─────────────────────────────────────────────────────

bool Init() {
    g_state = {};
    Log("=== SKRX VEHICLES Init ===");
    return true;
}

void Reset() {
    Log("=== SKRX VEHICLES Reset (models=%d assigns=%d swaps=%d defers=%d) ===",
        g_state.modelCount, g_state.assignCount, g_state.swapCount, g_state.deferCount);
    g_state.modelCount = 0;
    g_state.assignCount = 0;
    g_state.swapCount = 0;
    g_state.deferCount = 0;
    g_state.readySentCount = 0;
    for (auto& m : g_state.models) m = {};
    for (auto& a : g_state.assignments) a = {};
}

VehiclesClientState& GetState() { return g_state; }

uint16_t GetAssignedModel(uint16_t vehiclePoolId) {
    for (int i = 0; i < g_state.assignCount; ++i) {
        const auto& a = g_state.assignments[i];
        if (a.active && a.vehiclePoolId == vehiclePoolId)
            return a.customModelId;
    }
    return 0;
}

const ModelEntry* FindModel(uint16_t customModelId) {
    for (int i = 0; i < g_state.modelCount; ++i) {
        if (g_state.models[i].customModelId == customModelId)
            return &g_state.models[i];
    }
    return nullptr;
}

void OnVehiclesMessage(const GatewayHeader& hdr,
                       const uint8_t* payload, size_t payloadLen) {
    if (hdr.version != kVehiclesVersion || payloadLen < 1) {
        Log("version mismatch ver=%u expected=%u", hdr.version, kVehiclesVersion);
        return;
    }

    const auto op = static_cast<VehicleOp>(payload[0]);
    const uint8_t* opData = payload + 1;
    const size_t opLen = payloadLen - 1;

    switch (op) {
    case VehicleOp::ModelRegistry:
        OnModelRegistry(opData, opLen);
        break;
    case VehicleOp::VehicleAssign:
        OnVehicleAssign(opData, opLen);
        break;
    case VehicleOp::VehicleRevoke:
        OnVehicleRevoke(opData, opLen);
        break;
    case VehicleOp::VehicleBatchAssign:
        OnBatchAssign(opData, opLen);
        break;
    case VehicleOp::VehicleRespawn:
        OnVehicleRespawn(opData, opLen);
        break;
    default:
        Log("unexpected op %u from server", static_cast<unsigned>(payload[0]));
        break;
    }
}

bool InstallVehiclePoolHook(uintptr_t sampBase) {
    if (g_state.hookInstalled) return true;

    uintptr_t target = sampBase + kRVA_VehiclePoolCreate;

    Log("HOOK install: samp=0x%p target=0x%p (RVA 0x%X)",
        reinterpret_cast<void*>(sampBase), reinterpret_cast<void*>(target), kRVA_VehiclePoolCreate);

    if (!BuildTrampoline(target)) {
        Log("HOOK FAILED: trampoline build");
        return false;
    }
    if (!WriteDetour(target)) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
        Log("HOOK FAILED: detour write");
        return false;
    }

    g_state.hookInstalled = true;
    Log("HOOK installed: trampoline=0x%p prolog=%02X%02X%02X%02X%02X%02X",
        g_trampoline,
        g_trampoline[0], g_trampoline[1], g_trampoline[2],
        g_trampoline[3], g_trampoline[4], g_trampoline[5]);
    return true;
}

} // namespace skr::skrx::vehicles
