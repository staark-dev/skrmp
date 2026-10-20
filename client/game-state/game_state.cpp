#include "game-state/game_state.h"
#include "protocol/client_state.h"
#include "protocol/session_info.h"
#include "protocol/enhanced.h"
#include "protocol/radar_style.h"
#include "protocol/radar_rect.h"
#include "lobby/lobby_manager.h"
#include "escape-menu/escape_menu.h"
#include "intro/launch_cover.h"
#include <windows.h>
#include <atomic>
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
namespace { std::atomic<bool> g_enhancedAdvertised{false}; }
void ConfigureEnhancedDiscovery(bool advertised) { g_enhancedAdvertised.store(advertised); }

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
// CRadar::DrawRadarMask (cdecl, no params) — circular clip mask. Verified in
// gta_sa.exe: sole caller is DrawRadarMap (0x586880) at 0x586887, frame prologue.
// (0x586110 is DrawRadarSection(x, y), called 9x for the 3x3 tile grid — not the mask.)
constexpr uintptr_t kDrawRadarMask = 0x585700;
// CHud::DrawRadar (0x58A330): the black ring is 4x CSprite2d::Draw(rect, color)
// (thiscall, ret 8, ecx = 0xBAB208) — one per quadrant.
constexpr uintptr_t kRadarRingDrawCalls[] = {0x58A823, 0x58A8CD, 0x58A977, 0x58AA25};
constexpr uintptr_t kSprite2dDraw = 0x728350;
// Radar geometry in 640x448 HUD units (.rdata floats read by
// TransformRadarPointToScreenSpace 0x583480 and CHud radar code only):
// width 94, height 76, top edge 104 above the screen bottom.
// samp.dll R5 wraps CHud::DrawRadar (RVA ~0xA2500: `mov edx, 0x58A330; call edx`) and
// writes its own width/height before the call, restoring 94/76 after — so the scale is
// applied inside DrawRadar (detour below), on top of SA-MP's values.
constexpr uintptr_t kRadarWidth = 0x866B78;
constexpr uintptr_t kRadarHeight = 0x866B74;
constexpr uintptr_t kRadarTopFromBottom = 0x866B70;
// CHud::DrawRadar (cdecl, no params; plugin-sdk CHud.cpp). Entry: mov eax, [0x96A7CC].
constexpr uintptr_t kCHudDrawRadar = 0x58A330;
constexpr uint8_t kDrawRadarEntry[5] = {0xA1, 0xCC, 0xA7, 0x96, 0x00};
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
// plugin-sdk RenderWare.h: RsGlobal.maximumWidth/Height — actual render resolution.
constexpr uintptr_t kRsMaxWidth  = 0xC17044;
constexpr uintptr_t kRsMaxHeight = 0xC17048;
// HUD base resolution used by GTA's coordinate transform (640x448).
constexpr float kHudBaseWidth  = 640.0f;
constexpr float kHudBaseHeight = 448.0f;
// Left margin of the radar in HUD units (CHud::DrawRadar hardcodes 40.0f).
constexpr float kRadarLeftMargin = 40.0f;
// GTA speed units are per 1/50 s frame: km/h = |v| * 50 * 3.6.
constexpr float kSpeedToKmh = 180.0f;

constexpr DWORD kSendIntervalMs = 100;

using PointLiesWithinZoneFn = bool(__cdecl*)(const CVector*, void*);
using CTextGetFn            = const char*(__thiscall*)(void*, const char*);
using VoidFn                = void(__cdecl*)();
using FindPlayerSpeedFn     = const CVector&(__cdecl*)(int);
using FindPlayerVehicleFn   = uint8_t*(__cdecl*)(int, bool);

DWORD g_lastSend = 0;

// ── Log helpers ─────────────────────────────────────────────────────
// GetLogPath: resolves <gta>\SKR\game_state.log once, caches it.
static char g_logPath[MAX_PATH] = {};
static bool g_logPathOk = false;
static const char* GetLogPath() {
    if (g_logPathOk) return g_logPath;
    DWORD n = GetModuleFileNameA(nullptr, g_logPath, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    char* slash = strrchr(g_logPath, '\\');
    if (!slash) return nullptr;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - g_logPath), "SKR\\game_state.log");
    g_logPathOk = true;
    return g_logPath;
}

