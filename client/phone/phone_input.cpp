// PHONE-001 M2 — Phone input ownership and routing.
//
// Sits inside the existing pause_input chain. The phone key (L by default)
// toggles the phone when eligible. While the phone owns input:
//   - SA-MP cursor mode 2 (lock camera and fire)
//   - DirectInput zeroed (via the existing vtable hook)
//   - Mouse messages drive the phone's hit regions
//   - ESC calls phone.Back() (page → app root → home → close)
//   - T/F6 close the phone first, then pass through
//   - All other gameplay keys are consumed
//
// SA-MP R5 offsets for chat/dialog detection are verified against both the
// sampapi 0.3.7-R5-1 headers (omp-cef/deps/samp-api) and the independent
// samp-sdk-master reverse engineering project.

#include "phone/phone_input.h"
#include "phone/phone_controller.h"
#include "phone/phone_layout.h"
#include "phone/phone_model.h"
#include "phone/phone_renderer.h"
#include "phone/phone_policy.h"
#include "phone/phone_body.h"
#include "phone/phone_body_gta.h"
#include "phone/phone_net.h"
#include "phone/phone_text.h"
#include "protocol/phone_protocol.h"
#include "gps/gps_engine.h"
#include "gps/gps_destination.h"
#include "native-pause/pause_enhanced.h"
#include "native-pause/pause_input.h"

#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace skr::phone::input {
namespace {

// ── SA-MP R5 offsets ───────────────────────────────────────────────────
// Provenance: sampapi 0.3.7-R5-1 (BlastHack) cross-checked with
// samp-sdk-master type_reconstruction.md §4.13 (Dialog) and §4.15
// (DebugConsole/CInput). All offsets gated behind IsPinnedSampR5().
constexpr DWORD     kSampR5Timestamp        = 0x6372C39E;
constexpr DWORD     kSampR5ImageSize        = 0x27E000;

// CInput (DebugConsole) — chat/command input box.
// Pointer: samp.dll + 0x26EB84. Field isOpen: +0x14E0 (BOOL).
constexpr uintptr_t kSampR5InputBoxPtrRva   = 0x26EB84;
constexpr uintptr_t kInputBoxIsOpenOff      = 0x14E0;

// CDialog — ShowPlayerDialog server dialogs.
// Pointer: samp.dll + 0x26EB50. Field isShowing: +0x28 (BOOL).
constexpr uintptr_t kSampR5DialogPtrRva     = 0x26EB50;
constexpr uintptr_t kDialogIsShowingOff     = 0x28;

// SetCursorMode / ProcessInputEnabling (same as pause_input.cpp).
constexpr uintptr_t kSampR5GameRva          = 0x26EBAC;
constexpr uintptr_t kSampR5SetCursorModeRva = 0x0A06F0;
constexpr uintptr_t kSampR5ProcessInputRva  = 0x0A05D0;
constexpr int       kSampCursorNone         = 0;

// SA-MP R5 WndProc (samp+0x61650) opens the chat by itself, BEFORE our
// pause_input subclass sees the message: it re-installs itself on top of the
// chain (samp+0x61EC0), so consuming the key in our WndProc is too late.
//   WM_KEYUP: call @0x61B1C -> 0x61360(vk)   'T' / F6 / F-keys shortcuts
//   WM_CHAR:  call @0x61BD2 -> 0x61590(ch)   chat closed: 't' 'T' '`' open it
// Both are cdecl int(int), each with exactly this one caller (verified by a
// full-image E8 scan of the pinned build). PHONE-M4F redirects the two calls
// to a filter that skips them only while a phone text field has focus.
constexpr uintptr_t kSampR5KeyUpCallRva     = 0x61B1C;
constexpr uintptr_t kSampR5KeyUpRva         = 0x61360;
constexpr uintptr_t kSampR5CharCallRva      = 0x61BD2;
constexpr uintptr_t kSampR5CharRva          = 0x61590;
constexpr int       kSampCursorLockCamAndControl = 2;

// GTA SA 1.0 US: CWorld::Players[0].m_pPed.
constexpr uintptr_t kPlayersPed = 0xB7CD98;
// plugin-sdk CPed.h VALIDATE_OFFSETs: m_fHealth 0x540, m_pVehicle 0x58C.
// Ped flags start at 0x46C: bit 0 there is bIsStanding; bInVehicle is the
// ninth bit, i.e. byte 0x46D bit 0. m_pVehicle keeps the LAST vehicle after
// the ped exits, so it is only meaningful while bInVehicle is set.
constexpr uintptr_t kPedHealthOff     = 0x540;
constexpr uintptr_t kPedInVehicleByte = 0x46D;
constexpr uintptr_t kPedVehicleOff    = 0x58C;
// plugin-sdk CVehicle.h: m_pDriver 0x460.
constexpr uintptr_t kVehicleDriverOff = 0x460;

// ── Phone key ──────────────────────────────────────────────────────────
constexpr WPARAM kDefaultPhoneKey = 'L';  // 0x4C

// ── State ──────────────────────────────────────────────────────────────
bool         g_initialized = false;
Controller   g_controller;
Model        g_model;
Views        g_views;
Layout       g_layout;
Fonts        g_fonts = {};
FrameHits    g_hits = {};
Pointer      g_pointer = {};
bool         g_cursorShown = false;
policy::KeyEdge g_keyEdge;
bool         g_suspendedByFocus = false;
State        g_loggedState = State::Hidden;
bool         g_loggedOwner = false;
bool         g_renderLogged = false;
bool         g_noFitLogged = false;
policy::Block g_lastBlockLogged = policy::Block::None;

// ── Physical phone (M3) ────────────────────────────────────────────────
body::GtaBackend  g_bodyBackend;
body::Coordinator g_body(g_bodyBackend);
body::Phase       g_loggedPhase = body::Phase::Idle;
// Cost of the body update while it does work (QPC ticks), logged per session.
int64_t  g_bodyTicks = 0, g_bodyMaxTicks = 0;
uint32_t g_bodyFrames = 0;
uint32_t     g_sampWarned = 0;     // bit per distinct SA-MP read failure already logged

// ── Diagnostics ([PHONE-M2], gated by [Client] DebugLog) ───────────────
// Event-driven only: nothing here runs per frame unless a state changed.
const char* LogPath() {
    static char path[MAX_PATH] = {};
    static bool resolved = false;
    if (resolved) return path[0] ? path : nullptr;
    resolved = true;
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&LogPath), &self))
        return nullptr;
    char dir[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(self, dir, MAX_PATH);
    char* slash = (n && n < MAX_PATH) ? strrchr(dir, '\\') : nullptr;
    if (!slash) return nullptr;
    *slash = '\0';
    snprintf(path, sizeof(path), "%s\\SKR\\phone.log", dir);
    return path;
}

