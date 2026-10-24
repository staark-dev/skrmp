#include "native-pause/pause_map.h"
#include "native-pause/pause_enhanced.h"
#include "gps/gps_destination.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace skr::native_pause::map {
namespace {

// ── Log (shared native_pause.log) ───────────────────────────────────
void Log(const char* fmt, ...) {
    if (!skr::native_pause::enhanced::DebugLogEnabled()) return;
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "SKR\\native_pause.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%lu] MAP: ", GetTickCount());
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

// ── GTA SA 1.0 US — player pose (same fields as game_state.cpp ReadPlayer) ─
constexpr uintptr_t kPlayersPed        = 0xB7CD98;   // CWorld::Players[0].m_pPed
constexpr uintptr_t kFindPlayerVehicle = 0x56E0D0;   // cdecl CVehicle*(int, bool)
using FindPlayerVehicleFn = uint8_t*(__cdecl*)(int, bool);

// ── Archive / texture formats ────────────────────────────────────────
// IMG v2 (GTA SA): "VER2", uint32 count, count x { uint32 offsetSectors,
// uint16 streamingSectors, uint16 archiveSectors, char name[24] }, 2048-byte sectors.
// RenderWare chunks: { uint32 type, uint32 size, uint32 version }.
constexpr uint32_t kChunkStruct        = 0x01;
constexpr uint32_t kChunkTextureNative = 0x15;
constexpr uint32_t kChunkTexDictionary = 0x16;
constexpr size_t   kSector = 2048;
constexpr size_t   kMaxTxdBytes = 8u << 20;
constexpr int      kTilesPerUpdate = 6;

struct Tile {
    IDirect3DTexture9* tex = nullptr;
    float uInset = 0.0f, vInset = 0.0f;   // half-texel inset against edge bleeding
    bool attempted = false;
};

struct ImgEntry { uint32_t offset = 0; uint32_t bytes = 0; bool found = false; };

IDirect3DDevice9* g_device = nullptr;
Tile     g_tiles[kTileCount];
ImgEntry g_entries[kTileCount];
HANDLE   g_img = INVALID_HANDLE_VALUE;
bool     g_indexed = false;
bool     g_failed = false;
char     g_error[160] = {};
int      g_next = 0;
int      g_loaded = 0;
bool     g_poolDefault = false;   // true if D3DPOOL_MANAGED was rejected
bool     g_loggedFirst = false;

void Fail(const char* msg) {
    g_failed = true;
    snprintf(g_error, sizeof(g_error), "%s", msg);
    Log("error: %s", msg);
}

void CloseImg() {
    if (g_img != INVALID_HANDLE_VALUE) CloseHandle(g_img);
    g_img = INVALID_HANDLE_VALUE;
}

bool ReadAt(uint64_t offset, void* dst, size_t bytes) {
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(g_img, pos, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    return ReadFile(g_img, dst, static_cast<DWORD>(bytes), &read, nullptr) && read == bytes;
}

bool OpenAndIndex() {
    g_indexed = true;
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = (n && n < MAX_PATH) ? strrchr(path, '\\') : nullptr;
    if (!slash) { Fail("game directory not found"); return false; }
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "models\\gta3.img");
    // GTA keeps gta3.img open for reading; share read/write so we never block it.
    g_img = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_img == INVALID_HANDLE_VALUE) { Fail("models\\gta3.img could not be opened"); return false; }

    uint8_t header[8];
    if (!ReadAt(0, header, 8) || memcmp(header, "VER2", 4) != 0) {
        CloseImg();
        Fail("gta3.img is not an IMG v2 archive");
        return false;
    }
    uint32_t count = 0;
    memcpy(&count, header + 4, 4);
    if (count == 0 || count > 200000) { CloseImg(); Fail("gta3.img directory is invalid"); return false; }

