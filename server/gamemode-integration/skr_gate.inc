// SKR Gate natives (open.mp component SKRGate.dll). Protocol: shared/protocol/radar_style.h
#if defined _skr_gate_included
    #endinput
#endif
#define _skr_gate_included

enum SKR_RADAR_STYLE {
    SKR_RADAR_STYLE_ROUND = 0,   // native GTA: circular mask + black ring
    SKR_RADAR_STYLE_SQUARE = 1   // rectangle, no mask, no ring
}

enum SKR_RADAR_SIZE {
    SKR_RADAR_SIZE_DEFAULT = 0,  // native GTA size (scale ignored)
    SKR_RADAR_SIZE_CUSTOM = 1    // native size * scale, scale in [0.5, 2.0]
}

// Both can be called from OnPlayerConnect: the value is stored and sent once the
// client is ready. Return 0 for an unknown/rejected player or invalid arguments.
native SKR_SetRadarStyle(playerid, SKR_RADAR_STYLE:style);
native SKR_SetRadarSize(playerid, SKR_RADAR_SIZE:mode, Float:scale = 1.0);

// Returns the radar's screen-space rectangle in pixels (origin top-left).
// Only valid after the client has reported it (returns 0 before then).
// The value comes from the client: use it only for UI alignment, never for logic.
native SKR_GetRadarRect(playerid, &Float:x, &Float:y, &Float:w, &Float:h);

// Fired whenever a player's radar screen-space rectangle changes (resolution,
// scale, or aspect ratio change).  Use it to reposition CEF HUD elements.
forward OnPlayerRadarRect(playerid, Float:x, Float:y, Float:w, Float:h);
