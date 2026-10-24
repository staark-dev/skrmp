// ─────────────────────────────────────────────────────────────────────
// SKR Virtual Filesystem — TEST_ONLY SA-MP Resource Redirection
// ─────────────────────────────────────────────────────────────────────
// See samp_vfs.h for the full specification and constraints.
//
// Hook mechanism: x86 inline hooks (5-byte JMP rel32) on kernel32/
// kernelbase function entries, with a VirtualAlloc'd trampoline that
// holds the displaced prologue bytes plus a JMP back.
//
// Reentrancy: a thread-local guard prevents infinite recursion when
// hook functions call hooked APIs (e.g., VfsLog → CreateFileW).
//
// Thread safety: Install/Remove are single-threaded (called from
// PreloadSamp under g_loadMutex). The hooks themselves are reentrant
// and use no shared mutable state beyond the atomic trace counters.
// ─────────────────────────────────────────────────────────────────────

#include "vfs/samp_vfs.h"

#ifndef SKR_TEST_CERTIFICATION
#error "samp_vfs.cpp must only be compiled in TEST_ONLY builds"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cwchar>
#include <algorithm>

namespace skr::vfs {
namespace {

// ═══════════════════════════════════════════════════════════════════════
// Configuration & State
// ═══════════════════════════════════════════════════════════════════════

static VfsConfig g_config;
static bool g_installed = false;

// Reentrancy guard: prevents hook functions from recursing when they
// call hooked APIs (logging, GetFullPathNameW, GetFileAttributesW).
static __declspec(thread) int g_reentrancyGuard = 0;

struct ReentrancyScope {
    ReentrancyScope()  { ++g_reentrancyGuard; }
    ~ReentrancyScope() { --g_reentrancyGuard; }
    bool IsReentrant() const { return g_reentrancyGuard > 1; }
};

// ═══════════════════════════════════════════════════════════════════════
// Trace Counters
// ═══════════════════════════════════════════════════════════════════════

static std::atomic<int> g_traceTotal{0};
static std::atomic<int> g_traceRedirected{0};
static std::atomic<int> g_tracePassthrough{0};

// ═══════════════════════════════════════════════════════════════════════
// Logging
// ═══════════════════════════════════════════════════════════════════════

// Reuse samp_loader's log directory convention.
static std::wstring VfsLogDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&VfsLogDir), &self);
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, p, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(p, n);
    auto slash = s.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return s.substr(0, slash + 1) + L"SKR\\";
}