    std::vector<uint8_t> dir(static_cast<size_t>(count) * 32);
    if (!ReadAt(8, dir.data(), dir.size())) { CloseImg(); Fail("gta3.img directory read failed"); return false; }
    int found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = dir.data() + static_cast<size_t>(i) * 32;
        char name[25] = {};
        memcpy(name, e + 8, 24);
        // radar00.txd … radar143.txd (case-insensitive)
        if (_strnicmp(name, "radar", 5) != 0) continue;
        char* dot = strchr(name, '.');
        if (!dot || _stricmp(dot, ".txd") != 0) continue;
        *dot = '\0';
        const char* digits = name + 5;
        if (!*digits || strlen(digits) > 3) continue;
        bool numeric = true;
        for (const char* p = digits; *p; ++p) numeric = numeric && (*p >= '0' && *p <= '9');
        if (!numeric) continue;
        const int index = atoi(digits);
        if (index < 0 || index >= kTileCount) continue;
        uint32_t offset; uint16_t streaming, archive;
        memcpy(&offset, e, 4);
        memcpy(&streaming, e + 4, 2);
        memcpy(&archive, e + 6, 2);
        const uint32_t sectors = archive ? archive : streaming;
        g_entries[index].offset = offset;
        g_entries[index].bytes = sectors * static_cast<uint32_t>(kSector);
        g_entries[index].found = sectors > 0;
        found += g_entries[index].found ? 1 : 0;
    }
    Log("gta3.img: %u entries, %d / %d radar tiles indexed", count, found, kTileCount);
    if (found == 0) { CloseImg(); Fail("no radar tiles found in gta3.img"); return false; }
    return true;
}

// ── RenderWare texture native ────────────────────────────────────────
struct Chunk { uint32_t type, size; const uint8_t* data; };

bool ReadChunk(const uint8_t* p, const uint8_t* end, Chunk& c) {
    if (end - p < 12) return false;
    memcpy(&c.type, p, 4);
    memcpy(&c.size, p + 4, 4);
    c.data = p + 12;
    return c.size <= static_cast<size_t>(end - c.data);
}

struct Image {
    D3DFORMAT format = D3DFMT_UNKNOWN;
    int width = 0, height = 0;
    bool palette8 = false;
    const uint8_t* palette = nullptr;   // 256 x RGBA
    int levels = 0;
    const uint8_t* level[16] = {};
    uint32_t levelSize[16] = {};
    uint32_t platform = 0, rasterFormat = 0;
};

uint32_t BlockBytes(D3DFORMAT f) {
    return f == D3DFMT_DXT1 ? 8u : (f == D3DFMT_DXT3 || f == D3DFMT_DXT5) ? 16u : 0u;
}
uint32_t PixelBytes(D3DFORMAT f) {
    switch (f) {
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return 4;
        case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X1R5G5B5: return 2;
        default: return 0;
    }
}
uint32_t LevelBytes(D3DFORMAT f, bool pal8, int w, int h) {
    if (pal8) return static_cast<uint32_t>(w * h);
    if (const uint32_t bb = BlockBytes(f))
        return static_cast<uint32_t>((std::max)(1, (w + 3) / 4) * (std::max)(1, (h + 3) / 4)) * bb;
    return static_cast<uint32_t>(w * h) * PixelBytes(f);
}

