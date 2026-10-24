#include "lobby/menu_patch.h"
#include "compatibility/hook_target_resolver.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace skr::lobby {

namespace {

constexpr uintptr_t kProcessAddress = 0x0057B440;
constexpr uint8_t kProcessSignature[] = {
    0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x5C, 0x84, 0xC0,
    0x74, 0x2A, 0x33, 0xC0, 0x8A, 0x46, 0x5B, 0x50,
};

uint8_t g_original = 0;
bool g_originalCaptured = false;
bool g_patched = false;

bool WriteByte(uintptr_t addr, uint8_t value) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 1, PAGE_EXECUTE_READWRITE, &old))
        return false;
    *reinterpret_cast<uint8_t*>(addr) = value;
    DWORD restore = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), 1, old, &restore);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), 1);
    return true;
}

// Confirms the address lives in a mapped, executable region before we touch it.
const char* ResolveProcessTarget() {
    auto* module = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!module) return "GTA module is unavailable";
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return "invalid DOS header";
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return "invalid PE header";
    if (reinterpret_cast<uintptr_t>(module) != nt->OptionalHeader.ImageBase ||
        nt->OptionalHeader.ImageBase != 0x00400000)
        return "unexpected GTA image base";
    if (nt->FileHeader.TimeDateStamp != 0x427101CA ||
        nt->OptionalHeader.SizeOfImage != 0x01177000)
        return "loaded GTA image does not match the Hoodlum PE profile";

    const auto* section = IMAGE_FIRST_SECTION(nt);
    const IMAGE_SECTION_HEADER* containing = nullptr;
    const DWORD targetRva = static_cast<DWORD>(kProcessAddress -
        reinterpret_cast<uintptr_t>(module));
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD size = max(section[i].Misc.VirtualSize, section[i].SizeOfRawData);
        if (targetRva >= section[i].VirtualAddress &&
            targetRva < section[i].VirtualAddress + size) {
            containing = &section[i];
            break;
        }
    }
    if (!containing) return "hook target is outside PE sections";

    const DWORD size = max(containing->Misc.VirtualSize, containing->SizeOfRawData);
    const bool executable = (containing->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    const auto result = compatibility::ResolveProfileHookTarget(
        module + containing->VirtualAddress, size,
        reinterpret_cast<uintptr_t>(module) + containing->VirtualAddress,
        executable, kProcessSignature, sizeof(kProcessSignature), kProcessAddress);
    switch (result.status) {
        case compatibility::HookResolveStatus::Resolved: return nullptr;
        case compatibility::HookResolveStatus::Missing: return "Hoodlum Process signature is missing";
        case compatibility::HookResolveStatus::Ambiguous: return "Hoodlum Process signature is ambiguous";
        case compatibility::HookResolveStatus::UnexpectedAddress: return "Hoodlum Process signature resolved at an unexpected address";
        case compatibility::HookResolveStatus::NonExecutable: return "hook target is not in an executable PE section";
        default: return "invalid hook target resolver input";
    }
}

} // namespace

bool PatchGtaMainMenu(MenuPatchStatus& out) {
    out.count = 1;
    auto& e = out.entries[0];
    e = {"CMenuManager::Process", kProcessAddress, 0, false, false, nullptr};

    if (g_patched) {
        e.firstByte = *reinterpret_cast<const uint8_t*>(kProcessAddress);
        if (e.firstByte != 0xC3) {
            e.reason = "patched Process entry was modified";
            return false;
        }
        e.patched = true;
        return true;
    }

    if (const char* reason = ResolveProcessTarget()) {
        e.reason = reason;
        return false;
    }
    e.firstByte = *reinterpret_cast<const uint8_t*>(kProcessAddress);
    g_original = e.firstByte;
    g_originalCaptured = true;
    if (!WriteByte(kProcessAddress, 0xC3)) {
        e.reason = "VirtualProtect/WriteByte failed";
        return false;
    }
    g_patched = true;
    e.patched = true;
    return true;
}

void RestoreGtaMainMenu() {
    if (g_patched && g_originalCaptured)
        WriteByte(kProcessAddress, g_original);
    g_patched = false;
}

} // namespace skr::lobby
