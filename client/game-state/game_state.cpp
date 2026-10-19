#include "game-state/game_state.h"
#include "protocol/client_state.h"
#include "protocol/session_info.h"
#include "lobby/lobby_manager.h"
#include "escape-menu/escape_menu.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Provenance:
//  GTA SA 1.0 US addresses: DK22Pac/plugin-sdk (plugin_sa/game_sa) —
//    CWorld::Players 0xB7CD98 (CPlayerInfo::m_pPed @0x0), CPlaceable::m_matrix @0x14,
//    CMatrix::pos @0x30, CTheZones::ZoneInfoArray 0xBA3798, TotalNumberOfInfoZones 0xBA3794,
//    CTheZones::PointLiesWithinZone 0x572270 (cdecl), CZone size 0x20,
//    CZone::m_szTextKey @0x8, m_fX1..m_fZ2 shorts @0x10, TheText 0xC1B340, CText::Get 0x6A0050 (thiscall).
//  samp.dll 0.3.7-R5 (sha256 b72b5dbe...2553a2): state is sent as SKR RPC 240 (see SendClientState)
//    using the BitStream/RakClient calls of the command sender at RVA 0x69900.
//    RVA 0x26EB94 = CNetGame*, state byte @+0x3CD (5 = connected, as used in d3d9_hook).

