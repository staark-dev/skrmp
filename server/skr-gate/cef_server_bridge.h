#pragma once
#include <cstdint>

// Server-side bridge for samp-cef open.mp component.
// samp-cef provides cef-openmp as a native open.mp component that exposes
// Pawn natives for creating/destroying browsers and emitting events.
// This header defines the SKR gamemode interface for sending CEF commands.
// Actual integration depends on the samp-cef server plugin being loaded.

namespace skr::server {

// These map to samp-cef Pawn natives exposed by the open.mp component.
// The gamemode calls them via AMX; this stub documents the contract.
//
// native cef_create_browser(playerid, browser_id, const url[], hidden, focused);
// native cef_destroy_browser(playerid, browser_id);
// native cef_hide_browser(playerid, browser_id, hide);
// native cef_emit_event(playerid, const event_name[], args...);
// native cef_subscribe(const event_name[], const callback[]);
// native cef_player_has_plugin(playerid);

} // namespace skr::server
