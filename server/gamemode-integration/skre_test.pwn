// SKR Native Runtime API — roundtrip test filterscript (TASK API-001 Phase 5).
// Source: server/gamemode-integration/skre_test.pwn
// Test copy: server-runtime-branded-test/filterscripts/skre_test.pwn
//
// Roundtrip:  /skreconfirm
//   1. SKRE_ShowConfirm(playerid, TEST_REQUEST, ...)           (Pawn -> Gate)
//   2. Gate validates, sends Call(Confirm) on RPC 241           (Gate -> client)
//   3. Client shows the native dialog, player presses Y or N
//   4. Client sends Reply on RPC 240                            (client -> Gate)
//   5. Gate checks session + call id, queues the response
//   6. OnSKREConfirmResponse(playerid, TEST_REQUEST, response)  (Gate -> Pawn, onTick)
//   7. CONFIRMED: armour refill (harmless test action)
//   8. SKRE_ShowNotification(...) follow-up                    (Pawn -> Gate -> client)
//
// Other commands: /skrestatus /skrenotify /skrenumbers [on|off] /skrecancel
//                 /skrebusy (second confirm must be refused) /skrespam (rate limit)

#define FILTERSCRIPT

#include <open.mp>
#include <skr_enhanced>

#define TEST_REQUEST    (7001)
#define COLOR_OK        (0xA8EE65FF)
#define COLOR_INFO      (0x90BFB4FF)
#define COLOR_ERR       (0xF08A83FF)

public OnFilterScriptInit()
{
    print("[skre_test] loaded: /skreconfirm /skrestatus /skrenotify /skrenumbers /skrecancel /skrebusy /skrespam");
    return 1;
}

public OnSKREClientReady(playerid)
{
    printf("[skre_test] OnSKREClientReady(%d) caps=0x%x", playerid, SKRE_GetClientCapabilities(playerid));
    SendClientMessage(playerid, COLOR_OK, "[SKRE] Native API ready. Try /skreconfirm");
    SKRE_ShowNotification(playerid, "SKR Enhanced native API ready", SKRE_NOTIFY_SUCCESS, 4);
    return 1;
}

public OnSKREClientDisconnected(playerid)
{
    printf("[skre_test] OnSKREClientDisconnected(%d)", playerid);
    return 1;
}

public OnSKREConfirmResponse(playerid, requestid, SKRE_RESPONSE:response)
{
    printf("[skre_test] OnSKREConfirmResponse(%d, %d, %d)", playerid, requestid, _:response);
    if (requestid != TEST_REQUEST) return 1;

    switch (response)
    {
        case SKRE_RESPONSE_CONFIRMED:
        {
            SetPlayerArmour(playerid, 100.0);   // harmless test action, decided by the server
            SendClientMessage(playerid, COLOR_OK, "[SKRE] Confirmed -> armour refilled (roundtrip OK)");
            SKRE_ShowNotification(playerid, "Roundtrip OK: armour refilled", SKRE_NOTIFY_SUCCESS, 5);
        }
        case SKRE_RESPONSE_CANCELLED:
        {
            SendClientMessage(playerid, COLOR_INFO, "[SKRE] Cancelled by player");
            SKRE_ShowNotification(playerid, "Cancelled - nothing changed", SKRE_NOTIFY_INFO, 4);
        }
        case SKRE_RESPONSE_TIMEOUT:
        {
            SendClientMessage(playerid, COLOR_INFO, "[SKRE] Timed out");
            SKRE_ShowNotification(playerid, "Request timed out", SKRE_NOTIFY_WARNING, 4);
        }
        default:
        {
            new msg[64];
            format(msg, sizeof msg, "[SKRE] Confirm failed (response %d)", _:response);
            SendClientMessage(playerid, COLOR_ERR, msg);
        }
    }
    return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
    if (!strcmp(cmdtext, "/skrestatus", true))
    {
        new msg[96];
        format(msg, sizeof msg, "[SKRE] ready=%d caps=0x%x (notify=1 confirm=2 hud=4)",
            SKRE_IsClientReady(playerid), SKRE_GetClientCapabilities(playerid));
        SendClientMessage(playerid, COLOR_INFO, msg);
        return 1;
    }
    if (!strcmp(cmdtext, "/skreconfirm", true))
    {
        if (!SKRE_ShowConfirm(playerid, TEST_REQUEST, "SKR API Test",
                "Confirm to run the roundtrip test. The server will refill your armour.", 30))
        {
            SendClientMessage(playerid, COLOR_ERR,
                "[SKRE] ShowConfirm returned 0 (not ready, one already pending, or rate limited)");
        }
        else
        {
            SendClientMessage(playerid, COLOR_INFO, "[SKRE] Confirm sent: press Y to confirm, N to cancel");
        }
        return 1;
    }
    if (!strcmp(cmdtext, "/skrenotify", true))
    {
        new ok = SKRE_ShowNotification(playerid, "Test notification from the server", SKRE_NOTIFY_INFO, 5);
        SendClientMessage(playerid, ok ? COLOR_INFO : COLOR_ERR, ok ? "[SKRE] Notification sent" : "[SKRE] Notification refused");
        return 1;
    }
    if (!strcmp(cmdtext, "/skrenumbers on", true) || !strcmp(cmdtext, "/skrenumbers off", true))
    {
        new bool:on = !strcmp(cmdtext, "/skrenumbers on", true);
        new ok = SKRE_SetHudVisible(playerid, SKRE_HUD_VITALS_NUMBERS, on);
        SendClientMessage(playerid, ok ? COLOR_INFO : COLOR_ERR, ok ? "[SKRE] HUD value sent" : "[SKRE] HUD value refused");
        return 1;
    }
    if (!strcmp(cmdtext, "/skrecancel", true))
    {
        new ok = SKRE_CancelConfirm(playerid);
        SendClientMessage(playerid, COLOR_INFO, ok ? "[SKRE] Pending confirm withdrawn" : "[SKRE] Nothing to cancel");
        return 1;
    }
    if (!strcmp(cmdtext, "/skrebusy", true))
    {
        new first = SKRE_ShowConfirm(playerid, TEST_REQUEST, "SKR API Test", "Busy test: answer this one.", 30);
        new second = SKRE_ShowConfirm(playerid, TEST_REQUEST + 1, "Second", "This must never appear.", 30);
        new msg[96];
        format(msg, sizeof msg, "[SKRE] first=%d second=%d (expected 1 / 0)", first, second);
        SendClientMessage(playerid, (first && !second) ? COLOR_OK : COLOR_ERR, msg);
        return 1;
    }
    if (!strcmp(cmdtext, "/skrespam", true))
    {
        new sent = 0;
        for (new i = 0; i < 15; i++)
        {
            if (SKRE_ShowNotification(playerid, "Rate limit test", SKRE_NOTIFY_INFO, 1)) sent++;
        }
        new msg[96];
        format(msg, sizeof msg, "[SKRE] %d of 15 accepted (expected 10: limit 10/s)", sent);
        SendClientMessage(playerid, sent == 10 ? COLOR_OK : COLOR_ERR, msg);
        return 1;
    }
    return 0;
}