// Texture native struct (GTA SA PC = platform 9 / D3D9; platform 8 = D3D8 layout):
// +0 platform, +4 filter/addressing, +8 name[32], +40 mask[32], +72 rasterFormat,
// +76 d3dFormat (9) | hasAlpha (8), +80 width u16, +82 height u16, +84 depth,
// +85 numLevels, +86 rasterType, +87 flags (9: bit3 compressed) | dxt type (8),
// +88 palette (PAL8: 256 x 4), then per level { uint32 size, bytes }.
bool ParseTxd(const uint8_t* data, size_t size, Image& img) {
    const uint8_t* end = data + size;
    Chunk dict;
    if (!ReadChunk(data, end, dict) || dict.type != kChunkTexDictionary) return false;
    const uint8_t* p = dict.data;
    const uint8_t* dictEnd = dict.data + dict.size;
    Chunk c;
    if (!ReadChunk(p, dictEnd, c) || c.type != kChunkStruct) return false;
    p = c.data + c.size;
    if (!ReadChunk(p, dictEnd, c) || c.type != kChunkTextureNative) return false;
    const uint8_t* texEnd = c.data + c.size;
    Chunk st;
    if (!ReadChunk(c.data, texEnd, st) || st.type != kChunkStruct || st.size < 88) return false;
    const uint8_t* s = st.data;
    const uint8_t* sEnd = st.data + st.size;

    uint32_t d3dFormat = 0; uint16_t w = 0, h = 0;
    memcpy(&img.platform, s, 4);
    memcpy(&img.rasterFormat, s + 72, 4);
    memcpy(&d3dFormat, s + 76, 4);
    memcpy(&w, s + 80, 2);
    memcpy(&h, s + 82, 2);
    const uint8_t numLevels = s[85];
    const uint8_t flags = s[87];
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || numLevels == 0) return false;
    img.width = w;
    img.height = h;
    img.palette8 = (img.rasterFormat & 0x2000) != 0;
    if (img.rasterFormat & 0x4000) return false;   // PAL4: not used by radar tiles

    if (img.platform == 9) {
        img.format = static_cast<D3DFORMAT>(d3dFormat);
        (void)flags;
    } else if (img.platform == 8) {
        const uint8_t dxt = flags;
        if (dxt == 1) img.format = D3DFMT_DXT1;
        else if (dxt == 3) img.format = D3DFMT_DXT3;
        else if (dxt == 5) img.format = D3DFMT_DXT5;
        else switch (img.rasterFormat & 0xF00) {
            case 0x500: img.format = D3DFMT_A8R8G8B8; break;
            case 0x600: img.format = D3DFMT_X8R8G8B8; break;
            case 0x200: img.format = D3DFMT_R5G6B5;   break;
            case 0x100: img.format = D3DFMT_A1R5G5B5; break;
            case 0x300: img.format = D3DFMT_A4R4G4B4; break;
            case 0xA00: img.format = D3DFMT_X1R5G5B5; break;
            default: return false;
        }
    } else {
        return false;
    }
    if (img.palette8) img.format = D3DFMT_A8R8G8B8;   // decoded via the palette
    if (!img.palette8 && !BlockBytes(img.format) && !PixelBytes(img.format)) return false;

    const uint8_t* q = s + 88;
    if (img.palette8) {
        if (sEnd - q < 1024) return false;
        img.palette = q;
        q += 1024;
    }
    int lw = img.width, lh = img.height;
    for (int i = 0; i < numLevels && i < 16; ++i) {
        if (sEnd - q < 4) break;
        uint32_t bytes = 0;
        memcpy(&bytes, q, 4);
        q += 4;
        const uint32_t need = LevelBytes(img.format, img.palette8, lw, lh);
        if (bytes < need || bytes > static_cast<size_t>(sEnd - q)) break;
        img.level[i] = q;
        img.levelSize[i] = bytes;
        img.levels = i + 1;
        q += bytes;
        if (lw == 1 && lh == 1) break;
        lw = (std::max)(1, lw / 2);
        lh = (std::max)(1, lh / 2);
    }
    return img.levels > 0;
}

bool FillLevel(IDirect3DTexture9* tex, const Image& img, int level) {
    const int lw = (std::max)(1, img.width >> level);
    const int lh = (std::max)(1, img.height >> level);
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(level, &lr, nullptr, 0))) return false;
    const uint8_t* src = img.level[level];
    auto* dst = static_cast<uint8_t*>(lr.pBits);
    if (img.palette8) {
        for (int y = 0; y < lh; ++y) {
            auto* row = reinterpret_cast<uint8_t*>(dst + static_cast<size_t>(y) * lr.Pitch);
            for (int x = 0; x < lw; ++x) {
                const uint8_t* c = img.palette + src[static_cast<size_t>(y) * lw + x] * 4;  // RGBA
                row[x * 4 + 0] = c[2];   // B
                row[x * 4 + 1] = c[1];   // G
                row[x * 4 + 2] = c[0];   // R
                row[x * 4 + 3] = c[3];   // A
            }
        }
    } else if (const uint32_t bb = BlockBytes(img.format)) {
        const int rows = (std::max)(1, (lh + 3) / 4);
        const size_t rowBytes = static_cast<size_t>((std::max)(1, (lw + 3) / 4)) * bb;
        for (int y = 0; y < rows; ++y)
            memcpy(dst + static_cast<size_t>(y) * lr.Pitch, src + y * rowBytes, rowBytes);
    } else {
        const size_t rowBytes = static_cast<size_t>(lw) * PixelBytes(img.format);
        for (int y = 0; y < lh; ++y)
            memcpy(dst + static_cast<size_t>(y) * lr.Pitch, src + y * rowBytes, rowBytes);
    }
    tex->UnlockRect(level);
    return true;
}

