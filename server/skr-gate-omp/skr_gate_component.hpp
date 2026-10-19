#pragma once

#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>
#include "protocol/messages.h"
#include "protocol/client_state.h"
#include "protocol/session_info.h"
#include "protocol/query_rules.h"
#include "crypto/crypto.h"
#include <bitstream.hpp>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <mutex>

namespace skr::gate_omp {

enum class SkrPlayerState : uint8_t {
    Connected,
    Challenged,
    Authorized,
    Rejected
};

struct SkrPlayerData {
    SkrPlayerState state = SkrPlayerState::Connected;
    uint8_t connectionId[protocol::kConnectionIdSize]{};
    uint8_t nonce[protocol::kNonceSize]{};
    uint64_t generation = 0;
    uint64_t challengeExpiresAt = 0;
    uint32_t accountId = 0;

    bool sessionInfoSent = false;

    // AC tracking
    uint32_t lastAcBeatSeq = 0;
    uint64_t lastAcBeatMs = 0;
    uint32_t missedAcBeats = 0;

    void Reset() {
        state = SkrPlayerState::Connected;
        generation = 0;
        challengeExpiresAt = 0;
        accountId = 0;
        sessionInfoSent = false;
        lastAcBeatSeq = 0;
        lastAcBeatMs = 0;
        missedAcBeats = 0;
        memset(connectionId, 0, protocol::kConnectionIdSize);
        memset(nonce, 0, protocol::kNonceSize);
    }
};

class SkrJoinFilter final : public SingleNetworkInEventHandler {
public:
    bool onReceive(IPlayer& peer, NetworkBitStream& bs) override;
    void setCore(ICore* c) { core_ = c; }
private:
    ICore* core_ = nullptr;
};

class SkrGateComponent;

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

class SkrGateComponent final
    : public IComponent
    , public PlayerConnectEventHandler
    , public CoreEventHandler
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

    // SKR gate logic
    bool IssueChallenge(int playerId, protocol::ChallengeMessage& challengeOut);
    bool VerifyProof(int playerId, const protocol::ProofMessage& proof,
                     const uint8_t publicKey[crypto::kKeySize]);
    void AuthorizePlayer(int playerId, uint32_t accountId);
    void RejectPlayer(int playerId, const char* reason);
    uint8_t ProcessAcHeartbeat(int playerId, const protocol::AcHeartbeatMessage& beat,
                                uint64_t nowMs);

    const SkrPlayerData* GetPlayerData(int playerId) const;
    bool IsAuthorized(int playerId) const;
    void SendSessionInfo(IPlayer& player);
    bool ScriptsHaveIntro();

private:
    ICore* core_ = nullptr;
    IPawnComponent* pawn_ = nullptr;
    crypto::MockSigningProvider crypto_;

    mutable std::mutex mutex_;
    std::unordered_map<int, SkrPlayerData> players_;
    uint64_t generationCounter_ = 0;

    static constexpr char kServerId[] = "skr-main";
    static constexpr uint8_t kServerIdLen = 8;
    static constexpr uint64_t kAuthTimeoutSec = 10;
    static constexpr int kRPC_ClientJoin = 25;

    SkrJoinFilter joinFilter_;
    SkrClientStateHandler clientStateHandler_;
    uint64_t tickAccumulatorMs_ = 0;
};

} // namespace skr::gate_omp