namespace skr::game_state {

namespace {

struct CVector { float x, y, z; };

constexpr uintptr_t kPlayersPed          = 0xB7CD98;
constexpr uintptr_t kZoneInfoArray       = 0xBA3798;
constexpr uintptr_t kTotalInfoZones      = 0xBA3794;
constexpr uintptr_t kPointLiesWithinZone = 0x572270;
constexpr uintptr_t kTheText             = 0xC1B340;
constexpr uintptr_t kCTextGet            = 0x6A0050;
constexpr size_t    kZoneSize            = 0x20;

constexpr uintptr_t kSampNetGameRva  = 0x26EB94;
constexpr uintptr_t kNetGameStateOff = 0x3CD;
constexpr uint8_t   kStateConnected  = 5;

// plugin-sdk CRadar.cpp: cachedCos 0xBA8308, cachedSin 0xBA830C.
constexpr uintptr_t kRadarCachedCos = 0xBA8308;
constexpr uintptr_t kRadarCachedSin = 0xBA830C;
// CRadar::CalculateCachedSinCos 0x583670 (cdecl, camera only, draws nothing). gta-reversed
// Radar.cpp: CRadar::DrawMap (0x586B00) calls it and sets the range; with the native
// radar hidden DrawMap does not run, so we call it ourselves.
constexpr uintptr_t kCalcCachedSinCos = 0x583670;
// DrawMap ground/foot range: speed = |FindPlayerSpeed|; <0.3 -> 180, >0.9 -> 350,
// else (speed - 0.3) * (850 / (0.3 * 10)) + 180. Plane and script-zoom branches omitted.
constexpr float kRadarMinRange = 180.0f, kRadarMaxRange = 350.0f;
constexpr float kRadarMinSpeed = 0.3f, kRadarMaxSpeed = 0.9f;

// plugin-sdk common.cpp: FindPlayerSpeed 0x56E090, FindPlayerVehicle 0x56E0D0 (cdecl).
constexpr uintptr_t kFindPlayerSpeed   = 0x56E090;
constexpr uintptr_t kFindPlayerVehicle = 0x56E0D0;

// CRadar::StreamRadarSections(CVector const&) 0x5858D0 — requests tiles near the
// player from the streaming system.  Must run during CGame::Process (the streaming
// update phase), not EndScene, for tiles to actually load.
// Provenance: plugin-sdk CRadar.cpp.
constexpr uintptr_t kStreamRadarSections = 0x5858D0;
// CRadar::DrawMap (static, cdecl, no params) — draws the full native radar:
// mask, tile sections, water, blips.  Provenance: plugin-sdk CRadar.cpp / gta-reversed.
constexpr uintptr_t kCRadarDrawMap = 0x586B00;
// CHud::Draw (static, cdecl, no params) — 2D HUD entry point called from Idle's
// render pass.  Provenance: plugin-sdk CHud.cpp / gta-reversed.
constexpr uintptr_t kCHudDraw = 0x58FAE0;
// gRadarTxdIds[12*12] at 0xBA8478 — TXD slot IDs for each radar tile (-1 = unregistered).
constexpr uintptr_t kGRadarTxdIds = 0xBA8478;
// plugin-sdk CVehicle.h: m_nCurrentGear @0x4B4; CPhysical m_vecMoveSpeed @0x44.
constexpr uintptr_t kVehCurrentGear = 0x4B4;
// plugin-sdk: CVehicle::m_pHandlingData @0x384; tHandlingData::m_transmissionData @0x2C;
// cTransmission::m_fMaxGearVelocity @0x58 -> handling + 0x84. Assumed stored in the same
// per-frame units as move speed (x180 = km/h); logged once to validate against handling.cfg
// (INFERNUS fMaxVelocity = 240.0).
constexpr uintptr_t kVehHandling        = 0x384;
constexpr uintptr_t kHandlingMaxGearVel = 0x2C + 0x58;
constexpr uintptr_t kPhysMoveSpeed  = 0x44;
// plugin-sdk CTheScripts.cpp: bDisplayHud 0xA444A0 (script DISPLAY_HUD flag).
constexpr uintptr_t kDisplayHud = 0xA444A0;
// GTA speed units are per 1/50 s frame: km/h = |v| * 50 * 3.6.
constexpr float kSpeedToKmh = 180.0f;

constexpr DWORD kSendIntervalMs = 100;

using PointLiesWithinZoneFn = bool(__cdecl*)(const CVector*, void*);
using CTextGetFn            = const char*(__thiscall*)(void*, const char*);
using VoidFn                = void(__cdecl*)();
using FindPlayerSpeedFn     = const CVector&(__cdecl*)(int);
using FindPlayerVehicleFn   = uint8_t*(__cdecl*)(int, bool);

DWORD g_lastSend = 0;

// Diagnostic: logs each distinct stage once to <gta>\SKR\game_state.log.
uint32_t g_loggedStages = 0;
void LogStage(int stage, const char* msg) {
    if (g_loggedStages & (1u << stage)) return;
    g_loggedStages |= (1u << stage);
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "SKR\\game_state.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] stage %d: %s\n", GetTickCount(), stage, msg);
    fclose(f);
}

bool ReadPlayer(CVector& pos, float& heading) {
    auto* ped = *reinterpret_cast<uint8_t**>(kPlayersPed);
    if (!ped) return false;
    auto* matrix = *reinterpret_cast<float**>(ped + 0x14);
    if (matrix) {
        pos = {matrix[12], matrix[13], matrix[14]};          // pos @0x30
        float fx = matrix[4], fy = matrix[5];                // forward ("up") @0x10
        heading = std::atan2(-fx, fy) * 57.2957795f;
    } else {
        auto* p = reinterpret_cast<float*>(ped + 0x4);       // CSimpleTransform pos
        pos = {p[0], p[1], p[2]};
        heading = p[3] * 57.2957795f;                        // CSimpleTransform heading (rad)
    }
    if (heading < 0.0f) heading += 360.0f;
    return true;
}

const char* ZoneText(const uint8_t* zone) {
    auto get = reinterpret_cast<CTextGetFn>(kCTextGet);
    return get(reinterpret_cast<void*>(kTheText), reinterpret_cast<const char*>(zone + 0x8));
}

// Smallest containing navigation zone = area. City/county = containing zone with z2 == 900:
// in info.zon only LA, VE, SF, RED, BONE, ROBAD, FLINTC, WHET use that height; the
// state-wide "San Andreas" zone (not in info.zon) is excluded by this rule.
void ResolveZones(const CVector& pos, char* city, size_t cityLen, char* area, size_t areaLen) {
    city[0] = area[0] = '\0';
    short count = *reinterpret_cast<short*>(kTotalInfoZones);
    if (count <= 0 || count > 1000) return;

    auto lies = reinterpret_cast<PointLiesWithinZoneFn>(kPointLiesWithinZone);
    const uint8_t* smallest = nullptr;
    const uint8_t* largest = nullptr;
    long smallArea = 0, largeArea = 0;

    for (short i = 0; i < count; ++i) {
        auto* z = reinterpret_cast<uint8_t*>(kZoneInfoArray + i * kZoneSize);
        if (!lies(&pos, z)) continue;
        auto* c = reinterpret_cast<const short*>(z + 0x10);  // x1,y1,z1,x2,y2,z2
        long a = static_cast<long>(c[3] - c[0]) * static_cast<long>(c[4] - c[1]);
        if (!smallest || a < smallArea) { smallest = z; smallArea = a; }
        if (c[5] == 900 && (!largest || a > largeArea)) { largest = z; largeArea = a; }
    }
    if (smallest) snprintf(area, areaLen, "%s", ZoneText(smallest));
    if (largest)  snprintf(city, cityLen, "%s", ZoneText(largest));
}

void Sanitize(char* s) {
    for (; *s; ++s)
        if (*s == '|' || *s == '"' || *s == '\\' || static_cast<unsigned char>(*s) < 0x20) *s = ' ';
}

// Call sites inside CGame::Process (0x53BEE0), from plugin-sdk meta (GAME_10US_COMPACT, H_CALL).
// Only a site still pointing at its original GTA target is patched, so we never
// overwrite a hook another module (SA-MP / omp-cef) already owns. 0x53E981 (Idle ->
// CGame::Process) was already hooked by another module and crashed on reuse.
struct CallSite { uintptr_t site; uintptr_t originalTarget; const char* name; };
constexpr CallSite kCandidates[] = {
    {0x53BF4E, 0x6997E0, "CGame::Process -> 0x6997E0 (meta.CCover.h)"},
    {0x53C0BC, 0x440D10, "CGame::Process -> 0x440D10 (meta.CEntryExitManager.h)"},
};

using VoidFn = void(__cdecl*)();
VoidFn g_origTarget = nullptr;

void Tick();

void __cdecl HookedCall() {
    g_origTarget();
    Tick();
}

} // namespace