void Log(const char* fmt, ...) {
    if (!skr::native_pause::enhanced::DebugLogEnabled()) return;
    const char* path = LogPath();
    if (!path) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%llu] [PHONE-M2] ", static_cast<unsigned long long>(GetTickCount64()));
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

const char* StateName(State s) {
    switch (s) {
        case State::Hidden:        return "HIDDEN";
        case State::Opening:       return "OPENING";
        case State::Home:          return "HOME";
        case State::AppActive:     return "APP_ACTIVE";
        case State::AppTransition: return "APP_TRANSITION";
        case State::Closing:       return "CLOSING";
        case State::Suspended:     return "SUSPENDED";
    }
    return "?";
}

// Logs controller state and input-owner edges since the last call.
void LogTransitions() {
    const State s = g_controller.GetState();
    if (s != g_loggedState) {
        Log("State: %s -> %s", StateName(g_loggedState), StateName(s));
        if (s == State::Hidden || s == State::Suspended) g_renderLogged = false;
        g_loggedState = s;
    }
    const bool owner = g_controller.OwnsInput();
    if (owner != g_loggedOwner) {
        Log(owner ? "Input owner: GAME -> PHONE" : "Input owner: PHONE -> GAME");
        g_loggedOwner = owner;
    }
}

// ── Guarded memory reads ───────────────────────────────────────────────
// SEH-only helpers (no C++ objects) so a stale SA-MP/GTA pointer yields a
// controlled false instead of an access violation inside the WndProc.
bool SafeReadU32(uintptr_t addr, uint32_t& out) {
    __try {
        out = *reinterpret_cast<const volatile uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadU8(uintptr_t addr, uint8_t& out) {
    __try {
        out = *reinterpret_cast<const volatile uint8_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadF32(uintptr_t addr, float& out) {
    __try {
        out = *reinterpret_cast<const volatile float*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── SA-MP R5 helpers ───────────────────────────────────────────────────
bool IsPinnedSampR5(HMODULE module) {
    if (!module) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->FileHeader.TimeDateStamp == kSampR5Timestamp &&
           nt->OptionalHeader.SizeOfImage == kSampR5ImageSize;
}

void WarnOnce(uint32_t bit, const char* what) {
    if (g_sampWarned & bit) return;
    g_sampWarned |= bit;
    Log("SA-MP read: %s (treated as closed)", what);
}

// Reads a BOOL field of a SA-MP R5 singleton. Any failure means "not open":
// the phone must stay usable when SA-MP is absent or the structure is not
// yet allocated, and each failure reason is logged once.
bool ReadSampBool(uintptr_t ptrRva, uintptr_t fieldOff, uint32_t warnBase) {
    HMODULE samp = GetModuleHandleW(L"samp.dll");
    if (!samp) { WarnOnce(1u << 0, "samp.dll not loaded"); return false; }
    if (!IsPinnedSampR5(samp)) { WarnOnce(1u << 1, "samp.dll is not the pinned 0.3.7-R5 build"); return false; }
    uint32_t obj = 0;
    if (!SafeReadU32(reinterpret_cast<uintptr_t>(samp) + ptrRva, obj)) {
        WarnOnce(warnBase << 0, "singleton pointer unreadable");
        return false;
    }
    if (!obj) return false;   // not allocated yet: normal during connect
    uint32_t value = 0;
    if (!SafeReadU32(obj + fieldOff, value)) {
        WarnOnce(warnBase << 1, "singleton field unreadable");
        return false;
    }
    return value != 0;
}

bool IsSampChatOpen() {
    return ReadSampBool(kSampR5InputBoxPtrRva, kInputBoxIsOpenOff, 1u << 2);
}

bool IsSampDialogOpen() {
    return ReadSampBool(kSampR5DialogPtrRva, kDialogIsShowingOff, 1u << 4);
}

void SetSampCursorMode(int mode) {
    HMODULE samp = GetModuleHandleW(L"samp.dll");
    if (!IsPinnedSampR5(samp)) return;
    auto* base = reinterpret_cast<uint8_t*>(samp);
    void* game = *reinterpret_cast<void**>(base + kSampR5GameRva);
    if (!game) return;
    using SetCursorModeFn = void(__thiscall*)(void*, int, int);
    using ProcessInputFn = void(__thiscall*)(void*);
    auto setCursor = reinterpret_cast<SetCursorModeFn>(base + kSampR5SetCursorModeRva);
    auto processInput = reinterpret_cast<ProcessInputFn>(base + kSampR5ProcessInputRva);
    setCursor(game, mode, mode == kSampCursorNone ? TRUE : FALSE);
    processInput(game);
}

// ── GTA player state ───────────────────────────────────────────────────
// Ped fields only; chat/dialog are sampled separately so the per-frame death
// check never touches SA-MP memory.
policy::GameView SamplePed() {
    policy::GameView g;
    uint32_t ped = 0;
    if (!SafeReadU32(kPlayersPed, ped) || !ped) return g;
    float health = 0.0f;
    if (!SafeReadF32(ped + kPedHealthOff, health)) return g;
    g.pedValid = true;
    g.alive = health > 0.0f;
    uint8_t flags = 0;
    g.inVehicle = SafeReadU8(ped + kPedInVehicleByte, flags) && (flags & 1u) != 0;
    if (g.inVehicle) {
        uint32_t vehicle = 0, driver = 0;
        // An unreadable vehicle is treated as driving: refuse rather than guess.
        g.isDriver = !SafeReadU32(ped + kPedVehicleOff, vehicle) || !vehicle ||
                     !SafeReadU32(vehicle + kVehicleDriverOff, driver) || driver == ped;
    }
    return g;
}

policy::GameView SampleGame() {
    policy::GameView g = SamplePed();
    g.chatOpen = IsSampChatOpen();
    g.dialogOpen = IsSampDialogOpen();
    return g;
}

policy::Block EvaluateOpen() {
    const policy::Block b = policy::Evaluate(SampleGame());
    Log("Open allowed: %s", b == policy::Block::None ? "YES" : "NO");
    if (b != policy::Block::None) Log("Block reason: %s", policy::BlockName(b));
    return b;
}

bool PauseOwnsCursor() {
    return skr::native_pause::input::GetState() != skr::native_pause::input::PauseState::Gameplay;
}

// CJ's physical phone follows the controller. The update is timed only when
// it does work; the per-session cost is logged when the body returns to IDLE.
void UpdateBody(bool needPed, const policy::GameView& ped) {
    if (!needPed) {
        g_body.Update(body::Request{});
    } else {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        g_body.Update(policy::BodyRequest(g_controller, ped));
        QueryPerformanceCounter(&t1);
        const int64_t dt = t1.QuadPart - t0.QuadPart;
        g_bodyTicks += dt;
        if (dt > g_bodyMaxTicks) g_bodyMaxTicks = dt;
        ++g_bodyFrames;
    }
    const body::Phase p = g_body.GetPhase();
    if (p == g_loggedPhase) return;
    Log("Body: %s -> %s (starts %u, quits %u, aborts %u, lost %u)",
        body::PhaseName(g_loggedPhase), body::PhaseName(p),
        g_body.Starts(), g_body.Quits(), g_body.Aborts(), g_body.Lost());
    g_loggedPhase = p;
    if (p == body::Phase::Idle && g_bodyFrames) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        const double us = 1e6 / static_cast<double>(f.QuadPart);
        Log("Body cost: %u frames, avg %.2f us, max %.2f us", g_bodyFrames,
            static_cast<double>(g_bodyTicks) * us / g_bodyFrames, static_cast<double>(g_bodyMaxTicks) * us);
        g_bodyTicks = g_bodyMaxTicks = 0;
        g_bodyFrames = 0;
    }
}

// ── Cursor management ──────────────────────────────────────────────────
void ShowPhoneCursor() {
    if (!g_cursorShown) {
        ClipCursor(nullptr);
        ReleaseCapture();
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        while (ShowCursor(TRUE) < 0) {}
        g_cursorShown = true;
    }
    SetSampCursorMode(kSampCursorLockCamAndControl);
}

void HidePhoneCursor() {
    // When the pause menu opened over the phone it already took the cursor
    // and SA-MP cursor mode; hiding here would leave the pause menu blind.
    if (PauseOwnsCursor()) {
        g_cursorShown = false;
        return;
    }
    if (g_cursorShown) {
        while (ShowCursor(FALSE) >= 0) {}
        SetCursor(nullptr);
        g_cursorShown = false;
    }
    SetSampCursorMode(kSampCursorNone);
}

// ── Hit testing ────────────────────────────────────────────────────────
const Hit* HitTest(float x, float y) {
    return g_hits.At(x, y);
}

// ── App actions (PHONE-M4F) ────────────────────────────────────────────
void Notice(const char* text, bool error, uint64_t now) {
    CopyText(g_model.notice, text);
    g_model.noticeError = error;
    g_model.noticeAt = now;
}

bool ValidNumber(const TextField& f) {
    return protocol::phone::IsValidPhoneNumber(f.text, f.len);
}

// Display name for a number: saved contact, then thread peer, else the number.
const char* NameFor(const char* number, uint8_t* hue) {
    for (int i = 0; i < g_model.contactCount; ++i)
        if (std::strcmp(g_model.contacts[i].number, number) == 0) {
            if (hue) *hue = g_model.contacts[i].hue;
            return g_model.contacts[i].name;
        }
    for (int i = 0; i < g_model.threadCount; ++i)
        if (std::strcmp(g_model.threads[i].number, number) == 0) {
            if (hue) *hue = g_model.threads[i].hue;
            return g_model.threads[i].peer;
        }
    return number;
}

void OpenConversationScreen(const char* number, uint64_t now) {
    uint8_t hue = 0;
    const char* name = NameFor(number, &hue);
    g_views.messages.sub = MessagesSub::Conversation;
    g_views.messages.scroll = 0;
    net::OpenConversation(number, name, hue);
    g_controller.Jump({AppId::Messages, 1}, now);
}

void SubmitDraft(uint64_t now) {
    MessagesView& mv = g_views.messages;
    if (!TextHasContent(mv.draft)) return;
    if (g_model.sync != SyncState::Ready) { Notice("Phone service unavailable", true, now); return; }
    if (net::SendSms(g_model.convo.number, mv.draft.text)) {
        TextClear(mv.draft);
        mv.scroll = 0;
    }
}

void SubmitContact(uint64_t now) {
    ContactsView& cv = g_views.contacts;
    if (!ValidNumber(cv.number)) { Notice("Enter a 7-digit phone number", true, now); return; }
    if (std::strcmp(cv.number.text, g_model.ownNumber) == 0) { Notice("That is your own number", true, now); return; }
    if (g_model.contactOpPending) return;                 // no duplicate submission
    if (!net::AddContact(cv.number.text, cv.name.text)) Notice("Phone service unavailable", true, now);
}

void SubmitComposeTo(uint64_t now) {
    MessagesView& mv = g_views.messages;
    if (!ValidNumber(mv.to)) { Notice("Enter a 7-digit phone number", true, now); return; }
    if (std::strcmp(mv.to.text, g_model.ownNumber) == 0) { Notice("That is your own number", true, now); return; }
    char number[16];
    CopyText(number, mv.to.text);
    OpenConversationScreen(number, now);
    g_views.focus = Field::SmsDraft;
}

void CopyToClipboard(const char* utf8) {
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wlen <= 0 || !OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen) * sizeof(wchar_t))) {
        if (void* dst = GlobalLock(mem)) {
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, static_cast<wchar_t*>(dst), wlen);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

void RefreshFor(AppId app) {
    if (g_model.sync != SyncState::Ready) { net::RequestSync(); return; }
    if (app == AppId::Contacts) net::RefreshContacts();
    if (app == AppId::Messages) net::RefreshThreads();
}

void HandleClick(float x, float y, uint64_t now) {
    const Hit* h = HitTest(x, y);
    if (!h) { g_views.focus = Field::None; return; }
    if (g_model.setupSupported && (!g_model.profileLoaded || g_model.profile.stage<4) &&
        (h->action==Action::Back || h->action==Action::Home)) { g_controller.Close(now); return; }
    if (g_model.setupSupported && (!g_model.profileLoaded || g_model.profile.stage<4) &&
        h->action!=Action::SetupNext && h->action!=Action::SetupWallpaper && h->action!=Action::ProfileRetry &&
        h->action!=Action::Back && h->action!=Action::Home) return;

    g_controller.Press(HitId(h->action, h->index), now);
    if (h->action != Action::FocusField && h->action != Action::SendSms) g_views.focus = Field::None;

    ContactsView& cv = g_views.contacts;
    switch (h->action) {
        case Action::OpenApp: {
            const AppId app = static_cast<AppId>(h->index);
            if (g_controller.OpenApp(app, now)) RefreshFor(app);
            break;
        }
        case Action::Back:
            if (cv.confirmRemove) cv.confirmRemove = false;
            else g_controller.Back(now);
            break;
        case Action::Home:
            g_controller.GoHome(now);
            break;
        case Action::OpenThread:
            if (h->index < g_model.threadCount) {
                g_views.messages.thread = h->index;
                char number[16];
                CopyText(number, g_model.threads[h->index].number);
                OpenConversationScreen(number, now);
            }
            break;
        case Action::OpenContact:
            if (h->index < g_model.contactCount) {
                cv.sub = ContactsSub::Details;
                cv.detail = h->index;
                CopyText(cv.detailNumber, g_model.contacts[h->index].number);
                cv.confirmRemove = false;
                g_controller.OpenPage(1, now);
            }
            break;
        case Action::AddContact:
            cv.sub = ContactsSub::Add;
            cv.confirmRemove = false;
            TextClear(cv.name);
            TextClear(cv.number);
            g_views.focus = Field::ContactName;
            g_controller.OpenPage(1, now);
            break;
        case Action::SaveContact:
            SubmitContact(now);
            break;
        case Action::MessageContact:
            if (cv.detailNumber[0]) {
                char number[16];
                CopyText(number, cv.detailNumber);
                OpenConversationScreen(number, now);
                g_views.focus = Field::SmsDraft;
            }
            break;
        case Action::RemoveContact:
            if (!g_model.contactOpPending) cv.confirmRemove = true;
            break;
        case Action::ConfirmRemove:
            cv.confirmRemove = false;
            if (!net::RemoveContact(cv.detailNumber)) Notice("Phone service unavailable", true, now);
            break;
        case Action::CancelRemove:
            cv.confirmRemove = false;
            break;
        case Action::Compose:
            g_views.messages.sub = MessagesSub::Compose;
            TextClear(g_views.messages.to);
            g_views.focus = Field::NewMessageTo;
            g_controller.OpenPage(1, now);
            break;
        case Action::ComposeNext:
            // index 0 = typed number, 1+ = contact row (index - 1).
            if (h->index > 0 && h->index - 1 < g_model.contactCount)
                TextSet(g_views.messages.to, g_model.contacts[h->index - 1].number, kFieldNumberMax);
            SubmitComposeTo(now);
            break;
        case Action::FocusField:
            g_views.focus = static_cast<Field>(h->index);
            break;
        case Action::SendSms:
            SubmitDraft(now);
            break;
        case Action::CopyNumber:
            if (g_model.sync == SyncState::Ready && g_model.ownNumber[0]) {
                CopyToClipboard(g_model.ownNumber);
                g_views.phone.copiedAt = now ? now : 1;
            }
            break;
        case Action::Retry:
            RefreshFor(g_controller.Current().app);
            break;
        case Action::LoadOlder:
            net::LoadOlderHistory();
            break;
        case Action::OpenBanner:
            if (g_model.banner.number[0]) {
                char number[16];
                CopyText(number, g_model.banner.number);
                OpenConversationScreen(number, now);
            }
            break;
        case Action::AcceptCall:
        case Action::DeclineCall:
            // Calls are not provided by the server (M4: signalling stubs only).
            g_controller.EndCall();
            break;
        case Action::OpenGpsMap:
            g_controller.Close(now);
            if (g_cursorShown) HidePhoneCursor();
            skr::native_pause::input::SetActiveTab(skr::native_pause::input::PauseTab::Map);
            skr::native_pause::input::SetState(skr::native_pause::input::PauseState::PauseOpen);
            break;
        case Action::ProfileRetry:
            if (g_model.sync==SyncState::Ready) net::RequestProfile(); else net::RequestSync();
            break;
        case Action::SetupNext:
            if (g_model.profileLoaded && g_model.profile.stage<4)
                net::SaveProfile(g_model.profile.stage+1,g_model.profile.wallpaper,g_model.profile.scale,g_model.profile.banners!=0);
            break;
        case Action::SetupWallpaper:
            net::SaveProfile(g_model.profile.stage,1-g_model.profile.wallpaper,g_model.profile.scale,g_model.profile.banners!=0);
            break;
        case Action::ToggleSetting:
            if (h->index==2) net::SaveProfile(g_model.profile.stage,g_model.profile.wallpaper,g_model.profile.scale,!g_model.settings.banners);
            if (h->index==3) net::SaveProfile(g_model.profile.stage,g_model.profile.wallpaper,g_model.profile.scale>=125?80:g_model.profile.scale+5,g_model.profile.banners!=0);
            if (h->index==4) net::SaveProfile(g_model.profile.stage,1-g_model.profile.wallpaper,g_model.profile.scale,g_model.profile.banners!=0);
            break;
        case Action::SelectDestination:
            g_views.gps.selected = h->index;
            break;
        case Action::ClearRoute:
            if (skr::gps::ClearMapWaypoint()) skr::gps::Reset();
            else { CopyText(g_model.notice,"Could not clear waypoint"); g_model.noticeError=true; g_model.noticeAt=now; }
            g_views.gps.selected = -1;
            break;
        default:
            break;
    }
}

// ── Text entry ─────────────────────────────────────────────────────────
struct FieldRef { TextField* f; size_t max; FieldKind kind; };

FieldRef FocusedField() {
    switch (g_views.focus) {
        case Field::ContactSearch: return {&g_views.contacts.search, kFieldSearchMax, FieldKind::Text};
        case Field::ContactName:   return {&g_views.contacts.name, kFieldNameMax, FieldKind::Text};
        case Field::ContactNumber: return {&g_views.contacts.number, kFieldNumberMax, FieldKind::Digits};
        case Field::SmsDraft:      return {&g_views.messages.draft, kFieldSmsMax, FieldKind::Text};
        case Field::NewMessageTo:  return {&g_views.messages.to, kFieldNumberMax, FieldKind::Digits};
        default:                   return {nullptr, 0, FieldKind::Text};
    }
}

// The focused field must still be on screen; otherwise its keys go to the phone.
bool FieldVisible(Field f) {
    const Screen s = g_controller.Current();
    switch (f) {
        case Field::ContactSearch: return s.app == AppId::Contacts && s.page == 0;
        case Field::ContactName:
        case Field::ContactNumber: return s.app == AppId::Contacts && s.page == 1 && g_views.contacts.sub == ContactsSub::Add;
        case Field::SmsDraft:      return s.app == AppId::Messages && s.page == 1 && g_views.messages.sub == MessagesSub::Conversation;
        case Field::NewMessageTo:  return s.app == AppId::Messages && s.page == 1 && g_views.messages.sub == MessagesSub::Compose;
        default:                   return false;
    }
}

void SubmitField(uint64_t now) {
    switch (g_views.focus) {
        case Field::ContactSearch: g_views.focus = Field::None; break;
        case Field::ContactName:   g_views.focus = Field::ContactNumber; break;
        case Field::ContactNumber: SubmitContact(now); break;
        case Field::SmsDraft:      SubmitDraft(now); break;
        case Field::NewMessageTo:  SubmitComposeTo(now); break;
        default: break;
    }
}

wchar_t g_highSurrogate = 0;

void InsertCodeUnit(wchar_t wc) {
    const FieldRef r = FocusedField();
    if (!r.f) return;
    if (wc >= 0xD800 && wc <= 0xDBFF) { g_highSurrogate = wc; return; }
    uint32_t cp = wc;
    if (wc >= 0xDC00 && wc <= 0xDFFF) {
        if (!g_highSurrogate) return;
        cp = 0x10000 + ((static_cast<uint32_t>(g_highSurrogate) - 0xD800) << 10) + (wc - 0xDC00);
    }
    g_highSurrogate = 0;
    if (cp == '\r' || cp == '\n' || cp == '\t') cp = ' ';
    TextInsert(*r.f, cp, r.max, r.kind);
}

void PasteClipboard() {
    if (!OpenClipboard(nullptr)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            for (int i = 0; w[i] && i < 256; ++i) InsertCodeUnit(w[i]);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

// Keyboard while a phone text field has focus. Every key is consumed so it
// never reaches SA-MP chat or GTA (DirectInput is already suppressed).
bool ProcessTextMessage(unsigned int msg, uintptr_t wParam, uint64_t now) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_ESCAPE) { g_views.focus = Field::None; return true; }
            if (wParam == VK_RETURN) { SubmitField(now); return true; }
            if (wParam == VK_BACK) {
                const FieldRef r = FocusedField();
                if (r.f) TextBackspace(*r.f);
                return true;
            }
            if (wParam == VK_TAB) {
                if (g_views.focus == Field::ContactName) g_views.focus = Field::ContactNumber;
                else if (g_views.focus == Field::ContactNumber) g_views.focus = Field::ContactName;
                return true;
            }
            if (wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) { PasteClipboard(); return true; }
            return true;                     // its WM_CHAR follows via TranslateMessage
        case WM_KEYUP:
        case WM_SYSKEYUP:
            return true;
        case WM_CHAR: {
            if (wParam < 0x20 || wParam == 0x7F) return true;           // handled on WM_KEYDOWN
            if ((GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) return true;
            HWND focus = GetFocus();
            if (focus && !IsWindowUnicode(focus) && wParam >= 0x80 && wParam <= 0xFF) {
                const char b = static_cast<char>(wParam);
                wchar_t wc = 0;
                if (MultiByteToWideChar(CP_ACP, 0, &b, 1, &wc, 1) == 1) InsertCodeUnit(wc);
                return true;
            }
            InsertCodeUnit(static_cast<wchar_t>(wParam));
            return true;
        }
        default:
            return false;
    }
}

void HandleRelease(uint64_t now) {
    g_controller.Release(now);
}

// ── SA-MP key shortcuts while typing ───────────────────────────────────
using SampKeyFn = int(__cdecl*)(int);
SampKeyFn g_sampKeyUp = nullptr;
SampKeyFn g_sampChar = nullptr;
bool g_sampKeyFilterTried = false;

bool TypingInPhone() {
    return g_initialized && g_views.focus != Field::None && g_controller.OwnsInput() && FieldVisible(g_views.focus);
}

// Returning 0 = "not handled": SA-MP's WndProc then forwards the message down
// the chain, where phone_input consumes it (the character goes into the field).
int __cdecl FilteredSampKeyUp(int vk) { return TypingInPhone() ? 0 : g_sampKeyUp(vk); }
int __cdecl FilteredSampChar(int ch) { return TypingInPhone() ? 0 : g_sampChar(ch); }

bool CallTargets(uintptr_t base, uintptr_t siteRva, uintptr_t targetRva) {
    const auto* site = reinterpret_cast<const uint8_t*>(base + siteRva);
    return site[0] == 0xE8 && base + siteRva + 5 + *reinterpret_cast<const int32_t*>(site + 1) == base + targetRva;
}

void PointCallAt(uintptr_t base, uintptr_t siteRva, void* fn) {
    auto* rel = reinterpret_cast<int32_t*>(base + siteRva + 1);
    DWORD old = 0;
    if (!VirtualProtect(rel, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    *rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(fn) - (base + siteRva + 5));
    VirtualProtect(rel, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), rel, 4);
}

// Once, on the game thread (the thread that runs the WndProc, so no message
// is mid-dispatch). Pinned R5 only, all-or-nothing on verified call bytes.
void InstallSampKeyFilter() {
    if (g_sampKeyFilterTried) return;
    HMODULE samp = GetModuleHandleW(L"samp.dll");
    if (!samp) return;                                  // retried until SA-MP loads
    g_sampKeyFilterTried = true;
    if (!IsPinnedSampR5(samp)) { Log("SA-MP key filter: not installed (samp.dll is not pinned R5)"); return; }
    const auto base = reinterpret_cast<uintptr_t>(samp);
    if (!CallTargets(base, kSampR5KeyUpCallRva, kSampR5KeyUpRva) || !CallTargets(base, kSampR5CharCallRva, kSampR5CharRva)) {
        Log("SA-MP key filter: not installed (call sites already modified)");
        return;
    }
    g_sampKeyUp = reinterpret_cast<SampKeyFn>(base + kSampR5KeyUpRva);
    g_sampChar = reinterpret_cast<SampKeyFn>(base + kSampR5CharRva);
    PointCallAt(base, kSampR5KeyUpCallRva, reinterpret_cast<void*>(&FilteredSampKeyUp));
    PointCallAt(base, kSampR5CharCallRva, reinterpret_cast<void*>(&FilteredSampChar));
    Log("SA-MP key filter: installed (WM_KEYUP/WM_CHAR chat shortcuts skipped while typing)");
}

// Status bar clock and lock-screen date from the local clock (once a second).
void UpdateClock(uint64_t now) {
    static uint64_t last = 0;
    static WORD lastMinute = 0xFFFF;
    if (last && now - last < 1000) return;
    last = now;
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (st.wMinute == lastMinute && g_model.status.time[0]) return;
    lastMinute = st.wMinute;
    static const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July",
                                    "August", "September", "October", "November", "December"};
    std::snprintf(g_model.status.time, sizeof(g_model.status.time), "%02u:%02u", st.wHour, st.wMinute);
    std::snprintf(g_model.status.date, sizeof(g_model.status.date), "%s, %u %s",
                  kDays[st.wDayOfWeek % 7], st.wDay, kMonths[(st.wMonth + 11) % 12]);
}

// Contact add/remove finished: leave the form / details page on success.
void ConsumeContactResult(uint64_t now) {
    if (g_model.contactOpResult == Model::OpResult::None) return;
    const bool ok = g_model.contactOpResult == Model::OpResult::Ok;
    g_model.contactOpResult = Model::OpResult::None;
    const Screen s = g_controller.Current();
    if (ok && s.app == AppId::Contacts && s.page == 1) {
        if (g_views.focus == Field::ContactName || g_views.focus == Field::ContactNumber) g_views.focus = Field::None;
        g_controller.Back(now);
    }
}

} // anonymous namespace

// ── Public API ─────────────────────────────────────────────────────────

void Initialize() {
    if (g_initialized) return;
    g_controller = Controller();
    // The model is NOT reset here: phone_net may already hold the server's
    // identity if Welcome arrived before the first frame. Contacts, threads and
    // the own number come only from the server (no sample data in game).
    g_model.previewData = false;
    g_model.status.signal = 4;
    g_model.status.battery = 100;

    // Sample GPS destinations
    CopyText(g_model.gps.list[0].name, "Los Santos PD");
    CopyText(g_model.gps.list[0].area, "Pershing Square");
    g_model.gps.list[0].distanceM = 420.0f;
    CopyText(g_model.gps.list[1].name, "All Saints Hospital");
    CopyText(g_model.gps.list[1].area, "Market");
    g_model.gps.list[1].distanceM = 780.0f;
    CopyText(g_model.gps.list[2].name, "Ammunation");
    CopyText(g_model.gps.list[2].area, "Market");
    g_model.gps.list[2].distanceM = 1150.0f;
    g_model.gps.count = 3;

    g_views = Views();
    g_keyEdge = {};
    g_body.SetHoldPose(body::LoadHoldPose());
    g_initialized = true;
    Log("Module initialized");
    Log("Build: SKR.Client " __DATE__ " " __TIME__);
    Log("Input hook active (pause_input WndProc chain, key=L)");
    Log("Body: GTA CTaskComplexUseMobilePhone, hold pose %s (%.2f s)",
        g_body.GetHoldPose().enabled ? "ON" : "off", g_body.GetHoldPose().atSec);
}

void Shutdown() {
    if (!g_initialized) return;
    g_body.Release(true);
    if (g_cursorShown) HidePhoneCursor();
    g_controller.Reset();
    LogTransitions();
    g_initialized = false;
}

bool ProcessMessage(unsigned int msg, uintptr_t wParam, intptr_t lParam,
                    uint64_t now) {
    if (!g_initialized) return false;

    // Text entry first: while a field has focus, L / T / Esc are characters
    // or editing keys, not phone or chat shortcuts.
    if (g_views.focus != Field::None && g_controller.OwnsInput() && FieldVisible(g_views.focus) &&
        ProcessTextMessage(msg, wParam, now))
        return true;

    // Phone key toggle: one physical press = one edge (see policy::KeyEdge).
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == kDefaultPhoneKey) {
        if (!g_keyEdge.Down(static_cast<uint32_t>(lParam), now))
            return g_controller.Visible();         // repeat / duplicate: eat it only while shown
        Log("Key L down edge");
        const policy::KeyResult r = policy::OnPhoneKeyEdge(g_controller, now, EvaluateOpen);
        if (r == policy::KeyResult::Opened) g_suspendedByFocus = false;
        LogTransitions();
        return r != policy::KeyResult::Blocked;    // blocked: L reaches chat/SA-MP untouched
    }
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == kDefaultPhoneKey) {
        g_keyEdge.Up();
        return g_controller.Visible();
    }

    // Focus handling runs regardless of ownership: a Suspended phone does not
    // own input, and its recovery path must still be reachable.
    if (msg == WM_KILLFOCUS) {
        g_keyEdge.Reset();
        if (g_controller.OwnsInput()) {
            g_controller.Suspend();
            g_suspendedByFocus = true;
            LogTransitions();
        }
        return false;
    }
    if (msg == WM_SETFOCUS) {
        if (g_suspendedByFocus && g_controller.GetState() == State::Suspended) {
            g_suspendedByFocus = false;
            if (EvaluateOpen() == policy::Block::None) g_controller.Resume(now);
            else g_controller.Close(now);         // Suspended -> Hidden
            LogTransitions();
        }
        return false;
    }

    // Everything below only applies while the phone owns input
    if (!g_controller.OwnsInput()) return false;

    // ESC → phone.Back() (page → app root → home → close)
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
        if ((static_cast<uint32_t>(lParam) & (1u << 30)) != 0) return true;
        g_controller.Back(now);
        LogTransitions();
        return true;
    }
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_ESCAPE) return true;

    // T / F6 (chat keys): close the phone and let the key pass through
    if ((msg == WM_KEYDOWN) && (wParam == 'T' || wParam == VK_F6)) {
        g_controller.Close(now);
        LogTransitions();
        return false;  // pass through so SA-MP opens chat
    }

    // Mouse tracking
    if (msg == WM_MOUSEMOVE) {
        g_pointer.x = static_cast<float>(static_cast<short>(LOWORD(lParam)));
        g_pointer.y = static_cast<float>(static_cast<short>(HIWORD(lParam)));
        return true;
    }
    if (msg == WM_LBUTTONDOWN) {
        g_pointer.down = true;
        HandleClick(g_pointer.x, g_pointer.y, now);
        return true;
    }
    if (msg == WM_LBUTTONUP) {
        g_pointer.down = false;
        HandleRelease(now);
        return true;
    }

    // Scroll: adjust the active list's scroll position
    if (msg == WM_MOUSEWHEEL) {
        float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / 120.0f;
        float scrollPx = delta * 30.0f;
        Screen cur = g_controller.Current();
        switch (cur.app) {
            case AppId::Contacts: g_views.contacts.scroll -= scrollPx; break;
            case AppId::Messages:
                if (cur.page > 0)
                    g_views.messages.scroll += scrollPx;   // conversation scroll = distance from the newest message
                else
                    g_views.messages.listScroll -= scrollPx;
                break;
            case AppId::Gps: g_views.gps.scroll -= scrollPx; break;
            default: break;
        }
        return true;
    }

    // WM_SETCURSOR — show our arrow while we own input
    if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return true;
    }

    // Consume all other keyboard/mouse messages to prevent GTA interaction
    switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEHWHEEL:
        case WM_INPUT:
            return true;
    }

    return false;
}

