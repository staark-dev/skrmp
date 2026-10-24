// PHONE-M4E test filterscript — server-side phone commands.
// /phoneauth  — authenticate (manual, skr_auth.inc not loaded here)
// /psync      — assign/show your phone number
// /psms <id> <text> — send SMS to another player
// /phonestatus — show caps and auth state

#define FILTERSCRIPT

#include <open.mp>
#include <skr_enhanced>

#define COLOR_OK     (0xA8EE65FF)
#define COLOR_ERR    (0xFF4444FF)

public OnFilterScriptInit()
{
    SKRE_PhoneEnable(true);
    printf("[phone_test] phone enabled, cap=0x%x", SKRE_CAP_PHONE);
    return 1;
}

public OnSKREClientReady(playerid)
{
    new caps = SKRE_GetClientCapabilities(playerid);
    if (caps & SKRE_CAP_PHONE) {
        SendClientMessage(playerid, COLOR_OK, "[phone_test] Phone capability negotiated");
    }
    return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
    if (!strcmp(cmdtext, "/phoneauth", true))
    {
        new name[MAX_PLAYER_NAME];
        GetPlayerName(playerid, name, sizeof name);
        new ok = SKRE_PlayerAuthenticated(playerid, name);
        new msg[128];
        format(msg, sizeof msg, "[phone] auth=%d is_auth=%d", ok, SKRE_IsPlayerAuthenticated(playerid));
        SendClientMessage(playerid, COLOR_OK, msg);
        return 1;
    }

    if (!strcmp(cmdtext, "/psync", true))
    {
        if (!SKRE_IsPlayerAuthenticated(playerid)) {
            SendClientMessage(playerid, COLOR_ERR, "[phone] authenticate first: /phoneauth");
            return 1;
        }
        SKRE_TestPhoneSync(playerid);
        return 1;
    }

    if (!strfind(cmdtext, "/psms ", true))
    {
        if (!SKRE_IsPlayerAuthenticated(playerid)) {
            SendClientMessage(playerid, COLOR_ERR, "[phone] authenticate first: /phoneauth");
            return 1;
        }

        new tmp[200];
        strmid(tmp, cmdtext, 6, sizeof tmp);

        // Parse: <targetid> <text>
        new targetid = -1, pos = 0;
        while (tmp[pos] >= '0' && tmp[pos] <= '9') {
            targetid = (targetid < 0 ? 0 : targetid) * 10 + (tmp[pos] - '0');
            pos++;
        }
        if (targetid < 0 || tmp[pos] != ' ') {
            SendClientMessage(playerid, COLOR_ERR, "[phone] usage: /psms <playerid> <message>");
            return 1;
        }
        pos++; // skip space

        if (tmp[pos] == '\0') {
            SendClientMessage(playerid, COLOR_ERR, "[phone] usage: /psms <playerid> <message>");
            return 1;
        }

        new text[121];
        strmid(text, tmp, pos, sizeof text);
        SKRE_TestSmsSend(playerid, targetid, text);
        return 1;
    }

    if (!strcmp(cmdtext, "/phonestatus", true))
    {
        new caps = SKRE_GetClientCapabilities(playerid);
        new msg[128];
        format(msg, sizeof msg, "[phone] ready=%d auth=%d caps=0x%x phone=%d",
            SKRE_IsClientReady(playerid),
            SKRE_IsPlayerAuthenticated(playerid),
            caps,
            (caps & SKRE_CAP_PHONE) ? 1 : 0);
        SendClientMessage(playerid, COLOR_OK, msg);
        return 1;
    }

    return 0;
}
