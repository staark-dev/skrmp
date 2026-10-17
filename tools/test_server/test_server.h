#pragma once
#include "protocol/messages.h"
#include "crypto/crypto.h"
#include "auth_service.h"
#include "gate.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

namespace skr::testserver {

constexpr uint16_t kDefaultPort = 7710;
constexpr size_t kMaxFramePayload = 4096;

// Wire frame: [uint8_t msgType][uint16_t payloadLen (big-endian)][payload]
enum class MsgType : uint8_t {
    Challenge      = 1,   // server → client
    Proof          = 2,   // client → server
    Result         = 3,   // server → client
    AcHeartbeat    = 4,   // client → server
    AcHeartbeatAck = 5,   // server → client
    IssueTicket    = 10,  // client → server (test helper: request a ticket)
    TicketResponse = 11,  // server → client (test helper: join_id)
};

struct ClientSlot {
    SOCKET sock = INVALID_SOCKET;
    uint16_t playerId = 0;
    bool active = false;
    uint8_t publicKey[crypto::kKeySize]{};
    uint8_t joinId[protocol::kJoinIdSize]{};
    uint32_t accountId = 0;
};

class TestServer {
public:
    TestServer(uint16_t port = kDefaultPort);
    ~TestServer();

    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    void AcceptLoop();
    void ClientLoop(size_t slotIndex);

    bool SendFrame(SOCKET sock, MsgType type, const void* payload, uint16_t len);
    bool RecvFrame(SOCKET sock, MsgType& type, uint8_t* payload, uint16_t& len);

    void HandleIssueTicket(size_t slotIndex, const uint8_t* payload, uint16_t len);
    void HandleProof(size_t slotIndex, const uint8_t* payload, uint16_t len);
    void HandleAcHeartbeat(size_t slotIndex, const uint8_t* payload, uint16_t len);

    uint16_t port_;
    SOCKET listenSock_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;

    crypto::MockSigningProvider crypto_;
    auth::AuthService auth_;
    gate::SkrGate gate_;

    std::mutex slotsMutex_;
    std::vector<ClientSlot> slots_;
    std::vector<std::thread> clientThreads_;
    uint16_t nextPlayerId_ = 1;

    static constexpr uint16_t kMaxSlots = 64;
};

} // namespace skr::testserver