static void VfsLog(const char* fmt, ...) {
    // NOTE: This function is called from within hooks. The reentrancy
    // guard must already be held by the caller so that the CreateFileW
    // inside _wfopen does not recurse back into our hook.
    std::wstring dir = VfsLogDir();
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = _wfopen((dir + L"samp_vfs.log").c_str(), L"a");
    if (!f) return;
    fprintf(f, "[%lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ═══════════════════════════════════════════════════════════════════════
// Known SA-MP Resource Files
// ═══════════════════════════════════════════════════════════════════════
// These are the resource files that SA-MP R5 may look for during
// initialization (DllMain + Samp_MainThread). Paths are relative to
// the GTA directory or the samp.dll directory.
//
// The list is intentionally broad for the initial trace pass. After
// examining the VFS log, it can be narrowed to only the files that
// SA-MP actually accesses during DllMain.

static const wchar_t* kSampResourceFiles[] = {
    // SA-MP data subdirectory — every file in the bundled SAMP\ folder
    L"SAMP\\SAMP.img",
    L"SAMP\\SAMP.ide",
    L"SAMP\\SAMP.ipl",
    L"SAMP\\SAMP.ifp",
    L"SAMP\\SAMPCOL.img",
    L"SAMP\\custom.img",
    L"SAMP\\CUSTOM.ide",
    L"SAMP\\samaps.txd",
    L"SAMP\\blanktex.txd",
    // Root-level SA-MP resources
    L"samp.saa",
    L"gtaweap3.ttf",
    L"sampaux3.ttf",
    L"sampgui.png",
    L"mouse.png",
    // SA-MP audio
    L"bass.dll",
    nullptr
};

// ═══════════════════════════════════════════════════════════════════════
// Path Matching & Redirection
// ═══════════════════════════════════════════════════════════════════════

// Case-insensitive wide-string comparison.
static bool WcsEqualI(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

// Check if `path` (absolute) is under `dir` (no trailing backslash).
// Returns the relative portion (after the backslash), or nullptr.
static const wchar_t* RelativeUnder(const wchar_t* path, const wchar_t* dir, size_t dirLen) {
    if (_wcsnicmp(path, dir, dirLen) != 0) return nullptr;
    if (path[dirLen] == L'\\') return path + dirLen + 1;
    if (path[dirLen] == L'\0') return path + dirLen;  // exact match
    return nullptr;
}

// Try to redirect a file path to the bundled SA-MP resource directory.
// Returns the redirected path, or an empty string if no redirection.
//
// IMPORTANT: This function must NOT call any hooked API without the
// reentrancy guard already held. It calls GetFullPathNameW (not hooked)
// and performs string comparisons only.
static std::wstring TryRedirect(const wchar_t* requestedPath) {
    if (!requestedPath || !*requestedPath) return {};

    // Resolve to absolute path (GetFullPathNameW is not hooked).
    wchar_t resolved[MAX_PATH]{};
    DWORD len = GetFullPathNameW(requestedPath, MAX_PATH, resolved, nullptr);
    if (!len || len >= MAX_PATH) return {};

    // Check if the resolved path is under the GTA directory.
    const wchar_t* relative = RelativeUnder(
        resolved, g_config.gtaDir.c_str(), g_config.gtaDir.size());
    if (!relative || !*relative) return {};

    // Check against known SA-MP resource file names.
    for (const wchar_t** res = kSampResourceFiles; *res; ++res) {
        if (WcsEqualI(relative, *res)) {
            // Build the redirected path: <bundled_samp_dir>\<relative>
            std::wstring redirected = g_config.sampResourceDir + relative;

            // Verify the file actually exists at the redirected location.
            // We call the REAL GetFileAttributesW via the trampoline to
            // avoid recursion. But since the reentrancy guard is already
            // held by the caller, even if we hit our hook it will pass
            // through immediately.
            DWORD attrs = GetFileAttributesW(redirected.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                return redirected;
            }
            // File not in bundled dir — fall through to original path.
            break;
        }
    }

    return {};
}

// ═══════════════════════════════════════════════════════════════════════
// x86 Inline Hook Infrastructure
// ═══════════════════════════════════════════════════════════════════════

// Minimal x86 instruction length decoder for Windows API function
// prologues. Handles the instruction patterns commonly found at the
// entry points of kernel32/kernelbase exports on Windows 7–11 x86.
//
// This is NOT a complete disassembler. It recognizes enough opcodes to
// find the first instruction boundary at or after byte 5, which is the
// minimum for a JMP rel32 patch.
static int x86InsnLength(const uint8_t* code) {
    const uint8_t op = code[0];

    // ── Single-byte instructions ──
    // push/pop reg (50-5F), nop (90), ret (C3), int3 (CC), cdq (99)
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90 || op == 0xC3 ||
        op == 0xCC || op == 0x99)
        return 1;

    // ── Two-byte: ModR/M with register-register encoding ──
    if (op == 0x8B || op == 0x89 || op == 0x33 || op == 0x3B ||
        op == 0x85 || op == 0x87 || op == 0x0B || op == 0x23 ||
        op == 0x2B || op == 0x31 || op == 0x29 || op == 0x01) {
        uint8_t modrm = code[1];
        uint8_t mod = modrm >> 6;
        uint8_t rm  = modrm & 7;
        if (mod == 3) return 2;  // reg, reg
        if (mod == 0) {
            if (rm == 5) return 6;  // [disp32]
            if (rm == 4) return 3;  // [SIB]
            return 2;
        }
        if (mod == 1) {
            if (rm == 4) return 4;  // [SIB+disp8]
            return 3;  // [reg+disp8]
        }
        if (mod == 2) {
            if (rm == 4) return 7;  // [SIB+disp32]
            return 6;  // [reg+disp32]
        }
    }

    // ── push imm8 ──
    if (op == 0x6A) return 2;

    // ── sub/and/cmp/add reg, imm8 (83 /r ib) ──
    if (op == 0x83 || op == 0x80) {
        uint8_t modrm = code[1];
        uint8_t mod = modrm >> 6;
        if (mod == 3) return 3;
    }

    // ── sub/add reg, imm32 (81 /r id) ──
    if (op == 0x81) {
        uint8_t modrm = code[1];
        uint8_t mod = modrm >> 6;
        if (mod == 3) return 6;
    }

    // ── mov reg, imm32 (B8+rd) ──
    if (op >= 0xB8 && op <= 0xBF) return 5;

    // ── push imm32 ──
    if (op == 0x68) return 5;

    // ── jmp/call rel32 ──
    if (op == 0xE9 || op == 0xE8) return 5;

    // ── jmp [imm32] / call [imm32] (FF /4 or FF /2 with mod=0, rm=5) ──
    if (op == 0xFF) {
        uint8_t modrm = code[1];
        uint8_t mod = modrm >> 6;
        uint8_t rm  = modrm & 7;
        if (mod == 0 && rm == 5) return 6;  // jmp/call [disp32]
    }

    // ── mov eax, [imm32] (A1) ──
    if (op == 0xA1) return 5;

    // ── lea reg, [reg+disp8] (8D /r) ──
    if (op == 0x8D) {
        uint8_t modrm = code[1];
        uint8_t mod = modrm >> 6;
        uint8_t rm  = modrm & 7;
        if (mod == 1) {
            if (rm == 4) return 4;
            return 3;
        }
        if (mod == 0 && rm == 5) return 6;
    }

    // ── ret imm16 (C2 iw) ──
    if (op == 0xC2) return 3;

    // ── Two-byte opcode prefix (0F ...) ──
    if (op == 0x0F) {
        uint8_t op2 = code[1];
        // jcc rel32 (0F 80-8F)
        if (op2 >= 0x80 && op2 <= 0x8F) return 6;
        // movzx/movsx (0F B6/B7/BE/BF) — ModR/M follows
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
            uint8_t modrm = code[2];
            uint8_t mod = modrm >> 6;
            if (mod == 3) return 3;
        }
    }

    return 0;  // Unknown — can't hook this function safely
}

struct InlineHook {
    void*    target      = nullptr;  // Original function address
    void*    detour      = nullptr;  // Our replacement function
    void*    trampoline  = nullptr;  // Allocated executable memory for displaced prologue
    uint8_t  savedBytes[32]{};       // Original bytes (for unhooking)
    int      patchSize   = 0;        // Number of bytes overwritten (>= 5)
    bool     active      = false;
};

static bool InstallInlineHook(InlineHook* hook) {
    const auto* code = static_cast<const uint8_t*>(hook->target);

    // Walk instructions to find a boundary at or past byte 5.
    int total = 0;
    while (total < 5) {
        int len = x86InsnLength(code + total);
        if (len == 0) {
            VfsLog("hook install FAILED: unknown x86 opcode 0x%02X at offset %d of target %p",
                   code[total], total, hook->target);
            return false;
        }
        total += len;
        if (total > 30) {
            VfsLog("hook install FAILED: prologue too long at target %p", hook->target);
            return false;
        }
    }
    hook->patchSize = total;

    // Allocate trampoline: displaced prologue + JMP back.
    hook->trampoline = VirtualAlloc(nullptr, total + 5,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
    if (!hook->trampoline) {
        VfsLog("hook install FAILED: VirtualAlloc for trampoline (%p)", hook->target);
        return false;
    }

    // Save original bytes.
    memcpy(hook->savedBytes, code, total);

    // Build trampoline: original prologue bytes + JMP back to original+patchSize.
    auto* tram = static_cast<uint8_t*>(hook->trampoline);
    memcpy(tram, code, total);
    tram[total] = 0xE9;  // JMP rel32
    auto jmpTarget = reinterpret_cast<intptr_t>(code) + total;
    auto jmpFrom   = reinterpret_cast<intptr_t>(tram) + total + 5;
    int32_t rel = static_cast<int32_t>(jmpTarget - jmpFrom);
    memcpy(tram + total + 1, &rel, 4);

    // Patch original function: JMP to our detour + NOP padding.
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook->target, total, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VfsLog("hook install FAILED: VirtualProtect on target %p", hook->target);
        VirtualFree(hook->trampoline, 0, MEM_RELEASE);
        hook->trampoline = nullptr;
        return false;
    }

    auto* patch = static_cast<uint8_t*>(hook->target);
    patch[0] = 0xE9;  // JMP rel32
    auto detourAddr = reinterpret_cast<intptr_t>(hook->detour);
    auto patchFrom  = reinterpret_cast<intptr_t>(hook->target) + 5;
    int32_t detourRel = static_cast<int32_t>(detourAddr - patchFrom);
    memcpy(patch + 1, &detourRel, 4);
    for (int i = 5; i < total; ++i) patch[i] = 0x90;  // NOP

    VirtualProtect(hook->target, total, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), hook->target, total);

    hook->active = true;
    VfsLog("hook installed: target=%p trampoline=%p patchSize=%d", hook->target, hook->trampoline, total);
    return true;
}

