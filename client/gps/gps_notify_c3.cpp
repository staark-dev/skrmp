// GPS notification sink → Codex C3 Native API toast renderer.
//
// BLOCKED — C3 NOT IN CURRENT TREE. This file needs the C3 version of
// native-api/native_api_ui.h (PushLocalNotification, LocalNotificationStyle)
// from codex/c3-native-api-ui. It is compiled only with the CMake option
// SKR_GPS_C3_BRIDGE=ON, which must stay OFF until that branch is merged.
// Verified to compile against the C3 headers (build-gate target
// SKRGpsC3BridgeCheck with -DSKR_C3_TREE=<C3 worktree>).
//
// C3 ticks its toast queue only while the Native API session is Ready
// (native_api.cpp Tick). A toast pushed outside that state would never age,
// so the sink drops notices unless the session is Ready.

#include "gps_notify.h"

#include "native-api/native_api.h"
#include "native-api/native_api_ui.h"

namespace skr::gps::notify {
namespace {

void C3Sink(const Notice& n, void*) {
    if (!native_api::IsReady()) return;
    using Style = native_api::ui::LocalNotificationStyle;
    const Style style = n.kind == Kind::Success ? Style::Success
                      : n.kind == Kind::Error   ? Style::Error
                                                : Style::GpsNavigation;
    native_api::ui::PushLocalNotification(style, n.durationSec, n.text);
}

} // namespace

void InstallC3Sink() { SetSink(&C3Sink, nullptr); }

} // namespace skr::gps::notify
