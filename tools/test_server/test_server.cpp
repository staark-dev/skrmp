#include "test_server.h"
#include <cstring>
#include <ctime>

#pragma comment(lib, "ws2_32.lib")

namespace skr::testserver {

TestServer::TestServer(uint16_t port)
    : port_(port), auth_(crypto_), gate_(crypto_) {
    slots_.resize(kMaxSlots);
}

TestServer::~TestServer() {
    Stop();
}

bool TestServer::Start() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[server] WSAStartup failed\n");
        return false;
    }

    listenSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock_ == INVALID_SOCKET) {
        printf("[server] socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return false;
    }

    // Allow port reuse for quick restarts
    int opt = 1;
    setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port_);

    if (bind(listenSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        printf("[server] bind() failed on port %u: %d\n", port_, WSAGetLastError());
        closesocket(listenSock_);
        WSACleanup();
        return false;
    }

    if (listen(listenSock_, SOMAXCONN) == SOCKET_ERROR) {
        printf("[server] listen() failed: %d\n", WSAGetLastError());
        closesocket(listenSock_);
        WSACleanup();
        return false;
    }

    running_ = true;
    acceptThread_ = std::thread(&TestServer::AcceptLoop, this);

    printf("[server] Listening on 127.0.0.1:%u\n", port_);
    printf("[server] Ready for connections (max %u slots)\n", kMaxSlots);
    return true;
}

void TestServer::Stop() {
    if (!running_.exchange(false)) return;

    if (listenSock_ != INVALID_SOCKET) {
        closesocket(listenSock_);
        listenSock_ = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(slotsMutex_);
        for (auto& slot : slots_) {
            if (slot.sock != INVALID_SOCKET) {
                closesocket(slot.sock);
                slot.sock = INVALID_SOCKET;
            }
            slot.active = false;
        }
    }

    if (acceptThread_.joinable()) acceptThread_.join();
    for (auto& t : clientThreads_) {
        if (t.joinable()) t.join();
    }
    clientThreads_.clear();

    WSACleanup();
    printf("[server] Stopped\n");
}

// ── Accept loop ──────────────────────────────────────────

void TestServer::AcceptLoop() {
    while (running_) {
        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock_,
                                   reinterpret_cast<sockaddr*>(&clientAddr),
                                   &addrLen);
        if (clientSock == INVALID_SOCKET) {
            if (running_) printf("[server] accept() error: %d\n", WSAGetLastError());
            continue;
        }

        // Find a free slot
        size_t slotIdx = SIZE_MAX;
        {
            std::lock_guard<std::mutex> lock(slotsMutex_);
            for (size_t i = 0; i < slots_.size(); ++i) {
                if (!slots_[i].active) {
                    slotIdx = i;
                    break;
                }
            }
        }

        if (slotIdx == SIZE_MAX) {
            printf("[server] No free slots, rejecting connection\n");
            closesocket(clientSock);
            continue;
        }

        uint16_t pid;
        {
            std::lock_guard<std::mutex> lock(slotsMutex_);
            pid = nextPlayerId_++;
            if (nextPlayerId_ >= gate::SkrGate::kMaxPlayers)
                nextPlayerId_ = 1;

            auto& slot = slots_[slotIdx];
            slot.sock = clientSock;
            slot.playerId = pid;
            slot.active = true;
            slot.accountId = 0;
            memset(slot.publicKey, 0, crypto::kKeySize);
            memset(slot.joinId, 0, protocol::kJoinIdSize);
        }

        printf("[server] Client connected → slot %zu, playerId %u\n",
               slotIdx, pid);

        // Send challenge immediately
        protocol::ChallengeMessage challenge{};
        if (gate_.OnPlayerConnect(pid, "skr-main", challenge)) {
            SendFrame(clientSock, MsgType::Challenge,
                      &challenge, sizeof(challenge));
            printf("[server] Sent challenge to playerId %u\n", pid);
        }

        clientThreads_.emplace_back(&TestServer::ClientLoop, this, slotIdx);
    }
}

