#include "native-pause/pause_controls.h"

#include "native-pause/pause_settings.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace skr::native_pause::controls {
namespace {

// GTA SA 1.0 US controller provenance:
// - local plugin-sdk-SA-LLM CControllerConfigManager.h SHA-256
//   F21DB04185E6B4374912F3DE77B79F2437EE2555A6686C8C5674AAD56891B989
// - local RenderWare skeleton.h SHA-256
//   52556FBC4414C9CD7844EFDB6236A2C325AA7454919F6F054C7C5C6A69827586
// - gta-reversed ControllerConfigManager.{h,cpp}, GTA SA 1.0 US.
constexpr uintptr_t kControlsManager = 0x00B70198;
constexpr uintptr_t kGetControllerKey = 0x0052F4F0;
constexpr uintptr_t kGetMouseButton = 0x0052F580;
constexpr uintptr_t kSetControllerKey = 0x00530490;
constexpr uintptr_t kSetMouseButton = 0x0052F590;
constexpr uintptr_t kClearAction = 0x0052FD70;
constexpr uintptr_t kDeleteMatching = 0x00531C90;
constexpr uintptr_t kReinitControls = 0x00531F20;

constexpr int kActionSlots = 59;
constexpr int kControllerTypes = 4;
constexpr uint32_t kKeyboardBlank = 1056; // rsNULL
constexpr uint32_t kMouseBlank = 0;

constexpr uint32_t kMouseLeft = 1;
constexpr uint32_t kMouseMiddle = 2;
constexpr uint32_t kMouseRight = 3;
constexpr uint32_t kMouseWheelUp = 4;
constexpr uint32_t kMouseWheelDown = 5;
constexpr uint32_t kMouseX1 = 6;
constexpr uint32_t kMouseX2 = 7;

struct NativeControllerKey {
    uint32_t code;
    uint32_t priority;
};
struct NativeControllerAction {
    NativeControllerKey keys[kControllerTypes];
};
struct NativeControllerManagerLayout {
    uint8_t beforeActions[0xB70];
    NativeControllerAction actions[kActionSlots];
    uint8_t afterActions[0x14];
};
static_assert(sizeof(NativeControllerKey) == 0x8);
static_assert(sizeof(NativeControllerAction) == 0x20);
static_assert(offsetof(NativeControllerManagerLayout, actions) == 0xB70);
static_assert(sizeof(NativeControllerManagerLayout) == 0x12E4);

using GetControllerKeyFn = uint32_t(__thiscall*)(void*, int, int);
using GetMouseButtonFn = uint32_t(__thiscall*)(void*, int);
using SetControllerKeyFn = void(__thiscall*)(void*, int, uint32_t, int);
using SetMouseButtonFn = void(__thiscall*)(void*, int, uint32_t);
using ClearActionFn = void(__thiscall*)(void*, int, int);
using DeleteMatchingFn = void(__thiscall*)(void*, int, uint32_t, int);
using ReinitControlsFn = void(__thiscall*)(void*);

using C = Category;
using X = logic::ActionContext;

// One authoritative table based on the pinned GTA 1.0 US enum. Slot 53 is
// NUM_OF_1ST_PERSON_ACTIONS (a marker, not an action), so it is intentionally
// absent. Action 58 is present in the 59-slot layout but GTA's action-context
// function reports NONE; it is displayed read-only rather than mutated.
constexpr ActionDescriptor kActions[] = {
    { 0, "Fire weapon", C::OnFoot, X::FirstAndThirdPerson, true },
    { 1, "Secondary fire", C::OnFoot, X::FirstAndThirdPerson, true },
    { 2, "Next weapon", C::OnFoot, X::ThirdPerson, true },
    { 3, "Previous weapon", C::OnFoot, X::ThirdPerson, true },
    { 4, "Move forward", C::OnFoot, X::FirstAndThirdPerson, true },
    { 5, "Move backward", C::OnFoot, X::FirstAndThirdPerson, true },
    { 6, "Move left", C::OnFoot, X::FirstAndThirdPerson, true },
    { 7, "Move right", C::OnFoot, X::FirstAndThirdPerson, true },
    { 8, "Sniper zoom in", C::OnFoot, X::FirstAndThirdPerson, true },
    { 9, "Sniper zoom out", C::OnFoot, X::FirstAndThirdPerson, true },
    {12, "Jump", C::OnFoot, X::ThirdPerson, true },
    {13, "Sprint", C::OnFoot, X::ThirdPerson, true },
    {14, "Look behind", C::OnFoot, X::ThirdPerson, true },
    {15, "Crouch", C::OnFoot, X::ThirdPerson, true },
    {16, "Answer phone", C::OnFoot, X::ThirdPerson, true },
    {17, "Walk", C::OnFoot, X::ThirdPerson, true },
    {32, "First-person look left", C::OnFoot, X::FirstAndThirdPerson, true },
    {33, "First-person look right", C::OnFoot, X::FirstAndThirdPerson, true },
    {42, "Cycle target left", C::OnFoot, X::ThirdPerson, true },
    {43, "Cycle target right", C::OnFoot, X::ThirdPerson, true },
    {44, "Center camera", C::OnFoot, X::ThirdPerson, true },
    {45, "Lock target", C::OnFoot, X::FirstAndThirdPerson, true },
    {47, "Conversation yes", C::OnFoot, X::ThirdPerson, true },
    {48, "Conversation no", C::OnFoot, X::ThirdPerson, true },
    {49, "Group forward", C::OnFoot, X::ThirdPerson, true },
    {50, "Group backward", C::OnFoot, X::ThirdPerson, true },
    {51, "First-person look up", C::OnFoot, X::FirstAndThirdPerson, true },
    {52, "First-person look down", C::OnFoot, X::FirstAndThirdPerson, true },

    {18, "Vehicle fire", C::Vehicle, X::InCar, true },
    {19, "Vehicle secondary fire", C::Vehicle, X::InCar, true },
    {20, "Steer left", C::Vehicle, X::InCar, true },
    {21, "Steer right", C::Vehicle, X::InCar, true },
    {22, "Steer up", C::Vehicle, X::InCar, true },
    {23, "Steer down", C::Vehicle, X::InCar, true },
    {24, "Accelerate", C::Vehicle, X::InCar, true },
    {25, "Brake / reverse", C::Vehicle, X::InCar, true },
    {26, "Radio station up", C::Vehicle, X::InCar, true },
    {27, "Radio station down", C::Vehicle, X::InCar, true },
    {28, "Radio track skip", C::Vehicle, X::InCar, true },
    {29, "Horn", C::Vehicle, X::InCar, true },
    {30, "Toggle submission", C::Vehicle, X::InCar, true },
    {31, "Handbrake", C::Vehicle, X::InCar, true },
    {34, "Vehicle look left", C::Vehicle, X::InCar, true },
    {35, "Vehicle look right", C::Vehicle, X::InCar, true },
    {36, "Vehicle look behind", C::Vehicle, X::InCar, true },
    {37, "Vehicle mouse look", C::Vehicle, X::InCar, true },
    {38, "Turret left", C::Vehicle, X::InCar, true },
    {39, "Turret right", C::Vehicle, X::InCar, true },
    {40, "Turret up", C::Vehicle, X::InCar, true },
    {41, "Turret down", C::Vehicle, X::InCar, true },

    {10, "Enter / exit vehicle", C::Shared, X::InCarThirdPerson, true },
    {11, "Change camera", C::Shared, X::Common, true },
    {46, "Network talk", C::Shared, X::Common, true },
    {54, "Toggle D-pad", C::Shared, X::Common, true },
    {55, "Toggle debug camera", C::Shared, X::Common, true },
    {56, "Take screenshot", C::Shared, X::Common, true },
    {57, "Toggle mouse pointer", C::Shared, X::Common, true },
    {58, "Debug camera menu", C::Shared, X::None, false },
};

struct Conflict {
    int action = -1;
    BindingSlot slot = BindingSlot::Primary;
};

logic::CaptureMachine g_capture{};
int g_captureAction = -1;
BindingSlot g_captureSlot = BindingSlot::Primary;
uint32_t g_pendingCode = 0;
std::array<Conflict, kActionSlots * 3> g_conflicts{};
int g_conflictCount = 0;
bool g_conflictPending = false;
char g_captureStatus[128] = "Select a binding to change it.";
char g_conflictSummary[256] = {};
char g_lastStatus[160] = "No control changes made.";

void* Manager() { return reinterpret_cast<void*>(kControlsManager); }

int ControllerType(BindingSlot slot) {
    return static_cast<int>(slot);
}

uint32_t BlankForType(int type) {
    return type <= static_cast<int>(BindingSlot::Secondary) ? kKeyboardBlank : kMouseBlank;
}

const ActionDescriptor* FindAction(int id) {
    for (const auto& action : kActions)
        if (action.id == id) return &action;
    return nullptr;
}

bool NativeRead(int action, int type, uint32_t& out) {
    if (action < 0 || action >= kActionSlots || type < 0 || type > 2) return false;
    __try {
        out = type == static_cast<int>(BindingSlot::Mouse)
            ? reinterpret_cast<GetMouseButtonFn>(kGetMouseButton)(Manager(), action)
            : reinterpret_cast<GetControllerKeyFn>(kGetControllerKey)(Manager(), action, type);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = BlankForType(type);
        return false;
    }
}

bool TakeSnapshot(std::array<NativeControllerAction, kActionSlots>& out) {
    __try {
        const auto* manager = reinterpret_cast<const NativeControllerManagerLayout*>(kControlsManager);
        std::memcpy(out.data(), manager->actions, sizeof(manager->actions));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool NativeSet(int action, int type, uint32_t code) {
    __try {
        if (type == static_cast<int>(BindingSlot::Mouse))
            reinterpret_cast<SetMouseButtonFn>(kSetMouseButton)(Manager(), action, code);
        else
            reinterpret_cast<SetControllerKeyFn>(kSetControllerKey)(Manager(), action, code, type);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RestoreSnapshot(const std::array<NativeControllerAction, kActionSlots>& snapshot) {
    __try {
        const auto clear = reinterpret_cast<ClearActionFn>(kClearAction);
        for (int action = 0; action < kActionSlots; ++action)
            for (int type = 0; type < kControllerTypes; ++type)
                clear(Manager(), action, type);

        // Native setters calculate ordering. Replaying in stored priority order
        // recreates the controller map without direct writes.
        for (int action = 0; action < kActionSlots; ++action) {
            for (uint32_t priority = 1; priority <= 4; ++priority) {
                for (int type = 0; type < kControllerTypes; ++type) {
                    const NativeControllerKey& key = snapshot[action].keys[type];
                    if (key.priority != priority || key.code == BlankForType(type)) continue;
                    if (type == static_cast<int>(BindingSlot::Mouse))
                        reinterpret_cast<SetMouseButtonFn>(kSetMouseButton)(Manager(), action, key.code);
                    else
                        reinterpret_cast<SetControllerKeyFn>(kSetControllerKey)(Manager(), action, key.code, type);
                }
            }
        }
        return settings::SaveNativeSettings();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SetStatus(char* target, size_t capacity, const char* text) {
    if (!capacity) return;
    std::snprintf(target, capacity, "%s", text ? text : "");
}

const char* SlotName(BindingSlot slot) {
    switch (slot) {
        case BindingSlot::Primary: return "primary";
        case BindingSlot::Secondary: return "secondary";
        case BindingSlot::Mouse: return "mouse";
    }
    return "binding";
}

void KeyName(uint32_t code, BindingSlot slot, char* out, size_t capacity) {
    if (!capacity) return;
    if (code == BlankForType(ControllerType(slot))) {
        std::snprintf(out, capacity, "UNBOUND");
        return;
    }
    if (slot == BindingSlot::Mouse) {
        const char* name = nullptr;
        switch (code) {
            case kMouseLeft: name = "LMB"; break;
            case kMouseMiddle: name = "MMB"; break;
            case kMouseRight: name = "RMB"; break;
            case kMouseWheelUp: name = "WHEEL UP"; break;
            case kMouseWheelDown: name = "WHEEL DOWN"; break;
            case kMouseX1: name = "MOUSE 4"; break;
            case kMouseX2: name = "MOUSE 5"; break;
        }
        std::snprintf(out, capacity, "%s", name ? name : "UNKNOWN");
        return;
    }

    if (code >= 33 && code <= 126) {
        std::snprintf(out, capacity, "%c", static_cast<char>(code));
        return;
    }
    if (code == 32) { std::snprintf(out, capacity, "SPACE"); return; }
    if (code >= 1001 && code <= 1012) {
        std::snprintf(out, capacity, "F%u", code - 1000);
        return;
    }
    struct NamedKey { uint32_t code; const char* name; };
    static constexpr NamedKey names[] = {
        {1013,"INSERT"},{1014,"DELETE"},{1015,"HOME"},{1016,"END"},
        {1017,"PAGE UP"},{1018,"PAGE DOWN"},{1019,"UP"},{1020,"DOWN"},
        {1021,"LEFT"},{1022,"RIGHT"},{1023,"NUM /"},{1024,"NUM *"},
        {1025,"NUM +"},{1026,"NUM -"},{1027,"NUM ."},{1028,"NUM 1"},
        {1029,"NUM 2"},{1030,"NUM 3"},{1031,"NUM 4"},{1032,"NUM 5"},
        {1033,"NUM LOCK"},{1034,"NUM 6"},{1035,"NUM 7"},{1036,"NUM 8"},
        {1037,"NUM 9"},{1038,"NUM 0"},{1039,"NUM ENTER"},{1040,"SCROLL LOCK"},
        {1041,"PAUSE"},{1042,"BACKSPACE"},{1043,"TAB"},{1044,"CAPS LOCK"},
        {1045,"ENTER"},{1046,"LEFT SHIFT"},{1047,"RIGHT SHIFT"},{1048,"SHIFT"},
        {1049,"LEFT CTRL"},{1050,"RIGHT CTRL"},{1051,"LEFT ALT"},{1052,"RIGHT ALT"},
        {1053,"LEFT WIN"},{1054,"RIGHT WIN"},{1055,"MENU"},
    };
    for (const auto& key : names) {
        if (key.code == code) {
            std::snprintf(out, capacity, "%s", key.name);
            return;
        }
    }
    std::snprintf(out, capacity, "KEY %u", code);
}

bool TranslateKeyboard(WPARAM wParam, LPARAM lParam, uint32_t& out) {
    const UINT vk = static_cast<UINT>(wParam);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        out = vk;
        return true;
    }
    if (vk >= VK_F1 && vk <= VK_F12) { out = 1001 + (vk - VK_F1); return true; }
    const bool extended = (static_cast<uintptr_t>(lParam) & (1u << 24)) != 0;
    switch (vk) {
        case VK_SPACE: out = 32; return true;
        case VK_INSERT: out = 1013; return true;
        case VK_DELETE: out = 1014; return true;
        case VK_HOME: out = 1015; return true;
        case VK_END: out = 1016; return true;
        case VK_PRIOR: out = 1017; return true;
        case VK_NEXT: out = 1018; return true;
        case VK_UP: out = 1019; return true;
        case VK_DOWN: out = 1020; return true;
        case VK_LEFT: out = 1021; return true;
        case VK_RIGHT: out = 1022; return true;
        case VK_DIVIDE: out = 1023; return true;
        case VK_MULTIPLY: out = 1024; return true;
        case VK_ADD: out = 1025; return true;
        case VK_SUBTRACT: out = 1026; return true;
        case VK_DECIMAL: out = 1027; return true;
        case VK_NUMPAD1: out = 1028; return true;
        case VK_NUMPAD2: out = 1029; return true;
        case VK_NUMPAD3: out = 1030; return true;
        case VK_NUMPAD4: out = 1031; return true;
        case VK_NUMPAD5: out = 1032; return true;
        case VK_NUMLOCK: out = 1033; return true;
        case VK_NUMPAD6: out = 1034; return true;
        case VK_NUMPAD7: out = 1035; return true;
        case VK_NUMPAD8: out = 1036; return true;
        case VK_NUMPAD9: out = 1037; return true;
        case VK_NUMPAD0: out = 1038; return true;
        case VK_RETURN: out = extended ? 1039 : 1045; return true;
        case VK_SCROLL: out = 1040; return true;
        case VK_PAUSE: out = 1041; return true;
        case VK_BACK: out = 1042; return true;
        case VK_TAB: out = 1043; return true;
        case VK_CAPITAL: out = 1044; return true;
        case VK_LSHIFT: out = 1046; return true;
        case VK_RSHIFT: out = 1047; return true;
        case VK_SHIFT: out = (MapVirtualKeyA((static_cast<UINT>(lParam) >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT) ? 1047 : 1046; return true;
        case VK_LCONTROL: out = 1049; return true;
        case VK_RCONTROL: out = 1050; return true;
        case VK_CONTROL: out = extended ? 1050 : 1049; return true;
        case VK_LMENU: out = 1051; return true;
        case VK_RMENU: out = 1052; return true;
        case VK_MENU: out = extended ? 1052 : 1051; return true;
        case VK_LWIN: out = 1053; return true;
        case VK_RWIN: out = 1054; return true;
        case VK_APPS: out = 1055; return true;
    }
    const UINT mapped = MapVirtualKeyA(vk, MAPVK_VK_TO_CHAR) & 0xFFu;
    if (mapped >= 33 && mapped <= 255) {
        out = mapped;
        return true;
    }
    return false;
}

bool TranslateMouse(UINT msg, WPARAM wParam, uint32_t& out) {
    switch (msg) {
        case WM_LBUTTONDOWN: out = kMouseLeft; return true;
        case WM_MBUTTONDOWN: out = kMouseMiddle; return true;
        case WM_RBUTTONDOWN: out = kMouseRight; return true;
        case WM_MOUSEWHEEL:
            out = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? kMouseWheelUp : kMouseWheelDown;
            return GET_WHEEL_DELTA_WPARAM(wParam) != 0;
        case WM_XBUTTONDOWN:
            out = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? kMouseX1 : kMouseX2;
            return true;
    }
    return false;
}

bool IsReleaseMessage(UINT msg) {
    return msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_LBUTTONUP ||
           msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP;
}

void BuildConflictSummary() {
    if (!g_conflictCount) {
        g_conflictSummary[0] = '\0';
        return;
    }
    char key[48]{};
    KeyName(g_pendingCode, g_captureSlot, key, sizeof(key));
    int written = std::snprintf(g_conflictSummary, sizeof(g_conflictSummary),
                                "%s is already used by ", key);
    for (int i = 0; i < g_conflictCount && i < 3 && written > 0 && written < static_cast<int>(sizeof(g_conflictSummary)); ++i) {
        const ActionDescriptor* action = FindAction(g_conflicts[i].action);
        written += std::snprintf(g_conflictSummary + written, sizeof(g_conflictSummary) - written,
                                 "%s%s", i ? ", " : "", action ? action->label : "another action");
    }
    if (g_conflictCount > 3 && written > 0 && written < static_cast<int>(sizeof(g_conflictSummary)))
        std::snprintf(g_conflictSummary + written, sizeof(g_conflictSummary) - written,
                      " and %d more", g_conflictCount - 3);
}

void FindConflicts() {
    g_conflictCount = 0;
    const ActionDescriptor* target = FindAction(g_captureAction);
    if (!target) return;
    const int firstType = g_captureSlot == BindingSlot::Mouse ? 2 : 0;
    const int lastType = g_captureSlot == BindingSlot::Mouse ? 2 : 1;
    for (const auto& candidate : kActions) {
        if (!logic::ContextsConflict(target->context, candidate.context)) continue;
        for (int type = firstType; type <= lastType; ++type) {
            uint32_t code = BlankForType(type);
            if (!NativeRead(candidate.id, type, code) || code != g_pendingCode) continue;
            if (candidate.id == g_captureAction && type == ControllerType(g_captureSlot)) continue;
            if (g_conflictCount < static_cast<int>(g_conflicts.size()))
                g_conflicts[g_conflictCount++] = {candidate.id, static_cast<BindingSlot>(type)};
        }
    }
    BuildConflictSummary();
}

bool ConflictsCleared() {
    const ActionDescriptor* target = FindAction(g_captureAction);
    if (!target) return false;
    const int firstType = g_captureSlot == BindingSlot::Mouse ? 2 : 0;
    const int lastType = g_captureSlot == BindingSlot::Mouse ? 2 : 1;
    for (const auto& candidate : kActions) {
        if (!logic::ContextsConflict(target->context, candidate.context)) continue;
        for (int type = firstType; type <= lastType; ++type) {
            if (candidate.id == g_captureAction && type == ControllerType(g_captureSlot)) continue;
            uint32_t code = BlankForType(type);
            if (NativeRead(candidate.id, type, code) && code == g_pendingCode) return false;
        }
    }
    return true;
}

bool CommitPendingBinding() {
    std::array<NativeControllerAction, kActionSlots> snapshot{};
    if (!TakeSnapshot(snapshot)) {
        SetStatus(g_lastStatus, sizeof(g_lastStatus), "Binding failed: GTA controller map unavailable.");
        return false;
    }

    bool mutated = false;
    __try {
        const auto remove = reinterpret_cast<DeleteMatchingFn>(kDeleteMatching);
        if (g_captureSlot == BindingSlot::Mouse) {
            remove(Manager(), g_captureAction, g_pendingCode, 2);
        } else {
            // Both keyboard columns are live simultaneously. Ask GTA to remove
            // context-conflicting initiators in each before assigning the chosen column.
            remove(Manager(), g_captureAction, g_pendingCode, 0);
            remove(Manager(), g_captureAction, g_pendingCode, 1);
        }
        mutated = NativeSet(g_captureAction, ControllerType(g_captureSlot), g_pendingCode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mutated = false;
    }

    uint32_t readback = 0;
    const bool saved = mutated && settings::SaveNativeSettings();
    const bool read = NativeRead(g_captureAction, ControllerType(g_captureSlot), readback);
    const bool verifiedReadback = logic::ReadbackMatches(g_pendingCode, readback, read);
    const auto decision = logic::EvaluateTransaction(mutated, saved, verifiedReadback, ConflictsCleared());
    if (decision == logic::TransactionDecision::Rollback) {
        const bool restored = RestoreSnapshot(snapshot);
        SetStatus(g_lastStatus, sizeof(g_lastStatus), restored
            ? "Binding was not verified; previous controls were restored."
            : "Binding failed and automatic rollback could not be verified.");
        return false;
    }

    char key[48]{};
    KeyName(g_pendingCode, g_captureSlot, key, sizeof(key));
    std::snprintf(g_lastStatus, sizeof(g_lastStatus), "%s %s set to %s and verified.",
                  FindAction(g_captureAction)->label, SlotName(g_captureSlot), key);
    return true;
}

void Captured(uint32_t code) {
    g_pendingCode = code;
    FindConflicts();
    if (g_conflictCount) {
        g_conflictPending = true;
        SetStatus(g_captureStatus, sizeof(g_captureStatus), "Conflict found. Choose Replace or Cancel.");
    } else {
        CommitPendingBinding();
        SetStatus(g_captureStatus, sizeof(g_captureStatus), g_lastStatus);
        g_captureAction = -1;
    }
}

} // namespace

int ActionCount() { return static_cast<int>(std::size(kActions)); }

const ActionDescriptor& ActionAt(int index) {
    if (index < 0 || index >= ActionCount()) index = 0;
    return kActions[index];
}

bool ReadBinding(int action, BindingSlot slot, uint32_t& outCode) {
    return NativeRead(action, ControllerType(slot), outCode);
}

void FormatBinding(int action, BindingSlot slot, char* out, size_t capacity) {
    uint32_t code = BlankForType(ControllerType(slot));
    if (!NativeRead(action, ControllerType(slot), code)) {
        std::snprintf(out, capacity, "UNAVAILABLE");
        return;
    }
    KeyName(code, slot, out, capacity);
}

bool BeginCapture(int action, BindingSlot slot) {
    const ActionDescriptor* descriptor = FindAction(action);
    if (!descriptor || !descriptor->editable || g_conflictPending) return false;
    g_captureAction = action;
    g_captureSlot = slot;
    g_capture.Begin();
    std::snprintf(g_captureStatus, sizeof(g_captureStatus),
                  "Release the current input, then press a %s input. ESC cancels.",
                  slot == BindingSlot::Mouse ? "mouse" : "keyboard");
    return true;
}

void CancelCapture() {
    if (!g_capture.Active()) return;
    g_capture.phase = logic::CapturePhase::Idle;
    g_captureAction = -1;
    SetStatus(g_captureStatus, sizeof(g_captureStatus), "Binding capture cancelled.");
}

bool IsCapturing() { return g_capture.Active(); }
bool IsCaptureArmed() { return g_capture.Armed(); }
const char* CaptureStatus() { return g_captureStatus; }

bool ProcessMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_capture.Active()) return false;

    if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !wParam)) {
        logic::AdvanceCapture(g_capture, logic::CaptureEvent::FocusLost);
        g_captureAction = -1;
        SetStatus(g_captureStatus, sizeof(g_captureStatus), "Binding capture cancelled after focus loss.");
        return false;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE) {
        logic::AdvanceCapture(g_capture, logic::CaptureEvent::Escape);
        g_captureAction = -1;
        SetStatus(g_captureStatus, sizeof(g_captureStatus), "Binding capture cancelled.");
        return true;
    }
    if (msg == WM_SYSKEYDOWN && wParam == VK_TAB &&
        (static_cast<uintptr_t>(lParam) & (1u << 29)) != 0) {
        logic::AdvanceCapture(g_capture, logic::CaptureEvent::FocusLost);
        g_captureAction = -1;
        SetStatus(g_captureStatus, sizeof(g_captureStatus), "Binding capture cancelled for Alt-Tab.");
        return false; // preserve the operating-system task switch
    }

    if (!g_capture.Armed()) {
        const auto event = IsReleaseMessage(msg) ? logic::CaptureEvent::Release
                                                  : logic::CaptureEvent::Other;
        logic::AdvanceCapture(g_capture, event);
        if (g_capture.Armed())
            SetStatus(g_captureStatus, sizeof(g_captureStatus), "Press the new input now. ESC cancels.");
        return true;
    }

    uint32_t code = 0;
    bool supported = false;
    if (g_captureSlot == BindingSlot::Mouse) {
        supported = TranslateMouse(msg, wParam, code);
    } else if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
               (static_cast<uint32_t>(lParam) & (1u << 30)) == 0) {
        supported = TranslateKeyboard(wParam, lParam, code);
    }
    if (supported) {
        logic::AdvanceCapture(g_capture, logic::CaptureEvent::SupportedInput);
        Captured(code);
        return true;
    }

    switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR: case WM_SYSCHAR: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_INPUT:
            return true;
        default:
            return false;
    }
}

bool ConflictPending() { return g_conflictPending; }
const char* ConflictSummary() { return g_conflictSummary; }

bool ResolveConflict(bool replace) {
    if (!g_conflictPending) return false;
    g_conflictPending = false;
    const auto decision = logic::ChooseConflictDecision(replace);
    if (decision == logic::ConflictDecision::Cancel) {
        SetStatus(g_lastStatus, sizeof(g_lastStatus), "Binding replacement cancelled; controls were unchanged.");
        g_captureAction = -1;
        return true;
    }
    const bool result = CommitPendingBinding();
    g_captureAction = -1;
    return result;
}

bool RestoreDefaults() {
    std::array<NativeControllerAction, kActionSlots> snapshot{};
    if (!TakeSnapshot(snapshot)) {
        SetStatus(g_lastStatus, sizeof(g_lastStatus), "Defaults failed: GTA controller map unavailable.");
        return false;
    }

    bool nativeOk = true;
    __try {
        reinterpret_cast<ReinitControlsFn>(kReinitControls)(Manager());

        // GTA's verified PC mouse defaults. The native ReinitControls path is
        // device-state dependent; setting these through GTA's mouse setter
        // makes Restore deterministic for a standard mouse without raw writes.
        const auto clear = reinterpret_cast<ClearActionFn>(kClearAction);
        for (int action = 0; action < kActionSlots; ++action)
            clear(Manager(), action, static_cast<int>(BindingSlot::Mouse));
        const struct { int action; uint32_t button; } mouseDefaults[] = {
            {0, kMouseLeft}, {18, kMouseLeft},
            {45, kMouseRight}, {37, kMouseRight},
            {14, kMouseMiddle}, {36, kMouseMiddle},
            {3, kMouseWheelUp}, {2, kMouseWheelDown},
            {26, kMouseWheelUp}, {27, kMouseWheelDown},
            {8, kMouseWheelUp}, {9, kMouseWheelDown},
        };
        for (const auto& binding : mouseDefaults)
            reinterpret_cast<SetMouseButtonFn>(kSetMouseButton)(Manager(), binding.action, binding.button);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nativeOk = false;
    }

    // Menu page 36 resets the companion mouse/controller preferences; the
    // actual binding reset above is the controller manager's native path.
    const bool saved = nativeOk && settings::ApplyDefaultsNow(settings::DefaultsGroup::Controls);
    uint32_t forwardPrimary = kKeyboardBlank;
    uint32_t forwardSecondary = kKeyboardBlank;
    uint32_t fireMouse = 0;
    const bool readback = NativeRead(4, 0, forwardPrimary) && NativeRead(4, 1, forwardSecondary) &&
                          NativeRead(0, 2, fireMouse);
    const bool verified = saved && readback &&
                          (forwardPrimary != kKeyboardBlank || forwardSecondary != kKeyboardBlank) &&
                          fireMouse == kMouseLeft;
    if (!verified) {
        const bool restored = RestoreSnapshot(snapshot);
        SetStatus(g_lastStatus, sizeof(g_lastStatus), restored
            ? "Default controls were not verified; previous controls were restored."
            : "Default reset failed and automatic rollback could not be verified.");
        return false;
    }

    SetStatus(g_lastStatus, sizeof(g_lastStatus), "GTA keyboard and mouse defaults restored, saved, and read back.");
    return true;
}

const char* LastOperationStatus() { return g_lastStatus; }

} // namespace skr::native_pause::controls
