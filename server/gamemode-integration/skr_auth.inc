// SKR Auth System — server-side pawn include
// Flow: Intro splash -> DB check -> Login (existing account) OR Register (new account)
//
// Usage in gamemode:
//   #include "skr_auth"
//
//   public OnPlayerConnect(playerid) {
//       SKR_StartAuth(playerid);
//   }
//   public OnPlayerDisconnect(playerid, reason) {
//       SKR_ResetAuth(playerid);
//   }
//   // Implement these callbacks:
//   public OnSKRAuthSuccess(playerid, const username[]) { /* spawn player */ }
//   public OnSKRAccountCheck(playerid, const name[]) {
//       // Query DB for account. When done, call:
//       // SKR_AccountExists(playerid, true/false);
//   }
//   public OnSKRLogin(playerid, const username[], const password[]) {
//       // Verify password against DB. When done, call:
//       // SKR_LoginResult(playerid, true/false, "message");
//   }
//   public OnSKRRegister(playerid, const username[], const password[]) {
//       // Insert into DB. When done, call:
//       // SKR_RegisterResult(playerid, true/false, "message");
//   }
//
// Requires: omp-cef server component (cef_create_browser, cef_emit_event,
//           cef_subscribe, cef_destroy_browser natives).

#if defined _skr_auth_included
    #endinput
#endif
#define _skr_auth_included

// Browser IDs
#define SKR_INTRO_BROWSER_ID    104
#define SKR_AUTH_BROWSER_ID     103

// Resource URLs — override before including if your paths differ.
#if !defined SKR_INTRO_URL
    #define SKR_INTRO_URL       "http://asset/skr/auth/intro.html"
#endif
#if !defined SKR_LOGIN_URL
    #define SKR_LOGIN_URL       "http://asset/skr/auth/login.html"
#endif
#if !defined SKR_REGISTER_URL
    #define SKR_REGISTER_URL    "http://asset/skr/auth/register.html"
#endif

// ── State ───────────────────────────────────────────────────────────

enum _SKR_AuthPhase {
    SKR_PHASE_NONE = 0,
    SKR_PHASE_INTRO,
    SKR_PHASE_CHECKING,
    SKR_PHASE_LOGIN,
    SKR_PHASE_REGISTER,
    SKR_PHASE_DONE
};

static _SKR_AuthPhase: g_phase[MAX_PLAYERS];
static g_authName[MAX_PLAYERS][MAX_PLAYER_NAME];
static bool: g_eventsSubscribed = false;

// ── Callbacks the gamemode must implement ────────────────────────────

forward OnSKRAuthSuccess(playerid, const username[]);
forward OnSKRAccountCheck(playerid, const name[]);
forward OnSKRLogin(playerid, const username[], const password[]);
forward OnSKRRegister(playerid, const username[], const password[]);

// ── Internal: CEF event forwards ────────────────────────────────────

forward _skr_OnIntroReady(playerid, const params[]);
forward _skr_OnAuthReady(playerid, const params[]);
forward _skr_OnAuthLogin(playerid, const params[]);
forward _skr_OnAuthRegister(playerid, const params[]);

stock _SKR_SubscribeEvents() {
    if (g_eventsSubscribed) return;
    g_eventsSubscribed = true;

    cef_subscribe("skr:intro:ready",   "_skr_OnIntroReady");
    cef_subscribe("skr:auth:ready",    "_skr_OnAuthReady");
    cef_subscribe("skr:auth:login",    "_skr_OnAuthLogin");
    cef_subscribe("skr:auth:register", "_skr_OnAuthRegister");
}

// ── Public API ──────────────────────────────────────────────────────

// Start the auth flow: show intro, then check DB, then login or register.
stock SKR_StartAuth(playerid) {
    if (!IsPlayerConnected(playerid)) return;

    _SKR_SubscribeEvents();

    // Store player name (used as account lookup key)
    GetPlayerName(playerid, g_authName[playerid], MAX_PLAYER_NAME);
    g_phase[playerid] = SKR_PHASE_INTRO;

    // Freeze player during auth
    TogglePlayerSpectating(playerid, true);

    // Show intro splash
    cef_create_browser(playerid, SKR_INTRO_BROWSER_ID, SKR_INTRO_URL, false, true);
}

// Called by gamemode after DB check: does this player have an account?
stock SKR_AccountExists(playerid, bool: exists) {
    if (g_phase[playerid] != SKR_PHASE_CHECKING) return;

    // Destroy intro browser
    cef_destroy_browser(playerid, SKR_INTRO_BROWSER_ID);

    if (exists) {
        g_phase[playerid] = SKR_PHASE_LOGIN;
        cef_create_browser(playerid, SKR_AUTH_BROWSER_ID, SKR_LOGIN_URL, false, true);
    } else {
        g_phase[playerid] = SKR_PHASE_REGISTER;
        cef_create_browser(playerid, SKR_AUTH_BROWSER_ID, SKR_REGISTER_URL, false, true);
    }
}