// Diagnostic: logs each distinct stage once to <gta>\SKR\game_state.log.
uint32_t g_loggedStages = 0;
void LogStage(int stage, const char* msg) {
    if (g_loggedStages & (1u << stage)) return;
    g_loggedStages |= (1u << stage);
    const char* path = GetLogPath();
    if (!path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] stage %d: %s\n", GetTickCount(), stage, msg);
    fclose(f);
}

// LogDiag: rate-limited repeated diagnostic logging (every ~5 seconds per tag).
// Unlike LogStage (one-shot), this logs periodically for ongoing diagnostics.
static DWORD g_diagLastLog_HudDraw = 0;
static DWORD g_diagLastLog_DrawRadar = 0;
static DWORD g_diagLastLog_CallSite = 0;

static void LogDiag(const char* tag, const char* msg) {
    const char* path = GetLogPath();
    if (!path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] DIAG %s: %s\n", GetTickCount(), tag, msg);
    fclose(f);
}

// ── Diagnostic counters ─────────────────────────────────────────────
static std::atomic<uint32_t> g_hudDrawCount{0};
static std::atomic<uint32_t> g_drawRadarCount{0};
static std::atomic<uint32_t> g_trampolineCallCount{0};

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

// ── Radar rect (RPC 243, shared/protocol/radar_rect.h) ──────────────────
// Sent whenever the client's radar screen-space rectangle changes.
struct RadarRect { float x, y, w, h; };
RadarRect g_lastSentRect = {};
bool g_radarRectDirty = false;

bool SendRadarRect(uintptr_t sampBase, uint8_t* netGame, const RadarRect& r) {
    void* rak = *reinterpret_cast<void**>(netGame);
    if (!rak) return false;

    alignas(16) uint8_t bs[0x100] = {};
    auto ctor = reinterpret_cast<BsThisFn>(sampBase + kBsCtorRva);
    auto dtor = reinterpret_cast<BsThisFn>(sampBase + kBsDtorRva);
    auto bits = reinterpret_cast<BsWriteBitsFn>(sampBase + kBsWriteBitsRva);
    ctor(bs);

    auto put = [&](const void* p, int nbits) { bits(bs, static_cast<const uint8_t*>(p), nbits, true); };
    const uint8_t version = protocol::kRadarRectVersion;
    put(&version, 8);
    put(&r.x, 32); put(&r.y, 32); put(&r.w, 32); put(&r.h, 32);

    int rpcId = protocol::kRpcRadarRect;
    auto vtbl = *reinterpret_cast<uint8_t**>(rak);
    auto rpc = *reinterpret_cast<RakRpcFn*>(vtbl + kRakRpcVtableOffset);
    bool ok = rpc(rak, &rpcId, bs, 1, 9, 0, false);
    dtor(bs);
    return ok;
}

// ── Radar style/size (RPC 242, shared/protocol/radar_style.h) ────────────
// Patches are verified once (VerifyRadarPatchSites) and toggled at runtime:
// Square = DrawRadarMask returns at entry + the 4 ring CALLs become `add esp, 8`;
// the scale is applied per frame in HookedDrawRadar.
// Everything runs on the game thread (RPC handlers and Tick).
bool g_radarMaskOk = false;
bool g_radarRingOk = false;
uint8_t g_radarRingOrig[4][5] = {};
protocol::RadarStyle g_radarStyle = protocol::RadarStyle::Round;
float g_radarScale = 1.0f;
bool g_detourActive = false;            // true while our JMP is live at 0x58A330
bool g_radarFloatsUnlocked = false;     // VirtualProtect .rdata once

// Radar rect globals are defined above, near SendRadarRect.

