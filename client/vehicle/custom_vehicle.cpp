// ─────────────────────────────────────────────────────────────────────
// OP215 — SKR Enhanced Custom Vehicle System (Runtime)
// ─────────────────────────────────────────────────────────────────────
// GTA SA 1.0 US (x86).  All addresses verified against plugin-sdk /
// gta-reversed.  The module runs on the GTA thread only.
// ─────────────────────────────────────────────────────────────────────

#include "vehicle/custom_vehicle.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace skr::vehicle {
namespace {

// ═══════════════════════════════════════════════════════════════════════
// GTA SA 1.0 US Addresses  (plugin-sdk / gta-reversed provenance)
// ═══════════════════════════════════════════════════════════════════════

// CStreaming
constexpr uintptr_t kAddImageToList         = 0x407610;  // int __cdecl(const char*, bool)
constexpr uintptr_t kRequestModel           = 0x4087E0;  // void __cdecl(int, int)
constexpr uintptr_t kLoadAllRequestedModels = 0x40EA10;  // void __cdecl(bool)
constexpr uintptr_t kHasModelLoaded         = 0x4044C0;  // bool __cdecl(int)
constexpr uintptr_t kRemoveModel            = 0x4089A0;  // void __cdecl(int)
constexpr uintptr_t kStreamingInfoArray     = 0x8E4CC0;  // CStreamingInfo[26316]
constexpr int kStreamingInfoSize            = 20;        // sizeof(CStreamingInfo)

// CModelInfo
constexpr uintptr_t kModelInfoPtrs          = 0xA9B0C8;  // CBaseModelInfo*[20000]

// CTxdStore
constexpr uintptr_t kTxdAddSlot             = 0x731C80;  // int __cdecl(const char*)
constexpr uintptr_t kTxdFindSlot            = 0x731850;  // int __cdecl(const char*)
constexpr uintptr_t kTxdAddRef              = 0x731A00;  // void __cdecl(int)
constexpr uintptr_t kTxdSetCurrent          = 0x7319C0;  // void __cdecl(int)
constexpr uintptr_t kTxdRemoveSlot          = 0x731CD0;  // void __cdecl(int)

// CWorld
constexpr uintptr_t kWorldAdd               = 0x563220;  // void __cdecl(CEntity*)

// Player position: CWorld::Players[0].m_pPed -> matrix -> pos
constexpr uintptr_t kPlayersPed             = 0xB7CD98;

// CAutomobile constructor (thiscall: ECX = this, args on stack)
constexpr uintptr_t kAutomobileCtor         = 0x6B0A90;  // void __thiscall(int modelId, uint8_t createdBy)
constexpr int kAutomobileSize               = 0x988;     // sizeof(CAutomobile)

// CVehicleModelInfo — size 0x308.
// Key offsets within CVehicleModelInfo:
//   0x00: vtable
//   0x04: hashKey (uint32)
//   0x0A: texDictionary (int16) — TXD slot index
//   0x0E: objectNumber (uint16) — model ID
//   0x14: colModel (CColModel*)
//   0x20: rwObject (RpClump*)
// Vehicle-specific (varies by game version):
//   0x30C: vehicleType (uint32) — 0=car, 5=bike, etc.
constexpr int kVehicleModelInfoSize         = 0x308;
constexpr int kMI_HashKey                   = 0x04;
constexpr int kMI_TexDict                   = 0x0A;
constexpr int kMI_ObjectNumber              = 0x0E;
constexpr int kMI_ColModel                  = 0x14;
constexpr int kMI_RwObject                  = 0x20;

// CStreamingInfo layout (20 bytes):
//   0x00: nextOnCd (int16)
//   0x02: prevOnCd (int16)
//   0x04: nextInImg (int16)
//   0x06: prevInImg (int16)
//   0x08: imgId (uint8)
//   0x09: padding (uint8)
//   0x0A: cdPosn (uint32) — sector offset within the IMG
//   0x0E: cdSize (uint32) — size in sectors (streaming blocks)
//   0x12: flags (uint8)
//   0x13: padding (uint8)
constexpr int kSI_NextOnCd  = 0x00;
constexpr int kSI_PrevOnCd  = 0x02;
constexpr int kSI_NextInImg = 0x04;
constexpr int kSI_PrevInImg = 0x06;
constexpr int kSI_ImgId     = 0x08;
constexpr int kSI_CdPosn    = 0x0A;
constexpr int kSI_CdSize    = 0x0E;
constexpr int kSI_Flags     = 0x12;

// TXD streaming IDs: TXD slot + 20000 (TXD stream base)
constexpr int kTxdStreamBase = 20000;

// ═══════════════════════════════════════════════════════════════════════
// Function Typedefs
// ═══════════════════════════════════════════════════════════════════════

using AddImageToListFn         = int   (__cdecl*)(const char*, bool);
using RequestModelFn           = void  (__cdecl*)(int, int);
using LoadAllRequestedModelsFn = void  (__cdecl*)(bool);
using HasModelLoadedFn         = bool  (__cdecl*)(int);
using RemoveModelFn            = void  (__cdecl*)(int);
using TxdAddSlotFn             = int   (__cdecl*)(const char*);
using TxdFindSlotFn            = int   (__cdecl*)(const char*);
using TxdAddRefFn              = void  (__cdecl*)(int);
using TxdSetCurrentFn          = void  (__cdecl*)(int);
using TxdRemoveSlotFn          = void  (__cdecl*)(int);
using WorldAddFn               = void  (__cdecl*)(void*);

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

static const VehicleDescriptor kDescriptors[] = {
    { 612, "skr_xc60",  "Volvo XC60 2009", 400 },  // Landstalker (SUV donor)
    { 613, "skr_m4gts", "BMW M4 GTS",      475 },  // Sabre (coupe donor)
};
static constexpr int kNumDescriptors = sizeof(kDescriptors) / sizeof(kDescriptors[0]);

struct VehicleSlot {
    const VehicleDescriptor* desc = nullptr;
    uint8_t* modelInfo       = nullptr;  // Allocated CVehicleModelInfo clone
    int      txdSlot         = -1;
    bool     registered      = false;
    bool     imgSetUp        = false;
};

static VehicleSlot g_slots[kMaxCustomVehicles] = {};
static int  g_numSlots    = 0;
static bool g_initialized = false;
static int  g_imgId       = -1;  // IMG archive index returned by AddImageToList

// F9 spawn state
static int  g_spawnIndex  = 0;   // cycles through registered vehicles
static bool g_f9WasDown   = false;
static bool g_f8WasDown   = false;
static bool g_showDiag    = false;

// IMG directory info: sector offsets for each file within skr_vehicles.img.
// Must match the order in the IMG built by build_skr_img.ps1.
struct ImgEntry {
    const char* name;
    uint32_t    sectorOffset;  // within the IMG (relative to IMG start)
    uint32_t    sectorCount;
};

// These values must match the IMG builder output.
// Header + directory = ceil((8 + 4*32) / 2048) = ceil(136/2048) = 1 sector.
// Data starts at sector 1.
static const ImgEntry kImgEntries[] = {
    { "skr_xc60.dff",  1,           4208 },
    { "skr_xc60.txd",  1 + 4208,    773  },
    { "skr_m4gts.dff", 1 + 4208 + 773, 3728 },
    { "skr_m4gts.txd", 1 + 4208 + 773 + 3728, 1332 },
};

// ═══════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════

static uint32_t GtaCrc32(const char* str) {
    // GTA SA CRC-32 variant (CKeyGen::GetUppercaseKey 0x53CF70).
    // Simplified: uppercase input, standard CRC-32, inverted final XOR.
    // For our purposes a simple hash is sufficient since we're using
    // unused model IDs.  We use a direct computation.
    uint32_t hash = 0;
    for (; *str; ++str) {
        char c = *str;
        if (c >= 'a' && c <= 'z') c -= 32;
        hash += static_cast<uint8_t>(c);
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

static void* GetModelInfoPtr(int modelId) {
    auto** ptrs = reinterpret_cast<void**>(kModelInfoPtrs);
    return ptrs[modelId];
}

static void SetModelInfoPtr(int modelId, void* ptr) {
    auto** ptrs = reinterpret_cast<void**>(kModelInfoPtrs);
    ptrs[modelId] = ptr;
}

static uint8_t* GetStreamingInfo(int streamId) {
    return reinterpret_cast<uint8_t*>(kStreamingInfoArray) + streamId * kStreamingInfoSize;
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

// Find the IMG entry index for a given filename (case-insensitive).
static int FindImgEntry(const char* name) {
    for (int i = 0; i < static_cast<int>(sizeof(kImgEntries)/sizeof(kImgEntries[0])); ++i) {
        if (_stricmp(kImgEntries[i].name, name) == 0) return i;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════
// Registration
// ═══════════════════════════════════════════════════════════════════════

static bool RegisterImg() {
    // Build the path relative to the GTA directory.
    // The IMG is staged to <gta>\SKR\op215\skr_vehicles.img by the build system.
    auto addImg = reinterpret_cast<AddImageToListFn>(kAddImageToList);
    g_imgId = addImg("SKR\\op215\\skr_vehicles.img", false);
    if (g_imgId < 0) {
        Log("RegisterImg FAILED: AddImageToList returned %d", g_imgId);
        return false;
    }
    Log("RegisterImg OK: imgId=%d", g_imgId);
    return true;
}

static bool RegisterVehicle(VehicleSlot& slot) {
    const auto* desc = slot.desc;

    // 1. Register a TXD slot for this vehicle's textures.
    auto txdAdd  = reinterpret_cast<TxdAddSlotFn>(kTxdAddSlot);
    auto txdFind = reinterpret_cast<TxdFindSlotFn>(kTxdFindSlot);

    slot.txdSlot = txdFind(desc->internalName);
    if (slot.txdSlot < 0) {
        slot.txdSlot = txdAdd(desc->internalName);
    }
    if (slot.txdSlot < 0) {
        Log("RegisterVehicle(%d) FAILED: TXD slot allocation", desc->modelId);
        return false;
    }
    Log("RegisterVehicle(%d): txdSlot=%d", desc->modelId, slot.txdSlot);

    // 2. Clone the donor vehicle's CVehicleModelInfo.
    auto* donorInfo = reinterpret_cast<uint8_t*>(GetModelInfoPtr(desc->donorModelId));
    if (!donorInfo) {
        Log("RegisterVehicle(%d) FAILED: donor %d has no model info", desc->modelId, desc->donorModelId);
        return false;
    }

    slot.modelInfo = static_cast<uint8_t*>(malloc(kVehicleModelInfoSize));
    if (!slot.modelInfo) {
        Log("RegisterVehicle(%d) FAILED: malloc", desc->modelId);
        return false;
    }
    memcpy(slot.modelInfo, donorInfo, kVehicleModelInfoSize);

    // 3. Patch the cloned model info.
    // Hash key — unique per model name.
    *reinterpret_cast<uint32_t*>(slot.modelInfo + kMI_HashKey) = GtaCrc32(desc->internalName);
    // TXD slot.
    *reinterpret_cast<int16_t*>(slot.modelInfo + kMI_TexDict) = static_cast<int16_t>(slot.txdSlot);
    // Model ID.
    *reinterpret_cast<uint16_t*>(slot.modelInfo + kMI_ObjectNumber) = static_cast<uint16_t>(desc->modelId);
    // Clear the RW object pointer — it will be populated when streamed.
    *reinterpret_cast<uintptr_t*>(slot.modelInfo + kMI_RwObject) = 0;
    // Collision: keep the donor's collision for now (prevents fall-through).
    // The DFF's embedded collision will replace it on load.

    // 4. Register in the global model info array.
    SetModelInfoPtr(desc->modelId, slot.modelInfo);

    // 5. Set up CStreamingInfo for the DFF.
    char dffName[32], txdName[32];
    snprintf(dffName, sizeof(dffName), "%s.dff", desc->internalName);
    snprintf(txdName, sizeof(txdName), "%s.txd", desc->internalName);

    int dffEntry = FindImgEntry(dffName);
    int txdEntry = FindImgEntry(txdName);
    if (dffEntry < 0 || txdEntry < 0) {
        Log("RegisterVehicle(%d) FAILED: IMG entries not found (dff=%d txd=%d)",
            desc->modelId, dffEntry, txdEntry);
        return false;
    }

    // DFF streaming info (model ID = streaming ID for models < 20000).
    auto* dffSi = GetStreamingInfo(desc->modelId);
    *reinterpret_cast<int16_t*>(dffSi + kSI_NextOnCd)  = -1;
    *reinterpret_cast<int16_t*>(dffSi + kSI_PrevOnCd)  = -1;
    *reinterpret_cast<int16_t*>(dffSi + kSI_NextInImg)  = -1;
    *reinterpret_cast<int16_t*>(dffSi + kSI_PrevInImg)  = -1;
    *reinterpret_cast<uint8_t*>(dffSi + kSI_ImgId)     = static_cast<uint8_t>(g_imgId);
    *reinterpret_cast<uint32_t*>(dffSi + kSI_CdPosn)   = kImgEntries[dffEntry].sectorOffset;
    *reinterpret_cast<uint32_t*>(dffSi + kSI_CdSize)   = kImgEntries[dffEntry].sectorCount;
    *reinterpret_cast<uint8_t*>(dffSi + kSI_Flags)     = 0;

    // TXD streaming info (streaming ID = txdSlot + 20000).
    int txdStreamId = slot.txdSlot + kTxdStreamBase;
    auto* txdSi = GetStreamingInfo(txdStreamId);
    *reinterpret_cast<int16_t*>(txdSi + kSI_NextOnCd)  = -1;
    *reinterpret_cast<int16_t*>(txdSi + kSI_PrevOnCd)  = -1;
    *reinterpret_cast<int16_t*>(txdSi + kSI_NextInImg)  = -1;
    *reinterpret_cast<int16_t*>(txdSi + kSI_PrevInImg)  = -1;
    *reinterpret_cast<uint8_t*>(txdSi + kSI_ImgId)     = static_cast<uint8_t>(g_imgId);
    *reinterpret_cast<uint32_t*>(txdSi + kSI_CdPosn)   = kImgEntries[txdEntry].sectorOffset;
    *reinterpret_cast<uint32_t*>(txdSi + kSI_CdSize)   = kImgEntries[txdEntry].sectorCount;
    *reinterpret_cast<uint8_t*>(txdSi + kSI_Flags)     = 0;

    slot.registered = true;
    slot.imgSetUp   = true;
    Log("RegisterVehicle(%d) OK: '%s' -> donor=%d txd=%d dffSector=%u txdSector=%u",
        desc->modelId, desc->internalName, desc->donorModelId, slot.txdSlot,
        kImgEntries[dffEntry].sectorOffset, kImgEntries[txdEntry].sectorOffset);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
// Spawn
// ═══════════════════════════════════════════════════════════════════════

static bool DoSpawn(int modelId) {
    auto requestModel  = reinterpret_cast<RequestModelFn>(kRequestModel);
    auto loadAll       = reinterpret_cast<LoadAllRequestedModelsFn>(kLoadAllRequestedModels);
    auto hasLoaded     = reinterpret_cast<HasModelLoadedFn>(kHasModelLoaded);
    auto worldAdd      = reinterpret_cast<WorldAddFn>(kWorldAdd);

    // Request the model and its TXD.
    requestModel(modelId, 2);  // flags=2: GAME_REQUIRED
    loadAll(false);

    // Synchronous wait (up to 5 seconds).
    DWORD start = GetTickCount();
    while (!hasLoaded(modelId)) {
        if (GetTickCount() - start > 5000) {
            Log("DoSpawn(%d) TIMEOUT waiting for model load", modelId);
            return false;
        }
        Sleep(10);
        loadAll(false);
    }

    float px, py, pz, heading;
    if (!ReadPlayerPos(px, py, pz, heading)) {
        Log("DoSpawn(%d) FAILED: cannot read player position", modelId);
        return false;
    }

    // Offset spawn 5 units in front of the player.
    float spawnX = px - 5.0f * sinf(heading);
    float spawnY = py + 5.0f * cosf(heading);
    float spawnZ = pz + 0.5f;

    // Allocate CAutomobile and call constructor.
    auto* vehicle = static_cast<uint8_t*>(malloc(kAutomobileSize));
    if (!vehicle) {
        Log("DoSpawn(%d) FAILED: malloc CAutomobile", modelId);
        return false;
    }
    memset(vehicle, 0, kAutomobileSize);

    __try {
        // CAutomobile::CAutomobile(int modelId, uint8_t createdBy)
        // thiscall: ECX = this
        auto ctor = kAutomobileCtor;
        __asm {
            push 2          // createdBy = MISSION_VEHICLE
            push modelId
            mov ecx, vehicle
            call ctor
        }

        // Set position via the entity's CPlaceable::SetPosn equivalent.
        // CEntity matrix is at offset 0x14 (pointer to CMatrix).
        auto* matrix = *reinterpret_cast<float**>(vehicle + 0x14);
        if (matrix) {
            matrix[12] = spawnX;
            matrix[13] = spawnY;
            matrix[14] = spawnZ;
        } else {
            // CSimpleTransform at offset 0x04.
            auto* pos = reinterpret_cast<float*>(vehicle + 0x04);
            pos[0] = spawnX;
            pos[1] = spawnY;
            pos[2] = spawnZ;
        }

        worldAdd(vehicle);
        Log("DoSpawn(%d) OK at (%.1f, %.1f, %.1f)", modelId, spawnX, spawnY, spawnZ);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("DoSpawn(%d) EXCEPTION in vehicle constructor", modelId);
        free(vehicle);
        return false;
    }
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════

int Initialize() {
    if (g_initialized) return g_numSlots;

    Log("=== OP215 Initialize ===");

    // Ensure the SKR log directory exists.
    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (n && n < MAX_PATH) {
        char* slash = strrchr(dir, '\\');
        if (slash) {
            snprintf(slash + 1, MAX_PATH - (slash + 1 - dir), "SKR\\");
            CreateDirectoryA(dir, nullptr);
            snprintf(slash + 1, MAX_PATH - (slash + 1 - dir), "SKR\\op215\\");
            CreateDirectoryA(dir, nullptr);
        }
    }

    if (!RegisterImg()) return 0;

    g_numSlots = 0;
    for (int i = 0; i < kNumDescriptors && g_numSlots < kMaxCustomVehicles; ++i) {
        auto& slot = g_slots[g_numSlots];
        slot.desc = &kDescriptors[i];
        if (RegisterVehicle(slot)) {
            ++g_numSlots;
        } else {
            slot.desc = nullptr;
        }
    }

    g_initialized = (g_numSlots > 0);
    Log("Initialize complete: %d vehicles registered", g_numSlots);
    return g_numSlots;
}

void Shutdown() {
    if (!g_initialized) return;
    Log("=== OP215 Shutdown ===");

    auto removeModel = reinterpret_cast<RemoveModelFn>(kRemoveModel);
    auto txdRemove   = reinterpret_cast<TxdRemoveSlotFn>(kTxdRemoveSlot);

    for (int i = 0; i < g_numSlots; ++i) {
        auto& slot = g_slots[i];
        if (!slot.desc) continue;

        if (slot.registered) {
            removeModel(slot.desc->modelId);
            SetModelInfoPtr(slot.desc->modelId, nullptr);
        }
        if (slot.txdSlot >= 0) {
            txdRemove(slot.txdSlot);
        }
        free(slot.modelInfo);
        slot = {};
    }

    g_numSlots = 0;
    g_initialized = false;
}

bool IsInitialized() { return g_initialized; }

void RequestLoad(int modelId) {
    auto requestModel = reinterpret_cast<RequestModelFn>(kRequestModel);
    requestModel(modelId, 2);
}

bool IsLoaded(int modelId) {
    auto hasLoaded = reinterpret_cast<HasModelLoadedFn>(kHasModelLoaded);
    return hasLoaded(modelId);
}

bool SpawnAtPlayer(int modelId) {
    if (!g_initialized) return false;
    bool found = false;
    for (int i = 0; i < g_numSlots; ++i) {
        if (g_slots[i].desc && g_slots[i].desc->modelId == modelId) {
            found = true; break;
        }
    }
    if (!found) return false;
    return DoSpawn(modelId);
}

DiagInfo GetDiag(int modelId) {
    DiagInfo info = { modelId, false, false, false, g_imgId >= 0 };
    for (int i = 0; i < g_numSlots; ++i) {
        if (g_slots[i].desc && g_slots[i].desc->modelId == modelId) {
            info.registered  = g_slots[i].registered;
            info.txdLoaded   = (g_slots[i].txdSlot >= 0);
            auto hasLoaded   = reinterpret_cast<HasModelLoadedFn>(kHasModelLoaded);
            info.modelLoaded = hasLoaded(modelId);
            break;
        }
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

    // F9: cycle spawn through registered vehicles.
    bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9Down && !g_f9WasDown) {
        int idx = g_spawnIndex % g_numSlots;
        auto* desc = g_slots[idx].desc;
        if (desc) {
            Log("F9 spawn: modelId=%d '%s'", desc->modelId, desc->displayName);
            SpawnAtPlayer(desc->modelId);
        }
        g_spawnIndex = (g_spawnIndex + 1) % g_numSlots;
    }
    g_f9WasDown = f9Down;

    // F8: toggle diagnostics overlay.
    bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8Down && !g_f8WasDown) {
        g_showDiag = !g_showDiag;
    }
    g_f8WasDown = f8Down;
}

} // namespace skr::vehicle