IDirect3DTexture9* CreateTexture(const Image& img) {
    if (!g_device) return nullptr;
    IDirect3DTexture9* tex = nullptr;
    if (!g_poolDefault) {
        if (SUCCEEDED(g_device->CreateTexture(img.width, img.height, img.levels, 0, img.format,
                                              D3DPOOL_MANAGED, &tex, nullptr))) {
            for (int i = 0; i < img.levels; ++i)
                if (!FillLevel(tex, img, i)) { tex->Release(); return nullptr; }
            return tex;
        }
        // D3D9Ex devices reject D3DPOOL_MANAGED; use DEFAULT + staging from here on.
        g_poolDefault = true;
        Log("D3DPOOL_MANAGED rejected; using D3DPOOL_DEFAULT (released on Reset)");
    }
    IDirect3DTexture9* staging = nullptr;
    if (FAILED(g_device->CreateTexture(img.width, img.height, img.levels, 0, img.format,
                                       D3DPOOL_SYSTEMMEM, &staging, nullptr)))
        return nullptr;
    bool ok = true;
    for (int i = 0; i < img.levels && ok; ++i) ok = FillLevel(staging, img, i);
    if (ok && SUCCEEDED(g_device->CreateTexture(img.width, img.height, img.levels, 0, img.format,
                                                D3DPOOL_DEFAULT, &tex, nullptr))) {
        if (FAILED(g_device->UpdateTexture(staging, tex))) { tex->Release(); tex = nullptr; }
    }
    staging->Release();
    return tex;
}

void LoadTile(int index, std::vector<uint8_t>& buffer) {
    Tile& t = g_tiles[index];
    t.attempted = true;
    const ImgEntry& e = g_entries[index];
    if (!e.found || e.bytes == 0 || e.bytes > kMaxTxdBytes) return;
    buffer.resize(e.bytes);
    if (!ReadAt(static_cast<uint64_t>(e.offset) * kSector, buffer.data(), e.bytes)) {
        Log("radar%02d: read failed", index);
        return;
    }
    Image img;
    if (!ParseTxd(buffer.data(), buffer.size(), img)) {
        Log("radar%02d: unsupported or malformed texture", index);
        return;
    }
    if (!g_loggedFirst) {
        g_loggedFirst = true;
        Log("first tile radar%02d: platform=%u rasterFormat=0x%X format=0x%X %dx%d levels=%d pal8=%d",
            index, img.platform, img.rasterFormat, static_cast<unsigned>(img.format),
            img.width, img.height, img.levels, img.palette8 ? 1 : 0);
    }
    t.tex = CreateTexture(img);
    if (!t.tex) {
        Log("radar%02d: CreateTexture failed (format 0x%X)", index, static_cast<unsigned>(img.format));
        return;
    }
    t.uInset = 0.5f / static_cast<float>(img.width);
    t.vInset = 0.5f / static_cast<float>(img.height);
    ++g_loaded;
}

void ReleaseAll() {
    for (Tile& t : g_tiles) {
        if (t.tex) t.tex->Release();
        t = Tile{};
    }
    g_next = 0;
    g_loaded = 0;
}

// Tile sampler state for the ImGui DX9 backend draw: the backend already
// clamps addressing; add mip filtering for zoomed-out tiles. The backend's
// D3D9 state block (pause_renderer Render) restores GTA's state afterwards.
void SetTileSampler(const ImDrawList*, const ImDrawCmd*) {
    if (!g_device) return;
    g_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
}