static void RemoveInlineHook(InlineHook* hook) {
    if (!hook->active) return;

    DWORD oldProtect = 0;
    if (VirtualProtect(hook->target, hook->patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(hook->target, hook->savedBytes, hook->patchSize);
        VirtualProtect(hook->target, hook->patchSize, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), hook->target, hook->patchSize);
    }

    VirtualFree(hook->trampoline, 0, MEM_RELEASE);
    hook->trampoline = nullptr;
    hook->active = false;
}

// ═══════════════════════════════════════════════════════════════════════
// Hook Instances
// ═══════════════════════════════════════════════════════════════════════

// Function typedefs for the originals (called via trampoline).
using CreateFileW_fn       = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_fn       = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetFileAttributesW_fn = DWORD (WINAPI*)(LPCWSTR);
using GetFileAttributesA_fn = DWORD (WINAPI*)(LPCSTR);
using FindFirstFileExW_fn  = HANDLE (WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);

static InlineHook g_hookCreateFileW;
static InlineHook g_hookCreateFileA;
static InlineHook g_hookGetFileAttributesW;
static InlineHook g_hookGetFileAttributesA;
static InlineHook g_hookFindFirstFileExW;

// Trampoline accessors (cast the trampoline pointer to the correct type).
#define REAL(hook, type) reinterpret_cast<type>((hook).trampoline)

