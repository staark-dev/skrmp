#pragma once

#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>
#include <Server/Components/Databases/databases.hpp>
#include <Server/Components/Vehicles/vehicles.hpp>
#include "protocol/messages.h"
#include "protocol/enhanced.h"
#include "protocol/client_state.h"
#include "protocol/session_info.h"
#include "protocol/query_rules.h"
#include "protocol/radar_style.h"
#include "protocol/radar_rect.h"
#include "protocol/native_api.h"
#include "protocol/rejection.h"
#include "protocol/skrx_gateway.h"
#include "protocol/skrx_vehicle.h"
#include "skrx_vehicles_server.h"
#include "crypto/crypto.h"
#include "client_policy.hpp"
#include "phone_worker.h"
#include <bitstream.hpp>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace skr::gate_omp {

enum class SkrPlayerState : uint8_t {
    Connected,
    Challenged,
    Authorized,
    Rejected
};

// Native Runtime API session (TASK API-001, native_api_server.cpp). Lives only as
// long as the connection: SkrPlayerData is erased on disconnect, so nothing from
// an earlier connection (session id, pending confirm) survives a reconnect.
struct SkrApiState {
    bool ready = false;              // Welcome sent: API usable for this connection
    bool quarantined = false;        // too many invalid messages: API traffic ignored
    uint8_t version = 0;
    uint32_t sessionId = 0;
    uint32_t capabilities = 0;       // negotiated (client offer & server support)
    uint32_t nextCallId = 1;
    uint32_t invalidMessages = 0;
    protocol::api::PendingConfirm confirm;
    protocol::api::RateLimiter callLimiter;
    protocol::api::RateLimiter replyLimiter;
    int8_t hudVitalsNumbers = -1;    // script override; -1 = player's own hud.ini setting
};

struct SkrPlayerData {
    protocol::EnhancedSession enhanced;
    SkrApiState api;
    SkrPlayerState state = SkrPlayerState::Connected;
    uint8_t connectionId[protocol::kConnectionIdSize]{};
    uint8_t nonce[protocol::kNonceSize]{};
    uint64_t generation = 0;
    uint64_t challengeExpiresAt = 0;
    uint64_t enhancedAdmissionDeadlineMs = 0;
    bool enhancedRequired = false;
    uint32_t accountId = 0;

    bool authenticated = false;
    char authName[25] = {};

    bool sessionInfoSent = false;

    // Radar (RPC 242): last values set by scripts; sent once session info is out.
    protocol::RadarStyle radarStyle = protocol::RadarStyle::Round;
    protocol::RadarSizeMode radarSizeMode = protocol::RadarSizeMode::Default;
    float radarScale = 1.0f;
    float radarScaleW = 1.0f;  // independent width scale (Bounds mode)
    float radarScaleH = 1.0f;  // independent height scale (Bounds mode)
    bool radarVisible = true;  // minimap visibility (from hud.enableMinimap)

    // Radar rect (RPC 243): last reported screen-space rectangle from the client.
    float radarRectX = 0.0f, radarRectY = 0.0f;
    float radarRectW = 0.0f, radarRectH = 0.0f;
    bool radarRectReceived = false;

    // AC tracking
    uint32_t lastAcBeatSeq = 0;
    uint64_t lastAcBeatMs = 0;
    uint32_t missedAcBeats = 0;

    void Reset() {
        enhanced = {};
        api = {};
        state = SkrPlayerState::Connected;
        generation = 0;
        challengeExpiresAt = 0;
        enhancedAdmissionDeadlineMs = 0;
        enhancedRequired = false;
        accountId = 0;
        authenticated = false;
        memset(authName, 0, sizeof(authName));
        sessionInfoSent = false;
        radarStyle = protocol::RadarStyle::Round;
        radarSizeMode = protocol::RadarSizeMode::Default;
        radarScale = 1.0f;
        radarScaleW = 1.0f;
        radarScaleH = 1.0f;
        radarVisible = true;
        radarRectX = radarRectY = radarRectW = radarRectH = 0.0f;
        radarRectReceived = false;
        lastAcBeatSeq = 0;
        lastAcBeatMs = 0;
        missedAcBeats = 0;
        memset(connectionId, 0, protocol::kConnectionIdSize);
        memset(nonce, 0, protocol::kNonceSize);
    }
};

