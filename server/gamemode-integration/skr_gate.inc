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