// Called by gamemode after verifying login credentials.
stock SKR_LoginResult(playerid, bool: success, const message[] = "") {
    if (g_phase[playerid] != SKR_PHASE_LOGIN) return;

    new json[256];
    if (success) {
        format(json, sizeof json, "{\"success\":true,\"message\":\"%s\"}", _skr_SafeMsg(message, "Autentificare reusita!"));
        cef_emit_event(playerid, "skr:auth:result", json);
        SetTimerEx("_skr_AuthComplete", 1000, false, "i", playerid);
    } else {
        format(json, sizeof json, "{\"success\":false,\"message\":\"%s\"}", _skr_SafeMsg(message, "Parola incorecta."));
        cef_emit_event(playerid, "skr:auth:result", json);
    }
}

// Called by gamemode after attempting registration.
stock SKR_RegisterResult(playerid, bool: success, const message[] = "") {
    if (g_phase[playerid] != SKR_PHASE_REGISTER) return;

    new json[256];
    if (success) {
        format(json, sizeof json, "{\"success\":true,\"message\":\"%s\"}", _skr_SafeMsg(message, "Cont creat cu succes!"));
        cef_emit_event(playerid, "skr:auth:result", json);
        SetTimerEx("_skr_AuthComplete", 1000, false, "i", playerid);
    } else {
        format(json, sizeof json, "{\"success\":false,\"message\":\"%s\"}", _skr_SafeMsg(message, "Inregistrarea a esuat."));
        cef_emit_event(playerid, "skr:auth:result", json);
    }
}

// Reset on disconnect.
stock SKR_ResetAuth(playerid) {
    if (playerid < 0 || playerid >= MAX_PLAYERS) return;
    g_phase[playerid] = SKR_PHASE_NONE;
    g_authName[playerid][0] = '\0';
}

// Query current phase.
stock bool: SKR_IsAuthenticated(playerid) {
    if (playerid < 0 || playerid >= MAX_PLAYERS) return false;
    return g_phase[playerid] == SKR_PHASE_DONE;
}

// ── Internal ────────────────────────────────────────────────────────

forward _skr_AuthComplete(playerid);
public _skr_AuthComplete(playerid) {
    if (!IsPlayerConnected(playerid)) return;
    g_phase[playerid] = SKR_PHASE_DONE;

    cef_destroy_browser(playerid, SKR_AUTH_BROWSER_ID);
    TogglePlayerSpectating(playerid, false);

    // PHONE-M4E: mark player as authenticated for the Gate phone backend.
    // Must happen before OnSKRAuthSuccess so phone ops are available immediately.
    #if defined _skr_enhanced_included
        SKRE_PlayerAuthenticated(playerid, g_authName[playerid]);
    #endif

    CallLocalFunction("OnSKRAuthSuccess", "is", playerid, g_authName[playerid]);
}

// Intro page loaded — give it a moment, then start DB check.
public _skr_OnIntroReady(playerid, const params[]) {
    if (g_phase[playerid] != SKR_PHASE_INTRO) return;

    // Update intro status
    cef_emit_event(playerid, "skr:intro:status", "{\"text\":\"Se verifica contul\"}");

    g_phase[playerid] = SKR_PHASE_CHECKING;

    // Ask gamemode to check DB
    CallLocalFunction("OnSKRAccountCheck", "is", playerid, g_authName[playerid]);
}

// Auth page (login or register) loaded — send player name.
public _skr_OnAuthReady(playerid, const params[]) {
    if (g_phase[playerid] != SKR_PHASE_LOGIN && g_phase[playerid] != SKR_PHASE_REGISTER) return;

    new json[128];
    format(json, sizeof json, "{\"name\":\"%s\"}", g_authName[playerid]);
    cef_emit_event(playerid, "skr:auth:init", json);
}

// Login attempt from client JS.
public _skr_OnAuthLogin(playerid, const params[]) {
    if (g_phase[playerid] != SKR_PHASE_LOGIN) return;

    new username[MAX_PLAYER_NAME], password[65];
    _skr_JsonString(params, "username", username, sizeof username);
    _skr_JsonString(params, "password", password, sizeof password);

    // Delegate to gamemode
    CallLocalFunction("OnSKRLogin", "iss", playerid, username, password);
}

// Register attempt from client JS.
public _skr_OnAuthRegister(playerid, const params[]) {
    if (g_phase[playerid] != SKR_PHASE_REGISTER) return;

    new username[MAX_PLAYER_NAME], password[65];
    _skr_JsonString(params, "username", username, sizeof username);
    _skr_JsonString(params, "password", password, sizeof password);

    // Delegate to gamemode
    CallLocalFunction("OnSKRRegister", "iss", playerid, username, password);
}

// ── Helpers ─────────────────────────────────────────────────────────

static stock _skr_SafeMsg(const msg[], const fallback[]) {
    if (strlen(msg) > 0) return msg;
    return fallback;
}

static stock _skr_JsonString(const json[], const key[], dest[], maxlen) {
    dest[0] = '\0';
    new keyPattern[64];
    format(keyPattern, sizeof keyPattern, "\"%s\":\"", key);

    new pos = strfind(json, keyPattern);
    if (pos == -1) return;

    pos += strlen(keyPattern);
    new i = 0;
    while (pos < strlen(json) && json[pos] != '"' && i < maxlen - 1) {
        if (json[pos] == '\\' && pos + 1 < strlen(json)) {
            pos++;
        }
        dest[i++] = json[pos++];
    }
    dest[i] = '\0';
}
