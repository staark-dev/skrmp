#pragma once
#include "protocol/skrx_registry.h"
#include "protocol/skrx_gateway.h"
#include <cstdint>

// SKR-X client-side: registry + gateway dispatch + CORE Ping/Pong.
//
// Lifecycle:
//   Init()                    — on DLL attach, creates registry with builtin entries
//   SetGatewayReady(true)     — called when CapGateway is negotiated in API Welcome
//   SendPing()                — sends CORE:Ping (rejected if gateway not ready)
//   OnGatewayMessage()        — called when RPC 241 carries envelope step 10
//   Tick()                    — timeout detection + log flush (call from game tick)
//   Reset()                   — on disconnect, flushes log and clears session state
//
// Threading: all calls happen on the GTA main thread (RakClient RPC
// handlers and CGame::Process tick are both on the main thread).

namespace skr::skrx {

constexpr uint32_t kPingTimeoutMs = 5000;

struct SkrXState {
    bool gatewayReady = false;
    bool pendingPing = false;
    uint32_t lastPingSentMs = 0;
    uint32_t pendingEcho = 0;
    uint32_t lastRttMs = 0;
    uint32_t pingCount = 0;
    uint32_t pongCount = 0;
    uint32_t timeoutCount = 0;
    uint32_t unsolicitedPongCount = 0;
    uint32_t stalePongCount = 0;
};

bool Init();
void Reset();
void Tick();

void SetRakClient(void* rakClient);
void SetGatewayReady(bool ready);
bool IsGatewayReady();

const protocol::skrx::Registry& GetRegistry();
SkrXState& GetState();

void OnGatewayMessage(const uint8_t* data, size_t len);
bool SendPing();

} // namespace skr::skrx