// ── Per-client loop ──────────────────────────────────────

void TestServer::ClientLoop(size_t slotIndex) {
    SOCKET sock;
    uint16_t playerId;
    {
        std::lock_guard<std::mutex> lock(slotsMutex_);
        sock = slots_[slotIndex].sock;
        playerId = slots_[slotIndex].playerId;
    }

    uint8_t buf[kMaxFramePayload];
    MsgType type;
    uint16_t len;

    while (running_) {
        if (!RecvFrame(sock, type, buf, len)) break;

        switch (type) {
            case MsgType::IssueTicket:
                HandleIssueTicket(slotIndex, buf, len);
                break;
            case MsgType::Proof:
                HandleProof(slotIndex, buf, len);
                break;
            case MsgType::AcHeartbeat:
                HandleAcHeartbeat(slotIndex, buf, len);
                break;
            default:
                printf("[server] Unknown message type %u from playerId %u\n",
                       static_cast<unsigned>(type), playerId);
                break;
        }
    }

    // Cleanup
    {
        std::lock_guard<std::mutex> lock(slotsMutex_);
        auto& slot = slots_[slotIndex];
        gate_.OnPlayerDisconnect(slot.playerId);
        if (slot.sock != INVALID_SOCKET) {
            closesocket(slot.sock);
            slot.sock = INVALID_SOCKET;
        }
        slot.active = false;
        printf("[server] playerId %u disconnected (slot %zu)\n",
               playerId, slotIndex);
    }
}

// ── Framing ──────────────────────────────────────────────

bool TestServer::SendFrame(SOCKET sock, MsgType type,
                           const void* payload, uint16_t len) {
    uint8_t header[3];
    header[0] = static_cast<uint8_t>(type);
    header[1] = static_cast<uint8_t>(len >> 8);
    header[2] = static_cast<uint8_t>(len);

    if (send(sock, reinterpret_cast<const char*>(header), 3, 0) != 3)
        return false;
    if (len > 0 && payload) {
        if (send(sock, reinterpret_cast<const char*>(payload), len, 0) != len)
            return false;
    }
    return true;
}