void WriteCode(uintptr_t addr, const void* src, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(reinterpret_cast<void*>(addr), src, len);
    VirtualProtect(reinterpret_cast<void*>(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
}

void VerifyRadarPatchSites() {
    auto* mask = reinterpret_cast<const uint8_t*>(kDrawRadarMask);
    g_radarMaskOk = mask[0] == 0x55 && mask[1] == 0x8B && mask[2] == 0xEC;

    g_radarRingOk = true;
    for (int i = 0; i < 4; ++i) {
        auto* p = reinterpret_cast<const uint8_t*>(kRadarRingDrawCalls[i]);
        if (p[0] != 0xE8 || kRadarRingDrawCalls[i] + 5 + *reinterpret_cast<const int32_t*>(p + 1) != kSprite2dDraw)
            g_radarRingOk = false;
        else
            memcpy(g_radarRingOrig[i], p, 5);
    }

    char m[80];
    snprintf(m, sizeof(m), "radar patch sites: mask %d ring %d", g_radarMaskOk, g_radarRingOk);
    LogStage(17, m);
}

VoidFn g_drawRadarTrampoline = nullptr;

// Scales whatever width/height SA-MP set for this frame; the top edge is raised by
// the extra height so the bottom margin stays. Restored so SA-MP's own restore matches.
// Crash counter for radar — stops calling the trampoline after repeated crashes
static std::atomic<int> g_radarCrashCount{0};
constexpr int kMaxRadarCrashes = 3;

void __cdecl HookedDrawRadar() {
    g_drawRadarCount.fetch_add(1);

    auto* w = reinterpret_cast<float*>(kRadarWidth);
    auto* h = reinterpret_cast<float*>(kRadarHeight);
    auto* top = reinterpret_cast<float*>(kRadarTopFromBottom);
    const float s = g_radarScale;
    const float w0 = *w, h0 = *h, top0 = *top;

    // Rate-limited diagnostic: every 5 seconds
    DWORD now = GetTickCount();
    if (now - g_diagLastLog_DrawRadar >= 5000) {
        g_diagLastLog_DrawRadar = now;
        uint8_t displayHud = *reinterpret_cast<uint8_t*>(kDisplayHud);
        char m[320];
        snprintf(m, sizeof(m),
                 "calls=%u trampCalls=%u crashCount=%d trampoline=0x%p "
                 "w=%.1f h=%.1f top=%.1f scale=%.2f style=%d bDisplayHud=%u "
                 "maskByte=0x%02X",
                 g_drawRadarCount.load(), g_trampolineCallCount.load(),
                 g_radarCrashCount.load(),
                 reinterpret_cast<void*>(g_drawRadarTrampoline),
                 w0, h0, top0, s, static_cast<int>(g_radarStyle), displayHud,
                 *reinterpret_cast<uint8_t*>(kDrawRadarMask));
        LogDiag("DrawRadar", m);
    }

    if (s != 1.0f) {
        *w = w0 * s;
        *h = h0 * s;
        *top = top0 + h0 * (s - 1.0f);
    }
    const float screenW = static_cast<float>(*reinterpret_cast<int*>(kRsMaxWidth));
    const float screenH = static_cast<float>(*reinterpret_cast<int*>(kRsMaxHeight));
    const float sx = screenW / kHudBaseWidth;
    const float sy = screenH / kHudBaseHeight;
    RadarRect rect;
    rect.w = *w * sx;
    rect.h = *h * sy;
    rect.x = kRadarLeftMargin * sx;
    rect.y = screenH - *top * sy;

    if (g_radarCrashCount.load() < kMaxRadarCrashes) {
        __try {
            g_trampolineCallCount.fetch_add(1);
            g_drawRadarTrampoline();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_radarCrashCount.fetch_add(1);
            LogStage(21 + g_radarCrashCount.load(),
                     "SEH: DrawRadar trampoline crashed — radar drawing disabled");
        }
    }

    if (s != 1.0f) {
        *w = w0;
        *h = h0;
        *top = top0;
    }

    if (std::abs(rect.x - g_lastSentRect.x) > 1.0f ||
        std::abs(rect.y - g_lastSentRect.y) > 1.0f ||
        std::abs(rect.w - g_lastSentRect.w) > 1.0f ||
        std::abs(rect.h - g_lastSentRect.h) > 1.0f) {
        g_lastSentRect = rect;
        g_radarRectDirty = true;
    }
}

// -- DrawRadar detour -- survives SA-MP dynamic toggling ---------------
// SA-MP hides the radar by writing 0xC3 (RET) to CHud::DrawRadar
// (0x58A330) and restores the original bytes at OnPlayerSpawn.
// Both operations overwrite our JMP.  MaintainDrawRadarDetour() is
// called every CGame::Process frame and re-applies the JMP whenever
// SA-MP restores the original entry bytes.

// Create the trampoline once (copies original entry bytes + jmp back).
void EnsureDrawRadarTrampoline() {
    if (g_drawRadarTrampoline) return;
    auto* fn = reinterpret_cast<uint8_t*>(kCHudDrawRadar);
    if (memcmp(fn, kDrawRadarEntry, sizeof(kDrawRadarEntry)) != 0) return;

    auto* tramp = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return;
    memcpy(tramp, kDrawRadarEntry, 5);
    tramp[5] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + 6) =
        static_cast<int32_t>((kCHudDrawRadar + 5) - (reinterpret_cast<uintptr_t>(tramp) + 10));
    g_drawRadarTrampoline = reinterpret_cast<VoidFn>(tramp);
    LogStage(19, "DrawRadar trampoline allocated");
}

// Write our E9 (JMP) over the first 5 bytes of 0x58A330.
void WriteDrawRadarJmp() {
    auto* fn = reinterpret_cast<uint8_t*>(kCHudDrawRadar);
    uint8_t patch[8];
    memcpy(patch, fn, 8);
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(patch + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HookedDrawRadar) - (kCHudDrawRadar + 5));
    DWORD old = 0;
    if (!VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &old)) return;
    LONG64 value;
    memcpy(&value, patch, 8);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(fn), value);
    VirtualProtect(fn, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 8);
    g_detourActive = true;
}

