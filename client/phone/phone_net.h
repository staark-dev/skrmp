#pragma once
// PHONE-M4F — Phone network layer (client side).
//
// One negotiated transport: PHONE Gateway v1 or legacy ClientRequest/Call.
// Existing typed phone bodies update the same Model. Server-authoritative: the
// model only ever holds what the server returned; an outgoing SMS stays
// Sending until SmsSendResult arrives.
//
// Requests go through a small paced queue (the Gate drops client requests
// above 10/s per player) and every list request is tracked by request id, so
// a late response for an older request is ignored. GTA thread only: called
// from native_api (RPC dispatch + CGame::Process tick) and the phone input.

#include <cstdint>
#include <cstddef>
#include "protocol/skrx_gateway.h"

namespace skr::phone::net {

// Called once when native_api reaches Ready state with CapPhone negotiated.
void OnPhoneReady(uint32_t sessionId, bool setupSupported = false, bool gatewayV1 = false);
void OnPhoneGatewayMessage(const protocol::skrx::GatewayHeader&,const uint8_t*,size_t);
bool RequestProfile();
bool SaveProfile(uint8_t stage, uint8_t wallpaper, uint8_t scale, bool banners);

// Called on disconnect / session reset. Drops queued and pending requests.
void Reset();

// Per-frame: drains the paced send queue and expires timed-out requests.
void Tick(uint64_t now);

// Dispatch a legacy phone Call; rejected while Gateway v1 is selected.
// Returns true if handled, false if unknown op.
bool OnPhoneCall(uint8_t op, uint32_t callId, const uint8_t* payload, size_t payloadLen);

// ── UI actions ──────────────────────────────────────────────────────
// All return false when nothing was queued (phone offline, invalid input,
// queue full). None of them blocks.
bool RequestSync();
bool RefreshContacts();
bool RefreshThreads();
bool OpenConversation(const char* peerNumber, const char* peerName, uint8_t hue);
bool LoadOlderHistory();
bool AddContact(const char* number, const char* displayName);
bool RemoveContact(const char* number);
bool SendSms(const char* recipientNumber, const char* text);

// True when the phone network layer is active (CapPhone negotiated).
bool IsActive();
void MarkConversationRead();

} // namespace skr::phone::net
