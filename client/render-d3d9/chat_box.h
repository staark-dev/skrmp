#pragma once

namespace skr::render {

// Draws an SKR panel behind the native SA-MP chat, sized to the visible text.
// Redirects verified call sites in samp.dll; call once samp.dll is loaded.
bool InstallChatBoxHook();

} // namespace skr::render
