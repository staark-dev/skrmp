// SKR-X VEHICLES M2.1 — Test filterscript
// Source: server/gamemode-integration/skrx_vehicles_test.pwn
//
// Commands:
//   /v <modelid>   — create vehicle (560 = stock Sultan, 612 = Volvo XC60)
//   /dv            — destroy last created vehicle
//   /vm <vehid>    — query true model ID for vehicle
//   /vcount        — show custom vehicle count + assignment count
//
// Stock models pass through to standard CreateVehicle for regression control.
// Custom models (612+) use SKRX_CreateVehicle and the VEHICLES protocol.

#define FILTERSCRIPT

#include <open.mp>
#include <skr_enhanced>
#include <skrx_vehicles>

#define COLOR_OK   (0xA8EE65FF)
#define COLOR_INFO (0x90BFB4FF)
#define COLOR_ERR  (0xF08A83FF)

static lastVehicle[MAX_PLAYERS] = {INVALID_VEHICLE_ID, ...};

public OnFilterScriptInit()
{
    print("[skrx_vehicles_test] loaded: /v <model> /dv /vm <vehid> /vcount");
    return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
    new cmd[16], params[32];
    sscanf(cmdtext, "s[16]s[32]", cmd, params);

    // /v <modelid>
    if (!strcmp(cmd, "/v"))
    {
        if (!strlen(params))
        {
            SendClientMessage(playerid, COLOR_INFO, "Usage: /v <modelid>  (560 = Sultan, 612 = Volvo XC60)");
            return 1;
        }

        new modelid = strval(params);
        if (modelid < 400)
        {
            SendClientMessage(playerid, COLOR_ERR, "Invalid model ID (must be >= 400)");
            return 1;
        }

        new Float:x, Float:y, Float:z, Float:a;
        GetPlayerPos(playerid, x, y, z);
        GetPlayerFacingAngle(playerid, a);

        new vid;
        if (modelid >= 612 && SKRX_IsCustomModel(modelid))
        {
            vid = SKRX_CreateVehicle(modelid, x + 3.0, y, z, a, -1, -1, 300);
        }
        else
        {
            vid = CreateVehicle(modelid, x + 3.0, y, z, a, -1, -1, 300);
        }

        if (vid == INVALID_VEHICLE_ID)
        {
            new msg[64];
            format(msg, sizeof(msg), "Failed to create vehicle model %d", modelid);
            SendClientMessage(playerid, COLOR_ERR, msg);
            return 1;
        }

        PutPlayerInVehicle(playerid, vid, 0);
        lastVehicle[playerid] = vid;

        new msg[80];
        new trueModel = (modelid >= 612) ? SKRX_GetVehicleModel(vid) : GetVehicleModel(vid);
        format(msg, sizeof(msg), "Created vehicle %d (model %d, true model %d)", vid, modelid, trueModel);
        SendClientMessage(playerid, COLOR_OK, msg);
        return 1;
    }

    // /dv — destroy last vehicle
    if (!strcmp(cmd, "/dv"))
    {
        new vid = lastVehicle[playerid];
        if (vid == INVALID_VEHICLE_ID)
        {
            SendClientMessage(playerid, COLOR_ERR, "No vehicle to destroy");
            return 1;
        }

        new trueModel = SKRX_GetVehicleModel(vid);
        new bool:destroyed;
        if (trueModel >= 612)
            destroyed = SKRX_DestroyVehicle(vid);
        else
            destroyed = bool:DestroyVehicle(vid);

        if (destroyed)
        {
            new msg[64];
            format(msg, sizeof(msg), "Destroyed vehicle %d (model %d)", vid, trueModel);
            SendClientMessage(playerid, COLOR_OK, msg);
            lastVehicle[playerid] = INVALID_VEHICLE_ID;
        }
        else
        {
            SendClientMessage(playerid, COLOR_ERR, "DestroyVehicle failed");
        }
        return 1;
    }

    // /vm <vehid> — query model
    if (!strcmp(cmd, "/vm"))
    {
        if (!strlen(params))
        {
            SendClientMessage(playerid, COLOR_INFO, "Usage: /vm <vehicleid>");
            return 1;
        }

        new vid = strval(params);
        new trueModel = SKRX_GetVehicleModel(vid);
        new stockModel = GetVehicleModel(vid);
        new bool:isCustom = SKRX_IsCustomModel(trueModel);
        new bool:ready = SKRX_IsVehicleReadyForPlayer(playerid, vid);

        new msg[128];
        format(msg, sizeof(msg),
            "Vehicle %d: true=%d stock=%d custom=%s ready=%s",
            vid, trueModel, stockModel,
            isCustom ? "yes" : "no",
            ready ? "yes" : "no");
        SendClientMessage(playerid, COLOR_INFO, msg);
        return 1;
    }

    // /vcount — stats
    if (!strcmp(cmd, "/vcount"))
    {
        SendClientMessage(playerid, COLOR_INFO, "SKRX vehicle system active (check server console for details)");
        return 1;
    }

    return 0;
}

public OnPlayerDisconnect(playerid, reason)
{
    lastVehicle[playerid] = INVALID_VEHICLE_ID;
    return 1;
}