bool ReadPose(float& x, float& y, float& fx, float& fy) {
    __try {
        uint8_t* entity = reinterpret_cast<FindPlayerVehicleFn>(kFindPlayerVehicle)(-1, false);
        if (!entity) entity = *reinterpret_cast<uint8_t**>(kPlayersPed);
        if (!entity) return false;
        auto* matrix = *reinterpret_cast<float**>(entity + 0x14);   // CPlaceable::m_matrix
        if (matrix) {
            x = matrix[12]; y = matrix[13];                          // pos @0x30
            fx = matrix[4]; fy = matrix[5];                          // forward @0x10
        } else {
            auto* p = reinterpret_cast<float*>(entity + 0x4);        // CSimpleTransform
            x = p[0]; y = p[1];
            fx = -std::sin(p[3]); fy = std::cos(p[3]);
        }
        return std::isfinite(x) && std::isfinite(y);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void Update(IDirect3DDevice9* device) {
    if (!device) return;
    if (g_device != device) {
        ReleaseAll();
        g_device = device;
    }
    if (g_failed || g_next >= kTileCount) {
        if (g_next >= kTileCount) CloseImg();
        return;
    }
    if (!g_indexed && !OpenAndIndex()) return;
    if (g_img == INVALID_HANDLE_VALUE) {
        // Re-loading after a device change / DEFAULT-pool reset.
        g_indexed = false;
        if (!OpenAndIndex()) return;
    }
    std::vector<uint8_t> buffer;
    for (int i = 0; i < kTilesPerUpdate && g_next < kTileCount; ++i)
        LoadTile(g_next++, buffer);
    if (g_next >= kTileCount) {
        Log("tiles loaded: %d / %d", g_loaded, kTileCount);
        CloseImg();
    }
}

void OnDeviceChanged() {
    ReleaseAll();
    g_device = nullptr;
}

void OnBeforeReset() {
    // MANAGED textures survive Reset; DEFAULT-pool ones must be released.
    if (g_poolDefault) ReleaseAll();
}

void Shutdown() {
    ReleaseAll();
    CloseImg();
    g_device = nullptr;
}

int LoadedTiles() { return g_loaded; }
bool Finished() { return g_failed || g_next >= kTileCount; }
const char* Error() { return g_failed ? g_error : nullptr; }

ImVec2 WorldToScreen(ImVec2 min, ImVec2 max, float cx, float cy, float zoom, float wx, float wy) {
    const float mx = (min.x + max.x) * 0.5f;
    const float my = (min.y + max.y) * 0.5f;
    return ImVec2(mx + (wx - cx) * zoom, my - (wy - cy) * zoom);   // north up
}

void DrawTiles(ImDrawList* dl, ImVec2 min, ImVec2 max, float cx, float cy, float zoom) {
    constexpr float kTileWorld = kWorldSize / kTilesPerSide;   // 500
    dl->AddCallback(&SetTileSampler, nullptr);
    for (int ty = 0; ty < kTilesPerSide; ++ty) {
        for (int tx = 0; tx < kTilesPerSide; ++tx) {
            const float wx0 = kWorldMin + tx * kTileWorld;
            const float wy0 = kWorldMax - ty * kTileWorld;           // northern edge
            ImVec2 a = WorldToScreen(min, max, cx, cy, zoom, wx0, wy0);
            ImVec2 b = WorldToScreen(min, max, cx, cy, zoom, wx0 + kTileWorld, wy0 - kTileWorld);
            // Shared, rounded edges: no gaps between neighbouring tiles.
            a = ImVec2(std::floor(a.x + 0.5f), std::floor(a.y + 0.5f));
            b = ImVec2(std::floor(b.x + 0.5f), std::floor(b.y + 0.5f));
            if (b.x < min.x || a.x > max.x || b.y < min.y || a.y > max.y) continue;
            const Tile& t = g_tiles[tx + kTilesPerSide * ty];
            if (t.tex)
                dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t.tex)), a, b,
                             ImVec2(t.uInset, t.vInset), ImVec2(1.0f - t.uInset, 1.0f - t.vInset));
            else
                dl->AddRectFilled(a, b, IM_COL32(16, 24, 33, 255));
        }
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

bool GetPlayerPosition(float& x, float& y, float& z) {
    __try {
        uint8_t* entity = reinterpret_cast<FindPlayerVehicleFn>(kFindPlayerVehicle)(-1, false);
        if (!entity) entity = *reinterpret_cast<uint8_t**>(kPlayersPed);
        if (!entity) return false;
        auto* matrix = *reinterpret_cast<float**>(entity + 0x14);   // CPlaceable::m_matrix
        const float* p = matrix ? matrix + 12 : reinterpret_cast<float*>(entity + 0x4);
        x = p[0]; y = p[1]; z = p[2];
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GetPlayerPose(float& x, float& y, float& fx, float& fy) {
    if (!ReadPose(x, y, fx, fy)) return false;
    const float len = std::sqrt(fx * fx + fy * fy);
    if (len > 0.0001f) { fx /= len; fy /= len; } else { fx = 0.0f; fy = 1.0f; }
    return true;
}

// ── Waypoint management via GTA CRadar native API ───────────────────
// GTA SA 1.0 US. Addresses, enums and layout verified against
// plugin-sdk game_sa/CRadar.h/.cpp and CMenuManager.h. Writes go only
// through CRadar functions plus GTA's own m_nTargetBlipIndex slot.
namespace waypoint {

constexpr uintptr_t kRadarTraces  = 0xBA86F0;   // CRadar::ms_RadarTrace[175]
constexpr int       kMaxTraces    = 175;
constexpr int       kTraceSize    = 0x28;       // VALIDATE_SIZE(tRadarTrace)
constexpr int       kOffPos       = 0x08;       // m_vecPos
constexpr int       kOffSprite    = 0x24;       // m_nRadarSprite
constexpr int       kOffFlags     = 0x25;       // bit1 = m_bInUse
constexpr uint8_t   kSpriteWaypoint = 41;       // RADAR_SPRITE_WAYPOINT
constexpr int       kBlipCoord      = 4;        // BLIP_COORD
constexpr int       kDisplayBlipOnly = 2;       // BLIP_DISPLAY_BLIP_ONLY (no 3D checkpoint)

// int __cdecl CRadar::SetCoordBlip(eBlipType, CVector, uint, eBlipDisplay, char*) 0x583820
using SetCoordBlipFn = int(__cdecl*)(int type, float x, float y, float z,
                                      unsigned int arg2, int display, const char* scriptName);
constexpr uintptr_t kSetCoordBlip = 0x583820;

// void __cdecl CRadar::SetBlipSprite(int handle, int spriteId) 0x583D70
using SetBlipSpriteFn = void(__cdecl*)(int handle, int sprite);
constexpr uintptr_t kSetBlipSprite = 0x583D70;

// FrontEndMenuManager.m_nTargetBlipIndex (CMenuManager +0x2C): GTA's own
// waypoint handle slot, so the native map and ours share one waypoint.
constexpr uintptr_t kTargetBlipIndex = 0xBA6748 + 0x2C;

int FindWaypointTraceIndex() {
    __try {
        auto* base = reinterpret_cast<const uint8_t*>(kRadarTraces);
        for (int i = 0; i < kMaxTraces; ++i) {
            const uint8_t* trace = base + i * kTraceSize;
            if ((trace[kOffFlags] & 0x02) && trace[kOffSprite] == kSpriteWaypoint)
                return i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

void ClearAllNative() {
    skr::gps::ClearMapWaypoint();
}

} // namespace waypoint

void SetWaypoint(float worldX, float worldY) {
    __try {
        waypoint::ClearAllNative();
        auto setCoord = reinterpret_cast<waypoint::SetCoordBlipFn>(waypoint::kSetCoordBlip);
        const int handle = setCoord(waypoint::kBlipCoord, worldX, worldY, 0.0f, 0,
                                    waypoint::kDisplayBlipOnly, nullptr);
        if (handle != -1 && handle != 0) {
            reinterpret_cast<waypoint::SetBlipSpriteFn>(waypoint::kSetBlipSprite)(
                handle, waypoint::kSpriteWaypoint);
            *reinterpret_cast<int*>(waypoint::kTargetBlipIndex) = handle;
        }
        Log("waypoint set at %.0f, %.0f (handle=0x%X)", worldX, worldY,
            static_cast<unsigned>(handle));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("waypoint set failed (exception)");
    }
}

void ClearWaypoint() {
    __try {
        waypoint::ClearAllNative();
        Log("waypoint cleared");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("waypoint clear failed (exception)");
    }
}

bool HasWaypoint() {
    return waypoint::FindWaypointTraceIndex() >= 0;
}

void GetWaypointPos(float& wx, float& wy) {
    __try {
        const int idx = waypoint::FindWaypointTraceIndex();
        if (idx >= 0) {
            const auto* trace = reinterpret_cast<const uint8_t*>(
                waypoint::kRadarTraces + idx * waypoint::kTraceSize);
            wx = *reinterpret_cast<const float*>(trace + waypoint::kOffPos);
            wy = *reinterpret_cast<const float*>(trace + waypoint::kOffPos + 4);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    wx = wy = 0.0f;
}

void ScreenToWorld(ImVec2 min, ImVec2 max, float cx, float cy, float zoom,
                   float sx, float sy, float& wx, float& wy) {
    const float mx = (min.x + max.x) * 0.5f;
    const float my = (min.y + max.y) * 0.5f;
    wx = cx + (sx - mx) / zoom;
    wy = cy - (sy - my) / zoom;
}

} // namespace skr::native_pause::map
