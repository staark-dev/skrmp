// ─────────────────────────────────────────────────────────────────────
// OP215 — SKR Enhanced Custom Vehicle System (Runtime)
// ─────────────────────────────────────────────────────────────────────
// GTA SA 1.0 US (x86).  All addresses verified against plugin-sdk /
// gta-reversed / SKR-EXP-001.  The module runs on the GTA thread only.
//
// Models are registered before the first frame by Operation 215
// (operation215/native_vehicle_registration, SAMP\CUSTOM.IMG, native scanner).
// This file reads that result, queues non-blocking requests through it and
// tracks load state for diagnostics and the F9 test spawn.
// ─────────────────────────────────────────────────────────────────────

#include "vehicle/custom_vehicle.h"
#include "operation215/native_vehicle_registration.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace skr::vehicle {
namespace registration = skr::operation215;
namespace {

// ═══════════════════════════════════════════════════════════════════════
// GTA SA 1.0 US Addresses  (plugin-sdk / gta-reversed provenance)
// ═══════════════════════════════════════════════════════════════════════

// CStreaming
constexpr uintptr_t kHasModelLoaded         = 0x4044C0;  // bool __cdecl(int)
constexpr uintptr_t kStreamingInfoArray     = 0x8E4CC0;  // CStreamingInfo[26316]
constexpr int kStreamingInfoSize            = 20;        // sizeof(CStreamingInfo)
constexpr int kSI_LoadState                 = 0x10;      // plugin-sdk VALIDATE_OFFSET

// Native vehicle construction (SKR-EXP-001 native_vehicle_spawn.cpp, D3/D5)
constexpr uintptr_t kVehicleNew             = 0x6E2D50;  // CVehicle::operator new(size) -> vehicle pool
constexpr uintptr_t kAutomobileCtor         = 0x6B0A90;  // CAutomobile(this, model, createdBy, setupSuspension)
constexpr uintptr_t kSetHeading             = 0x43E0C0;  // CPlaceable::SetHeading(this, float)
constexpr uintptr_t kUpdateRW               = 0x446F90;  // CEntity::UpdateRW(this)
constexpr uintptr_t kUpdateRwFrame          = 0x532B00;  // CEntity::UpdateRwFrame(this)
constexpr uintptr_t kWorldAdd               = 0x563220;  // CWorld::Add(entity)
constexpr uintptr_t kFindGroundZ            = 0x5696C0;  // CWorld::FindGroundZFor3DCoord
constexpr int kAutomobileSize               = 0x988;
constexpr uint8_t kCreatedByMission         = 2;
constexpr uint8_t kStatusAbandonedBits      = 0x20;      // (status << 3) at +0x36
constexpr size_t kVtableSetIsStatic         = 4;
constexpr size_t kVtableBaseHeight          = 53;        // CAutomobile::GetHeightAboveRoad

// Player position: CWorld::Players[0].m_pPed -> matrix -> pos
constexpr uintptr_t kPlayersPed             = 0xB7CD98;

constexpr DWORD kSpawnWaitMs                = 15000;     // F9: give up waiting for the stream
constexpr DWORD kStallWarnMs                = 10000;     // request still pending -> log once

using HasModelLoadedFn = bool (__cdecl*)(int);

// ═══════════════════════════════════════════════════════════════════════
// Logging
// ═══════════════════════════════════════════════════════════════════════

static char g_logPath[MAX_PATH] = {};

static const char* GetLogPath() {
    if (g_logPath[0]) return g_logPath;
    DWORD n = GetModuleFileNameA(nullptr, g_logPath, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    char* slash = strrchr(g_logPath, '\\');
    if (!slash) return nullptr;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - g_logPath), "SKR\\op215.log");
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

// ═══════════════════════════════════════════════════════════════════════
// Vehicle Registry
// ═══════════════════════════════════════════════════════════════════════

static constexpr int kMaxCustomVehicles = 8;

// Metadata donor is Sultan (560) for every model, as certified in EXP-001 D5.
static const VehicleDescriptor kDescriptors[] = {
    { 612, "skr_xc60",  "Volvo XC60 2009", 560 },
    { 613, "skr_m4gts", "BMW M4 GTS",      560 },
    { 614, "skr_rs6c8", "Audi RS6",        560 },  // reserved: maps only once its asset is enabled
};
static constexpr int kNumDescriptors = sizeof(kDescriptors) / sizeof(kDescriptors[0]);

struct VehicleSlot {
    const VehicleDescriptor* desc = nullptr;
    uint8_t lastLoadState = 0;
    DWORD   requestedAt   = 0;
    bool    stallLogged   = false;
};

static VehicleSlot g_slots[kMaxCustomVehicles] = {};
static int  g_numSlots    = 0;
static bool g_initialized = false;

// F9 spawn state
static int   g_spawnIndex   = 0;   // cycles through registered vehicles
static int   g_pendingSpawn = 0;   // model waiting for its stream, 0 = none
static DWORD g_pendingSince = 0;
static bool  g_f9WasDown    = false;
static bool  g_f8WasDown    = false;
static bool  g_showDiag     = false;

// ═══════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════

static uint8_t LoadState(int streamId) {
    return *(reinterpret_cast<const uint8_t*>(kStreamingInfoArray) + streamId * kStreamingInfoSize + kSI_LoadState);
}

static VehicleSlot* FindSlot(int modelId) {
    if (!g_initialized) return nullptr;
    for (int i = 0; i < g_numSlots; ++i)
        if (g_slots[i].desc && g_slots[i].desc->modelId == modelId) return &g_slots[i];
    return nullptr;
}

static bool ReadPlayerPos(float& x, float& y, float& z, float& heading) {
    __try {
        auto* ped = *reinterpret_cast<uint8_t**>(kPlayersPed);
        if (!ped) return false;
        auto* matrix = *reinterpret_cast<float**>(ped + 0x14);
        if (matrix) {
            x = matrix[12]; y = matrix[13]; z = matrix[14];
            float fx = matrix[4], fy = matrix[5];
            heading = std::atan2(-fx, fy);
        } else {
            auto* p = reinterpret_cast<float*>(ped + 0x4);
            x = p[0]; y = p[1]; z = p[2];
            heading = p[3];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Pool-allocated CAutomobile, placed on the ground and added to the world
// (EXP-001 NativeConstruct + NativePlace). Model must already be loaded.
static DWORD NativeSpawn(int modelId, float x, float y, float z, float heading, uint8_t** out) {
    *out = nullptr;
    __try {
        bool found = false;
        const float ground = reinterpret_cast<float (__cdecl*)(float, float, float, bool*, void**)>(
            kFindGroundZ)(x, y, z + 3.0f, &found, nullptr);
        void* memory = reinterpret_cast<void* (__cdecl*)(unsigned)>(kVehicleNew)(kAutomobileSize);
        if (!memory) return 0xE0000001;
        auto* v = static_cast<uint8_t*>(reinterpret_cast<void* (__thiscall*)(void*, int, uint8_t, bool)>(
            kAutomobileCtor)(memory, modelId, kCreatedByMission, true));
        const auto vtable = *reinterpret_cast<uintptr_t* const*>(v);
        const float base = reinterpret_cast<float (__thiscall*)(void*)>(vtable[kVtableBaseHeight])(v);
        const float finalZ = (found ? ground : z) + base + 0.1f;
        reinterpret_cast<void (__thiscall*)(void*, float)>(kSetHeading)(v, heading);
        auto* matrix = *reinterpret_cast<uint8_t**>(v + 0x14);
        if (matrix) {
            memcpy(matrix + 0x30, &x, 4); memcpy(matrix + 0x34, &y, 4); memcpy(matrix + 0x38, &finalZ, 4);
        } else {
            memcpy(v + 0x04, &x, 4); memcpy(v + 0x08, &y, 4); memcpy(v + 0x0C, &finalZ, 4);
        }
        v[0x36] = static_cast<uint8_t>((v[0x36] & 0x07) | kStatusAbandonedBits);
        reinterpret_cast<void (__thiscall*)(void*, bool)>(vtable[kVtableSetIsStatic])(v, false);
        reinterpret_cast<void (__thiscall*)(void*)>(kUpdateRW)(v);
        reinterpret_cast<void (__thiscall*)(void*)>(kUpdateRwFrame)(v);
        reinterpret_cast<void (__cdecl*)(void*)>(kWorldAdd)(v);
        *out = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return 0;
}

static bool DoSpawn(int modelId) {
    if (!IsLoaded(modelId)) {
        Log("DoSpawn(%d) REJECTED: model not loaded", modelId);
        return false;
    }
    float px, py, pz, heading;
    if (!ReadPlayerPos(px, py, pz, heading)) {
        Log("DoSpawn(%d) FAILED: cannot read player position", modelId);
        return false;
    }
    // 5 units in front of the player.
    const float x = px - 5.0f * sinf(heading);
    const float y = py + 5.0f * cosf(heading);
    uint8_t* vehicle = nullptr;
    const DWORD exception = NativeSpawn(modelId, x, y, pz, heading, &vehicle);
    Log("DoSpawn(%d) %s vehicle=%p exception=0x%08lX at (%.1f, %.1f, %.1f)", modelId,
        vehicle ? "OK" : "FAILED", vehicle, exception, x, y, pz);
    return vehicle != nullptr;
}

// Gate D evidence: every CStreaming load-state transition of a mapped model.
static void TrackLoadStates() {
    for (int i = 0; i < g_numSlots; ++i) {
        VehicleSlot& slot = g_slots[i];
        const int id = slot.desc->modelId;
        const uint8_t state = LoadState(id);
        if (state != slot.lastLoadState) {
            const int txd = registration::GetTxdStreamId(id);
            Log("stream model=%d loadState %u -> %u txdState=%u elapsed=%lums", id, slot.lastLoadState, state,
                txd >= 0 ? LoadState(txd) : 0xFF, slot.requestedAt ? GetTickCount() - slot.requestedAt : 0);
            slot.lastLoadState = state;
        }
        if (state != 0 && state != 1 && slot.requestedAt && !slot.stallLogged &&
            GetTickCount() - slot.requestedAt > kStallWarnMs) {
            slot.stallLogged = true;
            Log("stream model=%d still loadState=%u after %lums (no wait on main thread)", id, state,
                GetTickCount() - slot.requestedAt);
        }
    }
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════

int Initialize() {
    if (g_initialized) return g_numSlots;

    const registration::Stage stage = registration::GetStage();
    const size_t intact = registration::RecheckMappings();
    Log("=== OP215 Initialize === tid=%lu stage=%s intact=%u", GetCurrentThreadId(),
        registration::StageName(stage), static_cast<unsigned>(intact));

    g_numSlots = 0;
    for (int i = 0; i < kNumDescriptors && g_numSlots < kMaxCustomVehicles; ++i) {
        const auto* desc = &kDescriptors[i];
        const registration::Resource resource = registration::GetResource(desc->modelId);
        const bool mapped = registration::IsMapped(desc->modelId);
        Log("model %d '%s' resource=%s mapped=%u%s", desc->modelId, desc->displayName,
            registration::ResourceName(resource), mapped, mapped ? "" : " -> proxy fallback");
        if (!mapped) continue;
        g_slots[g_numSlots] = {};
        g_slots[g_numSlots].desc = desc;
        g_slots[g_numSlots].lastLoadState = LoadState(desc->modelId);
        ++g_numSlots;
    }

    g_initialized = (g_numSlots > 0);
    Log("Initialize complete: %d vehicles mapped via SAMP\\CUSTOM.IMG", g_numSlots);
    return g_numSlots;
}

void Shutdown() {
    // Descriptors live in the relocated GTA store and streaming entries belong
    // to CStreaming: nothing to free here, and no engine calls off-thread.
    if (!g_initialized) return;
    Log("=== OP215 Shutdown ===");
    g_numSlots = 0;
    g_pendingSpawn = 0;
    g_initialized = false;
}

bool IsInitialized() { return g_initialized; }

bool IsRegistered(int modelId) {
    return FindSlot(modelId) != nullptr && registration::IsMapped(modelId);
}

void RequestLoad(int modelId) {
    VehicleSlot* slot = FindSlot(modelId);
    if (!slot || !registration::IsMapped(modelId)) {
        Log("RequestLoad(%d) REJECTED: model not registered in OP215", modelId);
        return;
    }
    if (registration::GetResource(modelId) != registration::Resource::Registered) return;  // in flight or ready
    if (!slot->requestedAt) slot->requestedAt = GetTickCount();
    slot->stallLogged = false;
    registration::Request(modelId);  // issued by registration::Tick, one custom model at a time
}

bool IsLoaded(int modelId) {
    if (!IsRegistered(modelId) || !registration::IsReady(modelId)) return false;
    auto hasLoaded = reinterpret_cast<HasModelLoadedFn>(kHasModelLoaded);
    return hasLoaded(modelId);
}

bool SpawnAtPlayer(int modelId) {
    if (!IsRegistered(modelId)) return false;
    if (IsLoaded(modelId)) return DoSpawn(modelId);
    RequestLoad(modelId);
    g_pendingSpawn = modelId;
    g_pendingSince = GetTickCount();
    Log("SpawnAtPlayer(%d) deferred until streamed", modelId);
    return false;
}

DiagInfo GetDiag(int modelId) {
    DiagInfo info = { modelId, false, false, false, registration::GetStage() == registration::Stage::Mapped };
    if (IsRegistered(modelId)) {
        info.registered  = true;
        info.txdLoaded   = registration::GetTxdStreamId(modelId) >= 0;
        info.modelLoaded = IsLoaded(modelId);
    }
    return info;
}

int GetRegisteredCount() { return g_numSlots; }

const VehicleDescriptor* GetDescriptor(int index) {
    if (index < 0 || index >= g_numSlots) return nullptr;
    return g_slots[index].desc;
}

void Tick() {
    if (!g_initialized || g_numSlots == 0) return;

    registration::Tick();
    TrackLoadStates();

    if (g_pendingSpawn) {
        if (IsLoaded(g_pendingSpawn)) {
            const int model = g_pendingSpawn;
            g_pendingSpawn = 0;
            DoSpawn(model);
        } else if (GetTickCount() - g_pendingSince > kSpawnWaitMs) {
            Log("F9 spawn model=%d abandoned: not streamed after %lums", g_pendingSpawn, kSpawnWaitMs);
            g_pendingSpawn = 0;
        }
    }

    // F9: cycle spawn through registered vehicles.
    bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9Down && !g_f9WasDown && !g_pendingSpawn) {
        int idx = g_spawnIndex % g_numSlots;
        auto* desc = g_slots[idx].desc;
        if (desc) {
            Log("F9 spawn: modelId=%d '%s'", desc->modelId, desc->displayName);
            SpawnAtPlayer(desc->modelId);
        }
        g_spawnIndex = (g_spawnIndex + 1) % g_numSlots;
    }
    g_f9WasDown = f9Down;

    // F8: toggle diagnostics logging.
    bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8Down && !g_f8WasDown) {
        g_showDiag = !g_showDiag;
        for (int i = 0; i < g_numSlots; ++i) {
            const int id = g_slots[i].desc->modelId;
            Log("F8 diag model=%d loadState=%u loaded=%u", id, LoadState(id), IsLoaded(id));
        }
    }
    g_f8WasDown = f8Down;
}

} // namespace skr::vehicle