// ═══════════════════════════════════════════════════════════════════════
// Hook Functions
// ═══════════════════════════════════════════════════════════════════════

static HANDLE WINAPI Hook_CreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSA, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplate)
{
    ReentrancyScope scope;
    if (scope.IsReentrant()) {
        return REAL(g_hookCreateFileW, CreateFileW_fn)(
            lpFileName, dwDesiredAccess, dwShareMode, lpSA,
            dwCreationDisposition, dwFlagsAndAttributes, hTemplate);
    }

    g_traceTotal.fetch_add(1, std::memory_order_relaxed);

    std::wstring redirected = TryRedirect(lpFileName);
    if (!redirected.empty()) {
        g_traceRedirected.fetch_add(1, std::memory_order_relaxed);
        if (g_config.traceAll)
            VfsLog("CreateFileW REDIRECT: %ls -> %ls", lpFileName, redirected.c_str());
        return REAL(g_hookCreateFileW, CreateFileW_fn)(
            redirected.c_str(), dwDesiredAccess, dwShareMode, lpSA,
            dwCreationDisposition, dwFlagsAndAttributes, hTemplate);
    }

    g_tracePassthrough.fetch_add(1, std::memory_order_relaxed);

    HANDLE result = REAL(g_hookCreateFileW, CreateFileW_fn)(
        lpFileName, dwDesiredAccess, dwShareMode, lpSA,
        dwCreationDisposition, dwFlagsAndAttributes, hTemplate);
    DWORD err = (result == INVALID_HANDLE_VALUE) ? GetLastError() : 0;

    if (g_config.traceAll) {
        // Only log files under the GTA directory to keep the trace manageable.
        wchar_t resolved[MAX_PATH]{};
        DWORD len = GetFullPathNameW(lpFileName, MAX_PATH, resolved, nullptr);
        if (len && len < MAX_PATH) {
            if (_wcsnicmp(resolved, g_config.gtaDir.c_str(), g_config.gtaDir.size()) == 0) {
                if (result == INVALID_HANDLE_VALUE)
                    VfsLog("CreateFileW PASS: %ls -> FAILED (error %lu)", lpFileName, err);
                else
                    VfsLog("CreateFileW PASS: %ls -> OK (handle=%p)", lpFileName, result);
            }
        }
    }

    if (result == INVALID_HANDLE_VALUE) SetLastError(err);
    return result;
}

