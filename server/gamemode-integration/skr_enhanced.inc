// SKR Enhanced — Native Runtime API for Pawn (TASK API-001, API v1)
// Implemented by the SKR Gate open.mp component (Gate.dll).
// Protocol: docs/protocol/SKR_NATIVE_API.md
//
// The API only exists for players running the SKR Enhanced client on an SKR
// Runtime server. For everyone else SKRE_IsClientReady() stays 0 and every
// request native returns 0 without sending anything, so scripts can call
// these unconditionally and fall back to ShowPlayerDialog/SendClientMessage.
//
// Lifecycle:
//   OnPlayerConnect -> (Enhanced handshake) -> OnSKREClientReady(playerid)
//   ... requests ...
//   disconnect -> OnSKREClientDisconnected(playerid)
//                 (any pending confirm is dropped; no OnSKREConfirmResponse)
//
// All callbacks run on the server main thread, never inside packet handling,
// so calling any SKRE_* native from inside a callback is safe.
//
// Client answers are player input, not authority: always re-check on the
// server whatever the confirmed action depends on (money, position, ...).

#if defined _skr_enhanced_included
    #endinput
#endif
#define _skr_enhanced_included

#define SKRE_API_VERSION            (1)

// Text limits (bytes). Longer text makes the native return 0.
#define SKRE_MAX_NOTIFY_TEXT        (144)
#define SKRE_MAX_CONFIRM_TITLE      (64)
#define SKRE_MAX_CONFIRM_MESSAGE    (200)
// Characters below 0x20, 0x7F and '~' are replaced with spaces.

// Capability bits returned by SKRE_GetClientCapabilities.
#define SKRE_CAP_NOTIFY             (1)
#define SKRE_CAP_CONFIRM            (2)
#define SKRE_CAP_HUD_VALUE          (4)

enum SKRE_NOTIFY {
    SKRE_NOTIFY_INFO    = 0,
    SKRE_NOTIFY_SUCCESS = 1,
    SKRE_NOTIFY_WARNING = 2,
    SKRE_NOTIFY_ERROR   = 3
}

enum SKRE_RESPONSE {
    SKRE_RESPONSE_CONFIRMED   = 1,
    SKRE_RESPONSE_CANCELLED   = 2,   // player chose Cancel
    SKRE_RESPONSE_BUSY        = 3,   // client was already showing a dialog
    SKRE_RESPONSE_TIMEOUT     = 4,   // no answer before the timeout
    SKRE_RESPONSE_UNSUPPORTED = 5,
    SKRE_RESPONSE_ERROR       = 6    // client rejected the request
}

enum SKRE_HUD {
    // Numeric health/armour next to the SKR vitals strip. Session-only
    // override of the player's own setting; cleared on disconnect. Only
    // visible while the player uses the SKR vitals strip (hud.ini default).
    SKRE_HUD_VITALS_NUMBERS = 1
}

// 1 once the player's client completed the API handshake (after OnSKREClientReady).
native SKRE_IsClientReady(playerid);

// Negotiated SKRE_CAP_* bits, 0 when not ready.
native SKRE_GetClientCapabilities(playerid);

// Non-blocking toast. duration 1..15 seconds.
// Returns 0: not ready, invalid args, text empty/too long, or more than
// 10 requests/second to this player.
native SKRE_ShowNotification(playerid, const text[], SKRE_NOTIFY:kind = SKRE_NOTIFY_INFO, duration = 5);

// Modal confirmation. Exactly one OnSKREConfirmResponse(playerid, requestid, ...)
// follows a successful call, unless the player disconnects or you cancel it.
// requestid is yours, echoed back unchanged. timeout 5..120 seconds.
// Returns 0: not ready, invalid args, another confirm is still pending for
// this player, or rate limited.
native SKRE_ShowConfirm(playerid, requestid, const title[], const message[], timeout = 30);

// Withdraws the pending confirm (closes the client dialog). No response callback.
native SKRE_CancelConfirm(playerid);

// Sets one supported HUD value. May be called before OnSKREClientReady (e.g. in
// OnPlayerConnect): the value is stored and sent when the client is ready.
// Returns 0 for an unknown component or unknown player.
native SKRE_SetHudVisible(playerid, SKRE_HUD:component, bool:visible);

forward OnSKREClientReady(playerid);
forward OnSKREClientDisconnected(playerid);
forward OnSKREConfirmResponse(playerid, requestid, SKRE_RESPONSE:response);

// ── Phone backend (PHONE-M4E) ──────────────────────────────────────

#define SKRE_CAP_PHONE              (8)

// Tells the Gate that the phone backend is active. Call once during init.
// When enabled, clients that request CapPhone in Hello receive it in Welcome.
// When disabled (default), CapPhone is masked out: no phone ops reach the Gate.
native SKRE_PhoneEnable(bool:enable = true);

// Marks a player as authenticated with the given username.
// Call after OnSKRAuthSuccess(playerid, username[]). The Gate cross-checks the
// username against IPlayer::getName() — a mismatch returns 0.
// Phone operations are rejected until this succeeds.
native SKRE_PlayerAuthenticated(playerid, const username[]);

// 1 when the player has been authenticated via SKRE_PlayerAuthenticated.
native SKRE_IsPlayerAuthenticated(playerid);

// ── Phone test natives (server-side, no client protocol needed) ───
native SKRE_TestPhoneSync(playerid);
native SKRE_TestSmsSend(playerid, targetid, const text[]);

/*
Example:

    #define DLG_TELEPORT (1)

    public OnSKREClientReady(playerid)
    {
        SKRE_ShowNotification(playerid, "SKR Enhanced features enabled", SKRE_NOTIFY_SUCCESS);
        return 1;
    }

    CMD:gohome(playerid)
    {
        if (!SKRE_ShowConfirm(playerid, DLG_TELEPORT, "Teleport", "Go back to your house?"))
            ShowPlayerDialog(playerid, DLG_TELEPORT, DIALOG_STYLE_MSGBOX, "Teleport",
                "Go back to your house?", "Yes", "No");   // vanilla fallback
        return 1;
    }

    public OnSKREConfirmResponse(playerid, requestid, SKRE_RESPONSE:response)
    {
        if (requestid == DLG_TELEPORT && response == SKRE_RESPONSE_CONFIRMED)
            SetPlayerPos(playerid, 2495.0, -1687.0, 13.5);
        return 1;
    }
*/