bool TestServer::RecvFrame(SOCKET sock, MsgType& type,
                           uint8_t* payload, uint16_t& len) {
    uint8_t header[3];
    int received = 0;

    // Read 3-byte header fully
    while (received < 3) {
        int r = recv(sock, reinterpret_cast<char*>(header + received),
                     3 - received, 0);
        if (r <= 0) return false;
        received += r;
    }

    type = static_cast<MsgType>(header[0]);
    len = static_cast<uint16_t>((header[1] << 8) | header[2]);

    if (len > kMaxFramePayload) return false;

    // Read payload fully
    received = 0;
    while (received < len) {
        int r = recv(sock, reinterpret_cast<char*>(payload + received),
                     len - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    return true;
}

// ── Message handlers ─────────────────────────────────────

// Test helper: client requests a ticket before sending proof.
// Payload: [uint32_t accountId][uint8_t publicKey[32]]
void TestServer::HandleIssueTicket(size_t slotIndex, const uint8_t* payload,
                                    uint16_t len) {
    if (len < 4 + crypto::kKeySize) {
        printf("[server] IssueTicket: payload too short\n");
        return;
    }

    uint32_t accountId = (payload[0] << 24) | (payload[1] << 16) |
                         (payload[2] << 8)  | payload[3];
    const uint8_t* pubKey = payload + 4;

    std::lock_guard<std::mutex> lock(slotsMutex_);
    auto& slot = slots_[slotIndex];
    slot.accountId = accountId;
    memcpy(slot.publicKey, pubKey, crypto::kKeySize);

    uint8_t joinId[protocol::kJoinIdSize];
    if (!auth_.IssueTicket(accountId, pubKey, "skr-main", "dev-0.1", joinId)) {
        printf("[server] IssueTicket failed for account %u\n", accountId);
        return;
    }

    memcpy(slot.joinId, joinId, protocol::kJoinIdSize);

    // Send join_id back
    SendFrame(slot.sock, MsgType::TicketResponse, joinId, protocol::kJoinIdSize);
    printf("[server] Issued ticket for account %u → playerId %u\n",
           accountId, slot.playerId);
}

void TestServer::HandleProof(size_t slotIndex, const uint8_t* payload,
                              uint16_t len) {
    if (len < sizeof(protocol::ProofMessage)) {
        printf("[server] Proof: payload too short\n");
        return;
    }

    protocol::ProofMessage proof;
    memcpy(&proof, payload, sizeof(proof));

    uint16_t playerId;
    uint8_t pubKey[crypto::kKeySize];
    SOCKET sock;
    {
        std::lock_guard<std::mutex> lock(slotsMutex_);
        auto& slot = slots_[slotIndex];
        playerId = slot.playerId;
        memcpy(pubKey, slot.publicKey, crypto::kKeySize);
        sock = slot.sock;
    }

    bool proofValid = gate_.OnProofReceived(playerId, proof, pubKey);

    if (proofValid) {
        // Redeem ticket with auth service
        protocol::AuthError err = protocol::AuthError::None;
        uint32_t acctId = auth_.RedeemTicket(proof.join_id, "skr-main", err);

        if (acctId > 0 && err == protocol::AuthError::None) {
            gate_.OnRedeemResult(playerId, true, acctId);
            printf("[server] playerId %u AUTHORIZED (account %u)\n",
                   playerId, acctId);

            protocol::ResultMessage result{};
            result.version = protocol::kProtocolVersion;
            result.status = 1;
            result.error_code = static_cast<uint8_t>(protocol::AuthError::None);
            SendFrame(sock, MsgType::Result, &result, sizeof(result));
        } else {
            gate_.OnRedeemResult(playerId, false, 0);
            printf("[server] playerId %u REJECTED (ticket redeem failed: %u)\n",
                   playerId, static_cast<unsigned>(err));

            protocol::ResultMessage result{};
            result.version = protocol::kProtocolVersion;
            result.status = 0;
            result.error_code = static_cast<uint8_t>(err);
            SendFrame(sock, MsgType::Result, &result, sizeof(result));
        }
    } else {
        printf("[server] playerId %u REJECTED (proof verification failed)\n",
               playerId);

        protocol::ResultMessage result{};
        result.version = protocol::kProtocolVersion;
        result.status = 0;
        result.error_code = static_cast<uint8_t>(protocol::AuthError::InvalidSignature);
        SendFrame(sock, MsgType::Result, &result, sizeof(result));
    }
}

void TestServer::HandleAcHeartbeat(size_t slotIndex, const uint8_t* payload,
                                    uint16_t len) {
    if (len < sizeof(protocol::AcHeartbeatMessage)) return;

    protocol::AcHeartbeatMessage beat;
    memcpy(&beat, payload, sizeof(beat));

    uint16_t playerId;
    SOCKET sock;
    {
        std::lock_guard<std::mutex> lock(slotsMutex_);
        playerId = slots_[slotIndex].playerId;
        sock = slots_[slotIndex].sock;
    }

    auto now = std::chrono::steady_clock::now();
    uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch()).count();

    uint8_t action = gate_.OnAcHeartbeat(playerId, beat, nowMs);

    protocol::AcHeartbeatAck ack{};
    ack.version = protocol::kProtocolVersion;
    ack.sequence = beat.sequence;
    ack.action = action;
    SendFrame(sock, MsgType::AcHeartbeatAck, &ack, sizeof(ack));

    const char* actionStr[] = {"continue", "warn", "disconnect"};
    printf("[server] AC heartbeat #%u from playerId %u → %s\n",
           beat.sequence, playerId,
           action <= 2 ? actionStr[action] : "unknown");
}

} // namespace skr::testserver