class SkrGateComponent;

class SkrJoinFilter final : public SingleNetworkInEventHandler {
public:
    bool onReceive(IPlayer& peer, NetworkBitStream& bs) override;
    void setOwner(SkrGateComponent* gate) { gate_ = gate; }
private:
    SkrGateComponent* gate_ = nullptr;
};

// RPC 240 (shared/protocol/client_state.h): display-only client state from SKRClient.dll.
// Validated, rate-limited and exposed to scripts as player variables (PVars):
//   skr_state_seq (int, increments per accepted packet), skr_x/y/z, skr_heading,
//   skr_range, skr_orient (float), skr_speed, skr_gear, skr_maxspeed (int),
//   skr_city, skr_area (string).
class SkrClientStateHandler final : public SingleNetworkInEventHandler {
public:
    bool onReceive(IPlayer& peer, NetworkBitStream& bs) override;
    void setOwner(SkrGateComponent* gate, ICore* core) { gate_ = gate; core_ = core; }
    void forget(int playerId) { lastAcceptMs_.erase(playerId); seq_.erase(playerId); }
private:
    SkrGateComponent* gate_ = nullptr;
    ICore* core_ = nullptr;
    std::unordered_map<int, uint64_t> lastAcceptMs_;
    std::unordered_map<int, int> seq_;
};

// RPC 243 (shared/protocol/radar_rect.h): client reports its radar's screen-space rectangle.
class SkrRadarRectHandler final : public SingleNetworkInEventHandler {
public:
    bool onReceive(IPlayer& peer, NetworkBitStream& bs) override;
    void setOwner(SkrGateComponent* gate, ICore* core) { gate_ = gate; core_ = core; }
    void forget(int playerId) { lastAcceptMs_.erase(playerId); }
private:
    SkrGateComponent* gate_ = nullptr;
    ICore* core_ = nullptr;
    std::unordered_map<int, uint64_t> lastAcceptMs_;
};