void Tick(uint64_t now) {
    if (!g_initialized) return;

    // Update controller animation
    g_controller.Update(now);
    if (!WantsRender()) g_hits = {};
    UpdateClock(now);
    if (!g_model.previewData) {
        const auto status=skr::gps::GetStatus();
        g_model.gps.status = status==skr::gps::NavStatus::Idle ? NavStatus::Idle
            : status==skr::gps::NavStatus::Calculating ? NavStatus::Calculating
            : status==skr::gps::NavStatus::Navigating ? NavStatus::Navigating
            : status==skr::gps::NavStatus::Arrived ? NavStatus::Arrived : NavStatus::Unavailable;
        const auto& route=skr::gps::GetRoute();
        g_model.gps.remainingM=route.remainingDist;
        CopyText(g_model.gps.destName,skr::gps::HasDestination()?"Map waypoint":"No destination");
        CopyText(g_model.gps.destArea,route.failure==skr::gps::RouteFailure::None?"Road navigation":skr::gps::ToString(route.failure));
        g_model.gps.count=0; // No supported POI registry; never show invented destinations.
    }
    ConsumeContactResult(now);
    InstallSampKeyFilter();
    if (g_views.focus != Field::None && (!g_controller.OwnsInput() || !FieldVisible(g_views.focus)))
        g_views.focus = Field::None;

    // Manage cursor ownership based on phone state
    bool wantsInput = g_controller.OwnsInput();
    if (wantsInput && !g_cursorShown) ShowPhoneCursor();
    if (!wantsInput && g_cursorShown) HidePhoneCursor();

    // Reassert SA-MP cursor mode while phone owns input (same pattern as
    // pause_input::Tick — SA-MP may reset it during its frame tick).
    if (wantsInput) SetSampCursorMode(kSampCursorLockCamAndControl);

    // Death / ped destroyed (disconnect, respawn): reset the phone. Suspended
    // counts too, so a phone hidden by the pause menu cannot outlive the ped.
    const State st = g_controller.GetState();
    // Hidden with no physical phone: no game reads at all this frame.
    const bool needPed = st != State::Hidden || g_body.Owns();
    const policy::GameView ped = needPed ? SamplePed() : policy::GameView{};
    if (st != State::Hidden) {
        switch (policy::WhileOpen(ped)) {
            case policy::Running::Reset:
                Log("Reset: %s", ped.pedValid ? "player-dead" : "no-player-ped");
                g_controller.Reset();
                g_suspendedByFocus = false;
                if (g_cursorShown) HidePhoneCursor();
                break;
            case policy::Running::Close:
                if (st != State::Closing) {
                    Log("Close: player is driving");
                    g_controller.Close(now);
                }
                break;
            case policy::Running::Keep:
                break;
        }
    }

    UpdateBody(needPed, ped);
    LogTransitions();
}