static HANDLE WINAPI Hook_CreateFileA(
    LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSA, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplate)
{
    ReentrancyScope scope;
    if (scope.IsReentrant()) {
        return REAL(g_hookCreateFileA, CreateFileA_fn)(
            lpFileName, dwDesiredAccess, dwShareMode, lpSA,
            dwCreationDisposition, dwFlagsAndAttributes, hTemplate);
    }

    g_traceTotal.fetch_add(1, std::memory_order_relaxed);

    // Convert ANSI to wide for path matching.
    if (lpFileName) {
        wchar_t wide[MAX_PATH]{};
        int n = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide, MAX_PATH);
        if (n > 0) {
            std::wstring redirected = TryRedirect(wide);
            if (!redirected.empty()) {
                g_traceRedirected.fetch_add(1, std::memory_order_relaxed);
                if (g_config.traceAll)
                    VfsLog("CreateFileA REDIRECT: %s -> %ls", lpFileName, redirected.c_str());
                // Call the W version with the redirected path since it's already wide.
                return REAL(g_hookCreateFileW, CreateFileW_fn)(
                    redirected.c_str(), dwDesiredAccess, dwShareMode, lpSA,
                    dwCreationDisposition, dwFlagsAndAttributes, hTemplate);
            }

            if (g_config.traceAll) {
                wchar_t resolved[MAX_PATH]{};
                DWORD len = GetFullPathNameW(wide, MAX_PATH, resolved, nullptr);
                if (len && len < MAX_PATH) {
                    if (_wcsnicmp(resolved, g_config.gtaDir.c_str(), g_config.gtaDir.size()) == 0)
                        VfsLog("CreateFileA PASS: %s", lpFileName);
                }
            }
        }
    }

    g_tracePassthrough.fetch_add(1, std::memory_order_relaxed);
    return REAL(g_hookCreateFileA, CreateFileA_fn)(
        lpFileName, dwDesiredAccess, dwShareMode, lpSA,
        dwCreationDisposition, dwFlagsAndAttributes, hTemplate);
}

static DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR lpFileName) {
    ReentrancyScope scope;
    if (scope.IsReentrant())
        return REAL(g_hookGetFileAttributesW, GetFileAttributesW_fn)(lpFileName);

    g_traceTotal.fetch_add(1, std::memory_order_relaxed);

    std::wstring redirected = TryRedirect(lpFileName);
    if (!redirected.empty()) {
        g_traceRedirected.fetch_add(1, std::memory_order_relaxed);
        if (g_config.traceAll)
            VfsLog("GetFileAttributesW REDIRECT: %ls -> %ls", lpFileName, redirected.c_str());
        return REAL(g_hookGetFileAttributesW, GetFileAttributesW_fn)(redirected.c_str());
    }

    g_tracePassthrough.fetch_add(1, std::memory_order_relaxed);
    return REAL(g_hookGetFileAttributesW, GetFileAttributesW_fn)(lpFileName);
}