bool InstallProcessHook() {
    for (const auto& c : kCandidates) {
        auto* site = reinterpret_cast<uint8_t*>(c.site);
        if (site[0] != 0xE8) continue;
        uintptr_t target = c.site + 5 + *reinterpret_cast<int32_t*>(site + 1);
        if (target != c.originalTarget) continue;

        g_origTarget = reinterpret_cast<VoidFn>(target);
        int32_t newRel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookedCall) - (c.site + 5));
        DWORD old = 0;
        if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        *reinterpret_cast<int32_t*>(site + 1) = newRel;
        VirtualProtect(site + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);

        LogStage(11, c.name);
        return true;
    }
    LogStage(10, "no unhooked call site found, game-state tick disabled");
    return false;
}

namespace {

// ── SKR client-state RPC (format: shared/protocol/client_state.h) ─────────
// samp.dll R5 BitStream + RakClient, exactly as used by the command sender at RVA 0x69900
// (disassembled locally): ctor 0x1F730, WriteBits(ptr, bits, rightAligned) 0x1FFE0,
// Write(const char*, bytes) 0x20090, dtor 0x1F8D0; CNetGame+0 = RakClientInterface*,
// RPC(int* id, BitStream*, priority=1, reliability=8, channel=0, shiftTimestamp=0) at vtable +0x64.
constexpr uintptr_t kBsCtorRva = 0x1F730, kBsWriteBitsRva = 0x1FFE0;
constexpr uintptr_t kBsWriteBytesRva = 0x20090, kBsDtorRva = 0x1F8D0;
constexpr size_t kRakRpcVtableOffset = 0x64;

using BsThisFn = void(__thiscall*)(void*);
using BsWriteBitsFn = void(__thiscall*)(void*, const uint8_t*, int, bool);
using BsWriteBytesFn = void(__thiscall*)(void*, const char*, int);
using RakRpcFn = bool(__thiscall*)(void*, int*, void*, int, int, char, bool);

struct ClientState {
    float x, y, z, heading, range, orient;
    uint16_t speedKmh;
    int8_t gear;
    uint16_t maxKmh;
};

bool SendClientState(uintptr_t sampBase, uint8_t* netGame, const ClientState& st,
                     const char* city, const char* area) {
    void* rak = *reinterpret_cast<void**>(netGame);
    if (!rak) return false;

    // Larger than SA-MP's own stack BitStream (it lives inside a 0x118-byte frame).
    alignas(16) uint8_t bs[0x400] = {};
    auto ctor = reinterpret_cast<BsThisFn>(sampBase + kBsCtorRva);
    auto dtor = reinterpret_cast<BsThisFn>(sampBase + kBsDtorRva);
    auto bits = reinterpret_cast<BsWriteBitsFn>(sampBase + kBsWriteBitsRva);
    auto bytes = reinterpret_cast<BsWriteBytesFn>(sampBase + kBsWriteBytesRva);
    ctor(bs);

    auto put = [&](const void* p, int nbits) { bits(bs, static_cast<const uint8_t*>(p), nbits, true); };
    auto putName = [&](const char* s) {
        uint8_t len = static_cast<uint8_t>(strnlen(s, protocol::kClientStateMaxName));
        put(&len, 8);
        if (len) bytes(bs, s, len);
    };
    const uint8_t version = protocol::kClientStateVersion;
    put(&version, 8);
    put(&st.x, 32); put(&st.y, 32); put(&st.z, 32);
    put(&st.heading, 32); put(&st.range, 32); put(&st.orient, 32);
    put(&st.speedKmh, 16);
    put(&st.gear, 8);
    put(&st.maxKmh, 16);
    putName(city);
    putName(area);

    int rpcId = protocol::kRpcClientState;
    auto vtbl = *reinterpret_cast<uint8_t**>(rak);
    auto rpc = *reinterpret_cast<RakRpcFn*>(vtbl + kRakRpcVtableOffset);
    // SA-MP's bundled RakNet uses PacketReliability base 6: UNRELIABLE=6, RELIABLE=8,
    // RELIABLE_ORDERED=9 (D:\SA-MP Reverse\RE\client\docs\23-zonele-albe.md:51-52).
    // samp.dll calls this slot as (&rpcId, buffer, prio=1, rel=9, chan=0, bcast=0).
    bool ok = rpc(rak, &rpcId, bs, 1, 9, 0, false);
    dtor(bs);
    return ok;
}

// ── SKR session-info RPC 241 (shared/protocol/session_info.h) ────────────
// RakClientInterface slot +0x58 (index 22) = RegisterAsRemoteProcedureCall(const int* id,
// void(*)(RPCParameters*)) — how samp.dll R5 registers its own 141 RPCs
// (D:\SA-MP Reverse\RE\client\research\docs\network_protocol.md §4, CONFIRMED).
// RPCParameters starts with { unsigned char* input; unsigned int numberOfBitsOfData; }
// (SA-MP RakNet sdk/raknet/NetworkTypes.h). Handlers run on the game thread (NetGame tick).
constexpr size_t kRakRegisterRpcVtableOffset = 0x58;
struct RpcParams { const uint8_t* input; unsigned int numberOfBitsOfData; };
using RpcHandlerFn = void(__cdecl*)(RpcParams*);
using RakRegisterRpcFn = void(__thiscall*)(void*, const int*, RpcHandlerFn);

const int g_sessionInfoRpcId = protocol::kRpcSessionInfo;
void* g_sessionRpcRakClient = nullptr;

void __cdecl OnSessionInfoRpc(RpcParams* params) {
    if (!params || !params->input || params->numberOfBitsOfData < 16) return;
    if (params->input[0] != protocol::kSessionInfoVersion) return;
    skr::lobby::OnSessionInfo(params->input[1]);
}

// CNetGame (and its RakClient) is created once per session; register before
// the connection completes so the server's RPC 241 is not dropped.
void RegisterSessionInfoRpc(uint8_t* netGame) {
    void* rak = *reinterpret_cast<void**>(netGame);
    if (!rak || rak == g_sessionRpcRakClient) return;
    auto vtbl = *reinterpret_cast<uint8_t**>(rak);
    auto reg = *reinterpret_cast<RakRegisterRpcFn*>(vtbl + kRakRegisterRpcVtableOffset);
    reg(rak, &g_sessionInfoRpcId, &OnSessionInfoRpc);
    g_sessionRpcRakClient = rak;
    LogStage(13, "rpc 241 (session info) handler registered");
}

bool IsConnected() {
    HMODULE samp = GetModuleHandleA("samp.dll");
    if (!samp) return false;
    auto* netGame = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(samp) + kSampNetGameRva);
    return netGame && netGame[kNetGameStateOff] == kStateConnected;
}