bool WantsDirectInput() {
    return g_initialized && g_controller.OwnsInput();
}

bool WantsRender() {
    return g_initialized && policy::ShouldRender(g_controller);
}

Controller& GetController() { return g_controller; }
Model& GetModel() { return g_model; }
Views& GetViews() { return g_views; }

const Layout& GetLayout() {
    return g_layout;
}

const Fonts& GetFonts() { return g_fonts; }
const FrameHits& GetHits() { return g_hits; }

void SetFonts(const Fonts& f) { g_fonts = f; }

void SetFrameResult(const Layout& layout, const FrameHits& hits) {
    g_layout = layout;
    g_hits = hits;
    if (!layout.fits) {
        if (!g_noFitLogged) Log("Render skipped: layout does not fit the viewport");
        g_noFitLogged = true;
        return;
    }
    g_noFitLogged = false;
    if (!g_renderLogged && g_controller.Visible()) {
        Log("Render submitted (%d hit regions, body %.0fx%.0f at %.0f,%.0f)",
            hits.count, layout.body.W(), layout.body.H(), layout.body.x0, layout.body.y0);
        g_renderLogged = true;
    }
}

void OnIncomingCall(const char* name, const char* number, uint64_t now) {
    if (!g_initialized) return;
    CopyText(g_model.caller.name, name);
    CopyText(g_model.caller.number, number);
    g_controller.BeginIncomingCall(now);
}

void OnCallEnded() {
    if (!g_initialized) return;
    g_controller.EndCall();
}

void OnPushNotification(const char* title, const char* text, uint64_t now) {
    if (!g_initialized) return;
    CopyText(g_model.banner.title, title);
    CopyText(g_model.banner.text, text);
    g_model.banner.number[0] = 0;
    g_controller.PushNotification(now);
}

void OnSmsNotification(const char* title, const char* text, const char* number, uint8_t hue, uint64_t now) {
    if (!g_initialized) return;
    g_model.banner.app = AppId::Messages;
    CopyText(g_model.banner.title, title);
    CopyText(g_model.banner.text, text);
    CopyText(g_model.banner.number, number);
    g_model.banner.hue = hue;
    g_controller.PushNotification(now);
}

bool IsVisible() {
    return g_initialized && g_controller.Visible();
}

bool IsConversationVisible(const char* number) {
    if (!IsVisible() || !number) return false;
    const Screen s = g_controller.Current();
    return s.app == AppId::Messages && s.page == 1 && g_views.messages.sub == MessagesSub::Conversation &&
           std::strcmp(g_model.convo.number, number) == 0;
}

} // namespace skr::phone::input