static DWORD WINAPI Hook_GetFileAttributesA(LPCSTR lpFileName) {
    ReentrancyScope scope;
    if (scope.IsReentrant())
        return REAL(g_hookGetFileAttributesA, GetFileAttributesA_fn)(lpFileName);

    g_traceTotal.fetch_add(1, std::memory_order_relaxed);

    if (lpFileName) {
        wchar_t wide[MAX_PATH]{};
        int n = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide, MAX_PATH);
        if (n > 0) {
            std::wstring redirected = TryRedirect(wide);
            if (!redirected.empty()) {
                g_traceRedirected.fetch_add(1, std::memory_order_relaxed);
                if (g_config.traceAll)
                    VfsLog("GetFileAttributesA REDIRECT: %s -> %ls", lpFileName, redirected.c_str());
                return REAL(g_hookGetFileAttributesW, GetFileAttributesW_fn)(redirected.c_str());
            }
        }
    }

    g_tracePassthrough.fetch_add(1, std::memory_order_relaxed);
    return REAL(g_hookGetFileAttributesA, GetFileAttributesA_fn)(lpFileName);
}

static HANDLE WINAPI Hook_FindFirstFileExW(
    LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId,
    LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp,
    LPVOID lpSearchFilter, DWORD dwAdditionalFlags)
{
    ReentrancyScope scope;
    if (scope.IsReentrant()) {
        return REAL(g_hookFindFirstFileExW, FindFirstFileExW_fn)(
            lpFileName, fInfoLevelId, lpFindFileData, fSearchOp,
            lpSearchFilter, dwAdditionalFlags);
    }

    g_traceTotal.fetch_add(1, std::memory_order_relaxed);

    // For FindFirstFile, we need to check if the search pattern targets
    // the SA-MP resource directory (e.g., "C:\GTA\SAMP\*").
    // Extract the directory part and check for redirection.
    if (lpFileName) {
        wchar_t resolved[MAX_PATH]{};
        DWORD len = GetFullPathNameW(lpFileName, MAX_PATH, resolved, nullptr);
        if (len && len < MAX_PATH) {
            const wchar_t* relative = RelativeUnder(
                resolved, g_config.gtaDir.c_str(), g_config.gtaDir.size());
            if (relative && *relative) {
                // Check if this is a search in the SAMP subdirectory.
                if (_wcsnicmp(relative, L"SAMP\\", 5) == 0 ||
                    _wcsicmp(relative, L"SAMP") == 0) {
                    // Redirect to the bundled SAMP directory.
                    std::wstring redirected = g_config.sampResourceDir + relative;
                    g_traceRedirected.fetch_add(1, std::memory_order_relaxed);
                    if (g_config.traceAll)
                        VfsLog("FindFirstFileExW REDIRECT: %ls -> %ls", lpFileName, redirected.c_str());
                    return REAL(g_hookFindFirstFileExW, FindFirstFileExW_fn)(
                        redirected.c_str(), fInfoLevelId, lpFindFileData,
                        fSearchOp, lpSearchFilter, dwAdditionalFlags);
                }
                if (g_config.traceAll)
                    VfsLog("FindFirstFileExW PASS (GTA-relative): %ls", lpFileName);
            }
        }
    }

    g_tracePassthrough.fetch_add(1, std::memory_order_relaxed);
    return REAL(g_hookFindFirstFileExW, FindFirstFileExW_fn)(
        lpFileName, fInfoLevelId, lpFindFileData, fSearchOp,
        lpSearchFilter, dwAdditionalFlags);
}

#undef REAL

// ═══════════════════════════════════════════════════════════════════════
// Resolve Target Function
// ═══════════════════════════════════════════════════════════════════════
// On Windows, kernel32!CreateFileW may be a stub that jumps to
// kernelbase!CreateFileW. We follow the indirection to hook the actual
// implementation so ALL callers (including other DLLs) are intercepted.

static void* FollowJmpStub(void* fn) {
    auto* code = static_cast<const uint8_t*>(fn);
    // FF 25 xx xx xx xx = jmp [imm32] (IAT-style thunk)
    if (code[0] == 0xFF && code[1] == 0x25) {
        void* target = **reinterpret_cast<void** const*>(code + 2);
        return target;
    }
    // E9 xx xx xx xx = jmp rel32
    if (code[0] == 0xE9) {
        int32_t rel = 0;
        memcpy(&rel, code + 1, 4);
        return reinterpret_cast<void*>(reinterpret_cast<intptr_t>(code) + 5 + rel);
    }
    return fn;  // Not a stub — return as-is.
}