// Per-frame poll: re-apply the detour after SA-MP restores DrawRadar.
void MaintainDrawRadarDetour() {
    auto* fn = reinterpret_cast<const uint8_t*>(kCHudDrawRadar);
    const uint8_t entry = fn[0];

    if (entry == 0xE9) {                       // our JMP still in place
        g_detourActive = true;
        return;
    }
    if (entry == 0xC3) {                       // SA-MP hiding radar (RET)
        // After first spawn the trampoline exists.  SA-MP keeps writing
        // 0xC3 but only touches byte 0 -- our JMP offset at bytes 1-4
        // is still intact.  Restore byte 0 to 0xE9 every frame so the
        // JMP is in place when CHud::Draw runs (CGame::Process < Draw).
        if (g_drawRadarTrampoline) {
            auto* p = const_cast<uint8_t*>(fn);
            DWORD oldProt = 0;
            if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
                *p = 0xE9;
                VirtualProtect(p, 1, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), p, 1);
                g_detourActive = true;
            }
        } else {
            g_detourActive = false;
        }
        return;
    }
    if (entry == kDrawRadarEntry[0]) {         // 0xA1 -- SA-MP restored original
        // Unlock .rdata floats once
        if (!g_radarFloatsUnlocked) {
            DWORD old = 0;
            if (VirtualProtect(reinterpret_cast<void*>(kRadarTopFromBottom), 12,
                               PAGE_READWRITE, &old))
                g_radarFloatsUnlocked = true;
        }
        EnsureDrawRadarTrampoline();
        if (g_drawRadarTrampoline) {
            WriteDrawRadarJmp();
            LogStage(28, "DrawRadar detour applied after SA-MP restore");
        }
        return;
    }
    // Unknown entry byte -- log once and leave alone
    char m[80];
    snprintf(m, sizeof(m), "DrawRadar entry byte 0x%02X unexpected", entry);
    LogStage(29, m);
}

