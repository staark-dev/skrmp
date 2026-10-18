#pragma once
// Loads omp-cef's local API exports at runtime via GetProcAddress.
// Populates an InternalApi struct so CefBridge works without samp-cef.

namespace skr::cef_local {

// Try to locate and load omp-cef's client.dll (in GTA's cef/ folder).
// Returns true if all required exports were resolved.
// On success, CefBridge::Initialize is called with the resolved API.
bool TryLoad();

// True once TryLoad succeeded.
bool IsLoaded();

} // namespace skr::cef_local