class SkrGateComponent final
    : public IComponent
    , public PlayerConnectEventHandler
    , public CoreEventHandler
    , public PawnEventHandler
    , public VehicleEventHandler
{
public:
    // Visit https://open.mp/uid to generate a new unique ID.
    // Using a placeholder UID — replace with a real one from open.mp/uid before production.
    PROVIDE_UID(0x534B5247); // "SKRG" in hex

    ~SkrGateComponent() override;

    // IComponent
    StringView componentName() const override { return "SKR Gate"; }
    SemanticVersion componentVersion() const override { return SemanticVersion(0, 1, 0, 0); }

    void onLoad(ICore* c) override;
    void onInit(IComponentList* components) override;
    void onReady() override;
    void onFree(IComponent* component) override;
    void free() override;
    void reset() override;

    // PlayerConnectEventHandler
    void onIncomingConnection(IPlayer& player, StringView ipAddress, unsigned short port) override;
    void onPlayerConnect(IPlayer& player) override;
    void onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) override;
    void onPlayerClientInit(IPlayer& player) override;

    // CoreEventHandler
    void onTick(Microseconds elapsed, TimePoint now) override;

    // PawnEventHandler: registers the SKR_* natives in every script
    void onAmxLoad(IPawnScript& script) override;
    void onAmxUnload(IPawnScript& script) override {}

    // SKR gate logic
    bool IssueChallenge(int playerId, protocol::ChallengeMessage& challengeOut);
    bool VerifyProof(int playerId, const protocol::ProofMessage& proof,
                     const uint8_t publicKey[crypto::kKeySize]);
    void AuthorizePlayer(int playerId, uint32_t accountId);
    void RejectPlayer(int playerId, protocol::RejectionReason reason);
    uint8_t ProcessAcHeartbeat(int playerId, const protocol::AcHeartbeatMessage& beat,
                                uint64_t nowMs);

    const SkrPlayerData* GetPlayerData(int playerId) const;
    bool IsAuthorized(int playerId) const;
    bool IsEnhanced(int playerId) const;
    bool AdmitInitial(IPlayer& player);
    void Negotiate(IPlayer& player, NetworkBitStream& bs);
    void SendSessionInfo(IPlayer& player);
    bool ScriptsHaveIntro();

    // Script natives. False when the player is unknown/rejected or arguments are invalid.
    bool SetRadarStyle(int playerId, protocol::RadarStyle style);
    bool SetRadarSize(int playerId, protocol::RadarSizeMode mode, float scale);
    bool SetRadarBounds(int playerId, float scaleW, float scaleH);
    bool SetRadarVisible(int playerId, bool visible);
    bool GetRadarRect(int playerId, float& x, float& y, float& w, float& h) const;
    bool ResetRadarDefaults(int playerId);

    // Called by SkrRadarRectHandler when RPC 243 arrives.
    void OnRadarRect(int playerId, float x, float y, float w, float h);

    // ── Native Runtime API (native_api_server.cpp, docs/protocol/SKR_NATIVE_API.md) ──
    // Envelope steps 5..8 arriving on RPC 240 (routed by SkrClientStateHandler).
    void OnApiMessage(IPlayer& player, NetworkBitStream& bs);
    bool ApiIsReady(int playerId) const;
    uint32_t ApiCapabilities(int playerId) const;
    // Script entry points (SKRE_* natives). False = not sent (see the .inc for reasons).
    bool ApiNotify(int playerId, protocol::api::NotifyKind kind, uint8_t durationSec,
                   const char* text, size_t len);
    bool ApiShowConfirm(int playerId, int32_t requestId, const char* title, size_t titleLen,
                        const char* message, size_t messageLen, uint8_t timeoutSec);
    bool ApiCancelConfirm(int playerId);
    bool ApiSetHudVisible(int playerId, protocol::api::HudComponent component, bool visible);

    // ── SKR-X Gateway (skrx_gateway_server.cpp) ──
    void OnGatewayMessage(IPlayer& player, const uint8_t* data, size_t len);
    void OnGatewayCoreMessage(IPlayer& player, const protocol::skrx::GatewayHeader& hdr,
                               const uint8_t* payload, size_t payloadLen);
    void OnCorePing(IPlayer& player, const uint8_t* data, size_t len);

    // ── SKR-X VEHICLES (skrx_vehicles_server.cpp) ──
    void OnGatewayVehiclesMessage(IPlayer& player, const protocol::skrx::GatewayHeader& hdr,
                                   const uint8_t* payload, size_t payloadLen);
    void SendVehicleRegistryToPlayer(IPlayer& player);
    void SendVehicleBatchAssignToPlayer(IPlayer& player);
    void BroadcastVehicleAssign(int vehiclePoolId, uint16_t customModelId, uint32_t generation);
    void BroadcastVehicleRevoke(uint16_t vehiclePoolId);

    // VehicleEventHandler
    void onVehicleStreamIn(IVehicle& vehicle, IPlayer& player) override;
    void onVehicleStreamOut(IVehicle& vehicle, IPlayer& player) override;
    vehicles::VehicleManager& GetVehicleManager() { return vehicleManager_; }
    IVehiclesComponent* GetVehiclesComponent() const { return vehiclesComp_; }

    // ── Phone backend (M5.8, worker-owned SQLite) ──
    void SetPhoneEnabled(bool enabled);
    bool IsPhoneEnabled() const { return phoneEnabled_; }
    bool PlayerAuthenticate(int playerId, const char* name);
    bool IsPlayerAuthenticated(int playerId) const;
    void ApiOnClientRequest(IPlayer& player, const uint8_t* data, size_t len);
    bool PhoneDbInit();
    void PhoneTick();
    void OnGatewayPhoneMessage(IPlayer&, const protocol::skrx::GatewayHeader&, const uint8_t*, size_t);
    void PhoneDbClose();
    void PhoneOnPlayerDisconnect(int playerId);
    // PHONE-M4F: server-initiated phone pushes (no client request id).
    bool PhonePushSync(int playerId);           // after SKRE_PlayerAuthenticated


    // Test-only: server-side phone ops via Pawn commands (no client protocol needed)
    bool TestPhoneSync(int playerId);
    int  TestSmsSend(int fromId, int toId, const char* text, size_t textLen);

