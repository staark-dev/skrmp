// PHONE-M4F — SKR Street Phone: wallpaper texture (see phone_wallpaper.h).
#include "phone/phone_wallpaper.h"
#include "phone/phone_renderer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace skr::phone::wallpaper {
namespace {

IDirect3DTexture9* g_tex = nullptr;
IDirect3DDevice9*  g_triedDevice = nullptr;

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool ReadFile(const wchar_t* path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n > 54 && n < 64 * 1024 * 1024;
    if (ok) {
        out.resize(static_cast<size_t>(n));
        ok = fread(out.data(), 1, out.size(), f) == out.size();
    }
    fclose(f);
    return ok;
}

// Decodes an uncompressed 24/32-bit BMP (BI_RGB, or BI_BITFIELDS with the
// standard BGRA masks) into top-down BGRA with opaque alpha for 24-bit.
bool DecodeBmp(const std::vector<uint8_t>& d, std::vector<uint32_t>& px, UINT& w, UINT& h) {
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
    const uint32_t off = U32(&d[10]);
    const int32_t sw = static_cast<int32_t>(U32(&d[18])), sh = static_cast<int32_t>(U32(&d[22]));
    const uint16_t bpp = U16(&d[28]);
    const uint32_t comp = U32(&d[30]);
    if (sw <= 0 || sh == 0 || sw > 4096 || sh > 4096 || sh < -4096) return false;
    if (!(bpp == 24 || bpp == 32) || !(comp == 0 || (comp == 3 && bpp == 32))) return false;
    w = static_cast<UINT>(sw);
    h = static_cast<UINT>(sh < 0 ? -sh : sh);
    const size_t stride = (static_cast<size_t>(w) * (bpp / 8) + 3) & ~size_t(3);
    if (off > d.size() || d.size() - off < stride * h) return false;
    px.resize(static_cast<size_t>(w) * h);
    for (UINT y = 0; y < h; ++y) {
        const UINT src = sh > 0 ? h - 1 - y : y;          // positive height = bottom-up
        const uint8_t* row = &d[off + stride * src];
        for (UINT x = 0; x < w; ++x) {
            const uint8_t* p = row + x * (bpp / 8);
            const uint32_t a = bpp == 32 ? p[3] : 255u;
            px[static_cast<size_t>(y) * w + x] = (a << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
        }
    }
    return true;
}

std::wstring DefaultPath() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DefaultPath), &self);
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1) + L"SKR\\phone\\wallpaper.bmp";
}

} // namespace

bool LoadFile(IDirect3DDevice9* device, const wchar_t* path) {
    Release();
    if (!device || !path || !*path) return false;
    std::vector<uint8_t> file;
    std::vector<uint32_t> px;
    UINT w = 0, h = 0;
    if (!ReadFile(path, file) || !DecodeBmp(file, px, w, h)) return false;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) return false;
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) { tex->Release(); return false; }
    for (UINT y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(lr.Pitch) * y, &px[static_cast<size_t>(y) * w], w * 4);
    tex->UnlockRect(0);
    g_tex = tex;
    SetWallpaperTexture(reinterpret_cast<void*>(tex), static_cast<float>(w), static_cast<float>(h));
    return true;
}

void Ensure(IDirect3DDevice9* device) {
    if (!device || device == g_triedDevice) return;
    const std::wstring path = DefaultPath();
    LoadFile(device, path.c_str());
    g_triedDevice = device;                   // after LoadFile (its Release() clears it)
}

void Release() {
    SetWallpaperTexture(nullptr, 0, 0);
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    g_triedDevice = nullptr;
}

} // namespace skr::phone::wallpaper