void ApplyRadar(protocol::RadarStyle style, float scale) {
    if (style != g_radarStyle) {
        const bool square = style == protocol::RadarStyle::Square;
        if (g_radarMaskOk) {
            const uint8_t entry = square ? 0xC3 : 0x55;   // ret / original push ebp
            WriteCode(kDrawRadarMask, &entry, 1);
        }
        if (g_radarRingOk) {
            static const uint8_t kAddEsp8[5] = {0x83, 0xC4, 0x08, 0x90, 0x90};
            for (int i = 0; i < 4; ++i)
                WriteCode(kRadarRingDrawCalls[i], square ? kAddEsp8 : g_radarRingOrig[i], 5);
        }
        g_radarStyle = style;
    }
    g_radarScale = scale;
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

enum class Negotiation { Idle, HelloSent, OfferReceived, ConfirmSent, Enhanced, Failed };
Negotiation g_negotiation = Negotiation::Idle;
uint32_t g_negotiationToken = 0;
ULONGLONG g_negotiationDeadline = 0;
bool g_wasConnected = false;

bool SendNegotiation(uintptr_t base, uint8_t* netGame, protocol::EnhancedStep step, uint32_t token) {
    void* rak = *reinterpret_cast<void**>(netGame);
    if (!rak) return false;
    alignas(16) uint8_t bs[0x400]{};
    reinterpret_cast<BsThisFn>(base + kBsCtorRva)(bs);
    uint8_t packet[8]{protocol::kEnhancedMarker, static_cast<uint8_t>(step),
        protocol::kEnhancedVersion, protocol::kEnhancedCapabilities};
    memcpy(packet + 4, &token, 4); // verified x86 little-endian wire representation
    reinterpret_cast<BsWriteBytesFn>(base + kBsWriteBytesRva)(bs, reinterpret_cast<const char*>(packet), 8);
    int id = protocol::kRpcClientState;
    auto vtbl = *reinterpret_cast<uint8_t**>(rak);
    const bool sent = (*reinterpret_cast<RakRpcFn*>(vtbl + kRakRpcVtableOffset))(rak, &id, bs, 1, 9, 0, false);
    reinterpret_cast<BsThisFn>(base + kBsDtorRva)(bs);
    return sent;
}

void __cdecl OnSessionInfoRpc(RpcParams* params) {
    if (!params || !params->input || !g_enhancedAdvertised.load()) return;
    const auto* in = params->input;
    if (GetTickCount64() >= g_negotiationDeadline) return;
    if (params->numberOfBitsOfData == 64 && in[0] == protocol::kEnhancedMarker) {
        if (g_negotiation != Negotiation::HelloSent ||
            in[1] != static_cast<uint8_t>(protocol::EnhancedStep::Offer) ||
            in[2] != protocol::kEnhancedVersion || in[3] != protocol::kEnhancedCapabilities) return;
        memcpy(&g_negotiationToken, in + 4, 4);
        if (g_negotiationToken) g_negotiation = Negotiation::OfferReceived;
        return;
    }
    if (params->numberOfBitsOfData != 16 || in[0] != protocol::kSessionInfoVersion ||
        in[1] > 1 || g_negotiation != Negotiation::ConfirmSent) return;
    g_negotiation = Negotiation::Enhanced;
    skr::lobby::OnSessionInfo(in[1]);
    LogStage(26, "Enhanced capabilities confirmed (not authentication)");
}

void TickNegotiation(uintptr_t base, uint8_t* netGame) {
    const bool connected = netGame[kNetGameStateOff] == kStateConnected;
    if (!connected) {
        if (g_wasConnected) {
            skr::lobby::ResetEnhancedSession();
            ApplyRadar(protocol::RadarStyle::Round, 1.0f);
            g_negotiation = Negotiation::Idle;
            g_negotiationToken = 0;
            g_negotiationDeadline = 0;
        }
        g_wasConnected = false;
        return;
    }
    g_wasConnected = true;
    if (!g_enhancedAdvertised.load()) return;
    const auto now = GetTickCount64();
    if (g_negotiation == Negotiation::Idle) {
        g_negotiationDeadline = now + protocol::kEnhancedTimeoutMs;
        g_negotiation = SendNegotiation(base, netGame, protocol::EnhancedStep::Hello, 0)
            ? Negotiation::HelloSent : Negotiation::Failed;
        LogStage(27, "Enhanced hello attempted after native discovery");
    } else if (g_negotiation != Negotiation::Enhanced && now >= g_negotiationDeadline) {
        g_negotiation = Negotiation::Failed;
    } else if (g_negotiation == Negotiation::OfferReceived) {
        g_negotiation = SendNegotiation(base, netGame, protocol::EnhancedStep::Confirm, g_negotiationToken)
            ? Negotiation::ConfirmSent : Negotiation::Failed;
    }
}

const int g_radarStyleRpcId = protocol::kRpcRadarStyle;

void __cdecl OnRadarStyleRpc(RpcParams* params) {
    if (!params || !params->input || params->numberOfBitsOfData < 7 * 8) return;
    if (!skr::lobby::IsSkrSession()) return;
    const uint8_t* in = params->input;
    if (in[0] != protocol::kRadarStyleVersion || in[1] > 1 || in[2] > 1) return;
    float scale = 1.0f;
    if (in[2] == static_cast<uint8_t>(protocol::RadarSizeMode::Custom)) {
        memcpy(&scale, in + 3, sizeof(scale));
        if (!std::isfinite(scale) || scale < protocol::kRadarMinScale || scale > protocol::kRadarMaxScale)
            return;
    }
    ApplyRadar(static_cast<protocol::RadarStyle>(in[1]), scale);
}

// CNetGame (and its RakClient) is created once per session; register before
// the connection completes so the server's RPCs 241/242 are not dropped.
void RegisterSessionInfoRpc(uint8_t* netGame) {
    void* rak = *reinterpret_cast<void**>(netGame);
    if (!rak || rak == g_sessionRpcRakClient) return;
    auto vtbl = *reinterpret_cast<uint8_t**>(rak);
    auto reg = *reinterpret_cast<RakRegisterRpcFn*>(vtbl + kRakRegisterRpcVtableOffset);
    reg(rak, &g_sessionInfoRpcId, &OnSessionInfoRpc);
    reg(rak, &g_radarStyleRpcId, &OnRadarStyleRpc);
    g_sessionRpcRakClient = rak;
    LogStage(13, "rpc 241 (session info) + 242 (radar style) handlers registered");
}

bool IsConnected() {
    HMODULE samp = GetModuleHandleA("samp.dll");
    if (!samp) return false;
    auto* netGame = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(samp) + kSampNetGameRva);
    return netGame && netGame[kNetGameStateOff] == kStateConnected;
}

