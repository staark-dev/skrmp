#pragma once
// PHONE-001 — SKR Street Phone: native ImGui draw-list renderer.
//
// Draws the device shell and the active app pages into an existing ImDrawList
// (the single ImGui owner in native-pause/pause_renderer.cpp in game; a
// preview harness offscreen). No windows, no widgets, no textures, no I/O and
// no allocation: everything is draw-list geometry from the measured Layout.
//
// Interaction is immediate-mode: while the controller accepts clicks, every
// tappable region is recorded in FrameHits for the input layer (M2) and the
// region under the pointer is drawn hovered / pressed.

#include <cstdint>

#include "phone/phone_controller.h"
#include "phone/phone_layout.h"
#include "phone/phone_model.h"

struct ImDrawList;
struct ImFont;

namespace skr::phone {

// One family = faces of the same TTF at increasing raster sizes (nullptr =
// absent). Each text size is drawn with the smallest face that is at least
// 85 % of it, so 8-12 px phone text is never a 40 px glyph minified 4-5x
// (thin stems such as "I" vanish that way). The ImGui owner rasterises the
// small faces for the current backbuffer height and rebuilds them on resize.
struct Fonts {
    ImFont* heading[3] = {};      // bahnschrift / arialbd
    ImFont* body[3] = {};         // segoeui / arial
};

struct Pointer {
    float x = -1, y = -1;
    bool down = false;
};

enum class Action : uint8_t {
    None = 0,
    OpenApp,          // index = AppId
    Back,
    Home,
    OpenThread,       // index = thread
    OpenContact,      // index = contact
    AcceptCall,
    DeclineCall,
    SelectDestination,// index = destination
    ClearRoute,
    ToggleSetting,    // index = setting row
    Compose,          // new message (Messages page 0)
    DialKey,          // index = key 0..11 (1-9, *, 0, #) — reserved, calls not available
    PlaceCall,        // reserved, calls not available
    // PHONE-M4F
    FocusField,       // index = Field
    SendSms,
    CopyNumber,
    AddContact,       // open the add form
    SaveContact,
    MessageContact,   // open a conversation with the contact on page 1
    RemoveContact,    // ask for confirmation
    ConfirmRemove,
    CancelRemove,
    ComposeNext,      // new message: recipient entered
    Retry,            // retry PhoneSync / reload the current list
    LoadOlder,        // older SMS history page
    OpenGpsMap,
    SetupNext,
    SetupWallpaper,
    ProfileRetry,
    OpenBanner,       // notification banner -> its conversation
};

struct Hit {
    Rect rect;
    Action action = Action::None;
    uint8_t index = 0;
};

struct FrameHits {
    static constexpr int kMax = 64;
    Hit items[kMax];
    int count = 0;
    const Hit* At(float x, float y) const;   // topmost region under the point
};

// Stable press-feedback id for Controller::Press / PressAmount.
inline uint16_t HitId(Action a, uint8_t index) {
    return static_cast<uint16_t>((static_cast<uint16_t>(a) << 8) | index);
}

// Wallpaper image (ImTextureID of a loaded texture, see phone_wallpaper.h);
// nullptr = procedural wallpaper. Cover-cropped into the screen, never stretched.
void SetWallpaperTexture(void* texture, float width, float height);

const char* AppLabel(AppId app);
Rect AppIconRect(const Layout& L, AppId app);

void Draw(ImDrawList* dl, const Layout& L, const Controller& c, const Model& m, Views& v,
          const Fonts& f, const Pointer& p, uint64_t now, FrameHits* hits);

} // namespace skr::phone