static void* ResolveApiFunction(const char* module, const char* function) {
    HMODULE hMod = GetModuleHandleA(module);
    if (!hMod) return nullptr;
    void* fn = reinterpret_cast<void*>(GetProcAddress(hMod, function));
    if (!fn) return nullptr;
    // Follow one level of indirection (kernel32 → kernelbase).
    return FollowJmpStub(fn);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════

bool Install(const VfsConfig& config) {
    if (g_installed) return true;

    g_config = config;

    // Ensure trailing backslash on sampResourceDir.
    if (!g_config.sampResourceDir.empty() &&
        g_config.sampResourceDir.back() != L'\\')
        g_config.sampResourceDir += L'\\';

    // Remove trailing backslash from gtaDir for consistent comparison.
    while (!g_config.gtaDir.empty() && g_config.gtaDir.back() == L'\\')
        g_config.gtaDir.pop_back();

    VfsLog("=== SKR VFS Install ===");
    VfsLog("  GTA dir:  %ls", g_config.gtaDir.c_str());
    VfsLog("  SAMP dir: %ls", g_config.sampResourceDir.c_str());

    // Reset trace counters.
    g_traceTotal.store(0);
    g_traceRedirected.store(0);
    g_tracePassthrough.store(0);

    // Resolve function addresses.
    // We hook kernel32 exports. If they're stubs to kernelbase,
    // FollowJmpStub follows the indirection.
    auto* pCreateFileW      = ResolveApiFunction("kernel32.dll", "CreateFileW");
    auto* pCreateFileA      = ResolveApiFunction("kernel32.dll", "CreateFileA");
    auto* pGetFileAttrsW    = ResolveApiFunction("kernel32.dll", "GetFileAttributesW");
    auto* pGetFileAttrsA    = ResolveApiFunction("kernel32.dll", "GetFileAttributesA");
    auto* pFindFirstFileExW = ResolveApiFunction("kernel32.dll", "FindFirstFileExW");

    int installed = 0;
    int failed = 0;

    auto tryHook = [&](InlineHook& hook, void* target, void* detour, const char* name) {
        if (!target) {
            VfsLog("  %s: resolve FAILED (GetProcAddress returned null)", name);
            ++failed;
            return;
        }
        hook.target = target;
        hook.detour = detour;
        if (InstallInlineHook(&hook)) {
            ++installed;
        } else {
            ++failed;
        }
    };

    tryHook(g_hookCreateFileW,      pCreateFileW,      reinterpret_cast<void*>(&Hook_CreateFileW),      "CreateFileW");
    tryHook(g_hookCreateFileA,      pCreateFileA,      reinterpret_cast<void*>(&Hook_CreateFileA),      "CreateFileA");
    tryHook(g_hookGetFileAttributesW, pGetFileAttrsW,   reinterpret_cast<void*>(&Hook_GetFileAttributesW), "GetFileAttributesW");
    tryHook(g_hookGetFileAttributesA, pGetFileAttrsA,   reinterpret_cast<void*>(&Hook_GetFileAttributesA), "GetFileAttributesA");
    tryHook(g_hookFindFirstFileExW, pFindFirstFileExW, reinterpret_cast<void*>(&Hook_FindFirstFileExW), "FindFirstFileExW");

    VfsLog("  hooks: %d installed, %d failed", installed, failed);

    g_installed = (installed > 0);
    return g_installed;
}

void Remove() {
    if (!g_installed) return;

    VfsLog("=== SKR VFS Remove ===");
    LogTraceSummary();

    RemoveInlineHook(&g_hookCreateFileW);
    RemoveInlineHook(&g_hookCreateFileA);
    RemoveInlineHook(&g_hookGetFileAttributesW);
    RemoveInlineHook(&g_hookGetFileAttributesA);
    RemoveInlineHook(&g_hookFindFirstFileExW);

    g_installed = false;
    VfsLog("  all hooks removed");
}

bool IsInstalled() {
    return g_installed;
}

void LogTraceSummary() {
    VfsLog("  trace summary: total=%d redirected=%d passthrough=%d",
           g_traceTotal.load(), g_traceRedirected.load(), g_tracePassthrough.load());
}

} // namespace skr::vfs