// ── CHud::Draw hook — passthrough with diagnostic logging ──
VoidFn g_origHudDraw = nullptr;

void __cdecl HookedHudDraw() {
    g_hudDrawCount.fetch_add(1);

    // Rate-limited diagnostic: every 5 seconds
    DWORD now = GetTickCount();
    if (now - g_diagLastLog_HudDraw >= 5000) {
        g_diagLastLog_HudDraw = now;
        uint8_t displayHud = *reinterpret_cast<uint8_t*>(kDisplayHud);
        char m[256];
        snprintf(m, sizeof(m),
                 "calls=%u bDisplayHud=%u origHudDraw=0x%p radarStyle=%d radarScale=%.2f crashCount=%d",
                 g_hudDrawCount.load(), displayHud,
                 reinterpret_cast<void*>(g_origHudDraw),
                 static_cast<int>(g_radarStyle), g_radarScale,
                 g_radarCrashCount.load());
        LogDiag("HudDraw", m);

        // Check if call site 0x53E4FF still points to our hook
        constexpr uintptr_t kSite = 0x53E4FF;
        auto* site = reinterpret_cast<uint8_t*>(kSite);
        if (site[0] == 0xE8) {
            uintptr_t currentTarget = kSite + 5 + *reinterpret_cast<int32_t*>(site + 1);
            if (currentTarget != reinterpret_cast<uintptr_t>(&HookedHudDraw)) {
                char m2[128];
                snprintf(m2, sizeof(m2),
                         "ALERT: call site 0x53E4FF retargeted to 0x%08X (not our hook 0x%p)!",
                         static_cast<unsigned>(currentTarget),
                         reinterpret_cast<void*>(&HookedHudDraw));
                LogDiag("CallSite", m2);
            }
        }

        // Check DrawRadar entry: is our detour still in place?
        auto* drEntry = reinterpret_cast<uint8_t*>(kCHudDrawRadar);
        if (drEntry[0] != 0xE9) {
            char m3[128];
            snprintf(m3, sizeof(m3),
                     "ALERT: CHud::DrawRadar (0x58A330) entry byte is 0x%02X, not 0xE9 (jmp)!",
                     drEntry[0]);
            LogDiag("DetourCheck", m3);
        }
    }

    if (g_origHudDraw) g_origHudDraw();
}