private:
    // Native API internals (native_api_server.cpp)
    struct ApiEvent {
        enum class Kind : uint8_t { Ready, ConfirmResponse } kind;
        int playerId;
        uint32_t sessionId;   // event is dropped if the player's session changed
        int32_t requestId;
        int32_t response;
    };
    void ApiOnHello(IPlayer& player, const uint8_t* data, size_t len);
    void ApiOnReply(IPlayer& player, const uint8_t* data, size_t len);
    bool ApiSendCall(IPlayer& player, uint32_t sessionId, uint32_t callId,
                     protocol::api::Op op, const uint8_t* payload, size_t payloadLen);
    bool ApiSend(IPlayer& player, const uint8_t* data, size_t len);
    void ApiInvalid(int playerId, const char* reason);   // caller must NOT hold mutex_
    void ApiSendHudOverride(int playerId);
    void ApiTick(uint64_t nowMs);
    void ApiDispatchEvents();
    void ApiOnDisconnect(int playerId, bool wasReady, bool hadConfirm);
    uint32_t ApiNewSessionId();
    // Main-thread enqueue and negotiated transport; database work belongs to the worker.
    bool PhoneQueueRequest(IPlayer&,const protocol::api::ClientRequest&,bool gateway,bool serverPush=false);
    bool PhoneSendCall(IPlayer&,uint32_t session,uint32_t call,protocol::api::Op,const uint8_t*,size_t);

    std::vector<ApiEvent> apiEvents_;   // guarded by mutex_, drained on the main thread in onTick
    uint64_t apiTickAccumulatorMs_ = 0;
    uint32_t apiSessionCounter_ = 0;

    void ApplyHudDefaults(SkrPlayerData& data);
    // Sends the stored radar state if the player is authorized and has session info.
    void SendRadarStyle(int playerId);
    AdmissionDecision EvaluateClient(const IPlayer& player) const;
    void RejectForClientPolicy(IPlayer& player, AdmissionDecision decision);
    void RejectWithReason(IPlayer& player, protocol::RejectionReason reason,
                          bool forceEnhancedNotice = false);
    bool SendEnhancedRejection(IPlayer& player, protocol::RejectionReason reason);
    bool SendLegacyRejection(IPlayer& player, protocol::RejectionReason reason);
    uint8_t SupportedClientMask() const;

    ICore* core_ = nullptr;
    IPawnComponent* pawn_ = nullptr;
    crypto::MockSigningProvider crypto_;
    ClientPolicy clientPolicy_;
    std::string clientPolicyError_;

    mutable std::mutex mutex_;
    std::unordered_map<int, SkrPlayerData> players_;
    uint64_t generationCounter_ = 0;

    static constexpr char kServerId[] = "skr-main";
    static constexpr uint8_t kServerIdLen = 8;
    static constexpr uint64_t kAuthTimeoutSec = 10;
    static constexpr int kRPC_ClientJoin = 25;

    SkrJoinFilter joinFilter_;
    SkrClientStateHandler clientStateHandler_;
    SkrRadarRectHandler radarRectHandler_;
    uint64_t tickAccumulatorMs_ = 0;
    bool phoneEnabled_ = false;
    IDatabasesComponent* databases_ = nullptr;
    std::unique_ptr<skr::phone_backend::Worker> phoneWorker_;
    IVehiclesComponent* vehiclesComp_ = nullptr;
    vehicles::VehicleManager vehicleManager_;
};

// Registers the SKRE_* natives (native_api_server.cpp). Called from onAmxLoad.
void RegisterApiNatives(IPawnScript& script, SkrGateComponent* gate);
void ReleaseApiNatives(SkrGateComponent* gate);   // from ~SkrGateComponent

// Phone natives (phone_backend.cpp). Called from onAmxLoad.
void RegisterPhoneNatives(IPawnScript& script, SkrGateComponent* gate);
void ReleasePhoneNatives(SkrGateComponent* gate);

// Vehicle natives (skrx_vehicles_server.cpp). Called from onAmxLoad.
void RegisterVehicleNatives(IPawnScript& script, SkrGateComponent* gate,
                            vehicles::VehicleManager* mgr);
void ReleaseVehicleNatives();

} // namespace skr::gate_omp