// ── CHud::Draw hook — full native radar in RenderWare's 2D state ────────
// Tick() sets bDisplayHud = false every frame to suppress health/money/weapon
// (the CEF HUD replaces them).  But that also kills the radar blips/sprites.
// We temporarily re-enable it so the original CHud::Draw renders the complete
// radar (tiles + blips + player arrow + waypoint).  The CEF overlay, composited
// later in EndScene, covers the native health/money/weapon with its own elements.
VoidFn g_origHudDraw = nullptr;

void __cdecl HookedHudDraw() {
    bool skrSession = IsConnected() && skr::lobby::IsSkrSession();
    if (skrSession) {
        *reinterpret_cast<bool*>(kDisplayHud) = true;
    }
    if (g_origHudDraw) g_origHudDraw();
    if (skrSession) {
        *reinterpret_cast<bool*>(kDisplayHud) = false;
    }
}

void Tick() {
    // The CEF HUD replaces the native one while connected. Re-applied every frame because
    // SA-MP restores it (e.g. on spawn/spectate). Note: this also hides the crosshair.
    // CHud::bScriptDontDisplayRadar is intentionally NOT forced: forcing it crashed the game
    // at startup (reported by the user); the native radar stays visible for now.
    // Only on SKR servers (RPC 241 received): elsewhere the native HUD stays.
    if (IsConnected() && skr::lobby::IsSkrSession()) *reinterpret_cast<bool*>(kDisplayHud) = false;

    DWORD now = GetTickCount();
    if (now - g_lastSend < kSendIntervalMs) return;
    g_lastSend = now;

    LogStage(0, "tick running");
    HMODULE samp = GetModuleHandleA("samp.dll");
    if (!samp) { LogStage(1, "no samp.dll"); return; }
    auto base = reinterpret_cast<uintptr_t>(samp);
    auto* netGame = *reinterpret_cast<uint8_t**>(base + kSampNetGameRva);
    if (!netGame) { LogStage(2, "no CNetGame"); return; }
    RegisterSessionInfoRpc(netGame);
    skr::lobby::OnNetGameState(netGame[kNetGameStateOff]);
    if (netGame[kNetGameStateOff] != kStateConnected) {
        char m[64];
        snprintf(m, sizeof(m), "netgame state %u", netGame[kNetGameStateOff]);
        LogStage(3, m);
        return;
    }

    CVector pos{};
    float heading = 0.0f;
    if (!ReadPlayer(pos, heading)) { LogStage(4, "no player ped"); return; }

    // Connected with a ped in the world. Runs from CGame::Process, which keeps
    // ticking after SA-MP proxies the D3D device; the lobby decides when to
    // hand over based on the server's session info (RPC 241).
    skr::lobby::OnInWorldTick();
    skr::escape_menu::Tick();

    // Stream radar tiles during CGame::Process so the streaming system actually
    // loads them.  DrawMap also calls this from EndScene, but by then the
    // streaming update has already finished for this frame.
    if (skr::lobby::IsSkrSession()) {
        reinterpret_cast<void(__cdecl*)(const CVector&)>(kStreamRadarSections)(pos);

        // Diagnostic: log tile registration once
        auto* ids = reinterpret_cast<const int*>(kGRadarTxdIds);
        int reg = 0;
        for (int i = 0; i < 144; ++i) if (ids[i] != -1) ++reg;
        char m[96];
        snprintf(m, sizeof(m), "radar tiles: %d/144 registered, id[72]=%d", reg, ids[72]);
        LogStage(15, m);
    }

    // RPC 240 is SKR-private: never sent to servers without the SKR Gate.
    if (!skr::lobby::IsSkrSession()) { LogStage(14, "non-SKR server: rpc 240 disabled"); return; }

    char city[48], area[48];
    ResolveZones(pos, city, sizeof(city), area, sizeof(area));
    Sanitize(city);
    Sanitize(area);

    reinterpret_cast<VoidFn>(kCalcCachedSinCos)();
    float orient = std::atan2(*reinterpret_cast<float*>(kRadarCachedSin),
                              *reinterpret_cast<float*>(kRadarCachedCos)) * 57.2957795f;

    const CVector& v = reinterpret_cast<FindPlayerSpeedFn>(kFindPlayerSpeed)(-1);
    float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    float range = kRadarMinRange;
    if (speed > kRadarMaxSpeed) range = kRadarMaxRange;
    else if (speed > kRadarMinSpeed)
        range = (speed - kRadarMinSpeed) * (850.0f / (kRadarMinSpeed * 10.0f)) + kRadarMinRange;

    // Vehicle: speed from its own move vector, gear from CVehicle::m_nCurrentGear (-1 = on foot).
    int vehSpeedKmh = 0, gear = -1, maxKmh = 0;
    if (uint8_t* veh = reinterpret_cast<FindPlayerVehicleFn>(kFindPlayerVehicle)(-1, false)) {
        auto* mv = reinterpret_cast<const float*>(veh + kPhysMoveSpeed);
        vehSpeedKmh = static_cast<int>(std::sqrt(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]) * kSpeedToKmh + 0.5f);
        gear = veh[kVehCurrentGear];
        if (auto* handling = *reinterpret_cast<uint8_t**>(veh + kVehHandling)) {
            float raw = *reinterpret_cast<float*>(handling + kHandlingMaxGearVel);
            if (raw > 0.0f && raw < 10.0f) maxKmh = static_cast<int>(raw * kSpeedToKmh + 0.5f);
            char m[96];
            snprintf(m, sizeof(m), "first vehicle model %d: maxGearVelocity raw %.6f -> %d km/h",
                     *reinterpret_cast<int16_t*>(veh + 0x22), raw, maxKmh);   // CEntity::m_nModelIndex @0x22
            LogStage(12, m);
        }
    }

    ClientState st{};
    st.x = pos.x; st.y = pos.y; st.z = pos.z;
    st.heading = heading; st.range = range; st.orient = orient;
    st.speedKmh = static_cast<uint16_t>(vehSpeedKmh < 0 ? 0 : (vehSpeedKmh > 65535 ? 65535 : vehSpeedKmh));
    st.gear = static_cast<int8_t>(gear);
    st.maxKmh = static_cast<uint16_t>(maxKmh);
    if (SendClientState(base, netGame, st, city, area)) {
        char m[128];
        snprintf(m, sizeof(m), "rpc %u sent: %.1f %.1f %.1f %s|%s", protocol::kRpcClientState,
                 pos.x, pos.y, pos.z, city, area);
        LogStage(5, m);
    } else {
        LogStage(6, "rpc send failed (no RakClient)");
    }
}

} // namespace

bool InstallHudDrawHook() {
    // Idle's 2D render pass → CHud::Draw.  Provenance: plugin-sdk meta (H_CALL).
    constexpr uintptr_t kSite = 0x53E4FF;
    auto* site = reinterpret_cast<uint8_t*>(kSite);
    if (site[0] != 0xE8) {
        LogStage(16, "CHud::Draw call site: not a CALL instruction");
        return false;
    }
    uintptr_t target = kSite + 5 + *reinterpret_cast<int32_t*>(site + 1);
    if (target != kCHudDraw) {
        char m[96];
        snprintf(m, sizeof(m), "CHud::Draw call site target 0x%08X != 0x%08X",
                 static_cast<unsigned>(target), static_cast<unsigned>(kCHudDraw));
        LogStage(16, m);
        return false;
    }

    g_origHudDraw = reinterpret_cast<VoidFn>(target);
    int32_t newRel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&HookedHudDraw) - (kSite + 5));
    DWORD old = 0;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<int32_t*>(site + 1) = newRel;
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);

    LogStage(16, "CHud::Draw hooked — radar DrawMap moved from EndScene");
    return true;
}

} // namespace skr::game_state