void Tick() {
    // Native HUD stays visible; the server hides individual components via
    // CEF_ToggleHudComponent. Radar style/size come from the server (RPC 242);
    // not gated on IsConnected: 241/242 can arrive before SA-MP reports connected.
    if (!skr::lobby::IsSkrSession()) ApplyRadar(protocol::RadarStyle::Round, 1.0f);

    // Re-apply DrawRadar detour after SA-MP restores original bytes (at spawn).
    // Runs every frame -- not rate-limited -- so we catch the transition immediately.
    MaintainDrawRadarDetour();

    // Early CNetGame detection -- runs every CGame::Process frame (no rate limit).
    // Signals the cover to dismiss BEFORE the "Started" chat message renders.
    {
        HMODULE sampEarly = GetModuleHandleA("samp.dll");
        if (sampEarly) {
            auto* ng = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uintptr_t>(sampEarly) + kSampNetGameRva);
            if (ng) skr::launch_cover::MarkNetGameReady();
        }
    }

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
    TickNegotiation(base, netGame);
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

    // Failsafe ONLY — not the normal dismiss path. The cover now dismisses
    // itself in Draw() at the first frame where worldConnected + negotiation
    // done + takeover.  This fires only if that path somehow fails after 5 s.
    {
        static DWORD s_inWorldSince = 0;
        if (!s_inWorldSince) s_inWorldSince = GetTickCount();
        if (GetTickCount() - s_inWorldSince >= 5000)
            skr::launch_cover::MarkGameplayConfirmed();
    }

    // Stream radar tiles during CGame::Process so the streaming system actually
    // loads them.  DrawMap also calls this from EndScene, but by then the
    // streaming update has already finished for this frame.
    if (skr::lobby::IsSkrSession()) {
        __try {
            reinterpret_cast<void(__cdecl*)(const CVector&)>(kStreamRadarSections)(pos);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogStage(25, "SEH: StreamRadarSections crashed");
        }

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

    // RPC 243: radar screen-space rect, sent only when it changes.
    // TODO: disabled for debugging random quits — re-enable after testing
    if (false && g_radarRectDirty) {
        if (SendRadarRect(base, netGame, g_lastSentRect)) {
            g_radarRectDirty = false;
            char m[96];
            snprintf(m, sizeof(m), "rpc %u sent: %.0f %.0f %.0f %.0f",
                     protocol::kRpcRadarRect, g_lastSentRect.x, g_lastSentRect.y,
                     g_lastSentRect.w, g_lastSentRect.h);
            LogStage(20, m);
        }
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

    LogStage(16, "CHud::Draw hooked — native radar with CEF mask overlay");

    // One-shot diagnostic: log initial radar state at hook install time
    {
        uint8_t displayHud = *reinterpret_cast<uint8_t*>(kDisplayHud);
        float rw = *reinterpret_cast<float*>(kRadarWidth);
        float rh = *reinterpret_cast<float*>(kRadarHeight);
        float rt = *reinterpret_cast<float*>(kRadarTopFromBottom);
        uint8_t maskByte = *reinterpret_cast<uint8_t*>(kDrawRadarMask);
        char dm[256];
        snprintf(dm, sizeof(dm),
                 "INIT bDisplayHud=%u radarW=%.1f radarH=%.1f radarTop=%.1f "
                 "maskEntry=0x%02X scale=%.2f style=%d",
                 displayHud, rw, rh, rt, maskByte, g_radarScale,
                 static_cast<int>(g_radarStyle));
        LogDiag("HookInstall", dm);
    }

    VerifyRadarPatchSites();
    // DrawRadar detour is NOT installed here.  SA-MP writes 0xC3 (RET) to
    // CHud::DrawRadar entry to hide the radar until OnPlayerSpawn; any JMP
    // we install now would be overwritten.  MaintainDrawRadarDetour() in
    // Tick() polls the entry byte every frame and applies the detour as
    // soon as SA-MP restores the original bytes.
    return true;
}

bool IsEnhancedNegotiationPending() {
    return g_enhancedAdvertised.load() && g_negotiation != Negotiation::Enhanced &&
        g_negotiation != Negotiation::Failed;
}

} // namespace skr::game_state
