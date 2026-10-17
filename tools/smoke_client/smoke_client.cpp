// Smoke test client — exercises full auth + AC heartbeat flow against SKRTestServer.
// Usage: smoke_client.exe [port]   (default 7710)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "protocol/messages.h"
#include "crypto/crypto.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

// Must match test_server.h framing
enum class MsgType : uint8_t {
    Challenge      = 1,
    Proof          = 2,
    Result         = 3,
    AcHeartbeat    = 4,
    AcHeartbeatAck = 5,
    IssueTicket    = 10,
    TicketResponse = 11,
};

static bool SendFrame(SOCKET sock, MsgType type,
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

static bool RecvFrame(SOCKET sock, MsgType& type,
                      uint8_t* payload, uint16_t& len) {
    uint8_t header[3];
    int received = 0;
    while (received < 3) {
        int r = recv(sock, reinterpret_cast<char*>(header + received),
                     3 - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    type = static_cast<MsgType>(header[0]);
    len = static_cast<uint16_t>((header[1] << 8) | header[2]);
    if (len > 4096) return false;
    received = 0;
    while (received < len) {
        int r = recv(sock, reinterpret_cast<char*>(payload + received),
                     len - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    return true;
}

static int g_steps = 0, g_passed = 0;

static void Step(const char* name, bool ok) {
    ++g_steps;
    printf("  [%s] %s\n", ok ? "OK" : "FAIL", name);
    if (ok) ++g_passed;
}

int main(int argc, char* argv[]) {
    uint16_t port = 7710;
    if (argc > 1) {
        int p = atoi(argv[1]);
        if (p > 0 && p < 65536) port = static_cast<uint16_t>(p);
    }

    printf("=== SKR Smoke Test Client ===\n");
    printf("Connecting to 127.0.0.1:%u\n\n", port);

    // ── Init Winsock ─────────────────────────────────────
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        printf("socket() failed\n");
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        printf("connect() failed: %d — is SKRTestServer running?\n",
               WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    Step("Connected to server", true);

    // ── Generate keypair ─────────────────────────────────
    skr::crypto::MockSigningProvider crypto;
    skr::crypto::KeyPair kp{};
    bool keyOk = crypto.GenerateKeyPair(kp);
    Step("Generated keypair", keyOk);

    uint8_t buf[4096];
    MsgType type;
    uint16_t len;

    // ── Receive Challenge ────────────────────────────────
    bool gotChallenge = RecvFrame(sock, type, buf, len);
    Step("Received challenge", gotChallenge && type == MsgType::Challenge &&
         len == sizeof(skr::protocol::ChallengeMessage));

    skr::protocol::ChallengeMessage challenge{};
    memcpy(&challenge, buf, sizeof(challenge));

    printf("    server_id: %.8s  generation: %llu  expires_at: %llu\n",
           challenge.server_id, challenge.generation, challenge.expires_at);

    // ── Request ticket (test helper) ─────────────────────
    uint32_t accountId = 1001;
    uint8_t ticketReq[4 + skr::crypto::kKeySize];
    ticketReq[0] = static_cast<uint8_t>(accountId >> 24);
    ticketReq[1] = static_cast<uint8_t>(accountId >> 16);
    ticketReq[2] = static_cast<uint8_t>(accountId >> 8);
    ticketReq[3] = static_cast<uint8_t>(accountId);
    memcpy(ticketReq + 4, kp.publicKey, skr::crypto::kKeySize);

    bool sentTicketReq = SendFrame(sock, MsgType::IssueTicket,
                                   ticketReq, sizeof(ticketReq));
    Step("Sent ticket request", sentTicketReq);

    // Receive join_id
    bool gotTicket = RecvFrame(sock, type, buf, len);
    Step("Received join_id", gotTicket && type == MsgType::TicketResponse &&
         len == skr::protocol::kJoinIdSize);

    uint8_t joinId[skr::protocol::kJoinIdSize];
    memcpy(joinId, buf, skr::protocol::kJoinIdSize);

    // ── Build transcript and sign ────────────────────────
    skr::protocol::TranscriptBuilder transcript;
    bool transcriptOk = transcript.Build(challenge, joinId);
    Step("Built transcript", transcriptOk);

    uint8_t signature[skr::crypto::kSignatureSize];
    bool signOk = crypto.Sign(kp.privateKey, transcript.buffer,
                              transcript.length, signature);
    Step("Signed transcript", signOk);

    // Verify locally first
    bool localVerify = crypto.Verify(kp.publicKey, transcript.buffer,
                                     transcript.length, signature);
    Step("Local signature verification", localVerify);

    // ── Send Proof ───────────────────────────────────────
    skr::protocol::ProofMessage proof{};
    proof.version = skr::protocol::kProtocolVersion;
    memcpy(proof.join_id, joinId, skr::protocol::kJoinIdSize);
    memcpy(proof.signature, signature, skr::crypto::kSignatureSize);

    bool sentProof = SendFrame(sock, MsgType::Proof,
                               &proof, sizeof(proof));
    Step("Sent proof", sentProof);

    // ── Receive Result ───────────────────────────────────
    bool gotResult = RecvFrame(sock, type, buf, len);
    bool resultOk = gotResult && type == MsgType::Result &&
                    len == sizeof(skr::protocol::ResultMessage);
    Step("Received result", resultOk);

    skr::protocol::ResultMessage result{};
    if (resultOk) memcpy(&result, buf, sizeof(result));

    bool authorized = result.status == 1 &&
                      result.error_code == static_cast<uint8_t>(
                          skr::protocol::AuthError::None);
    Step("Authorization granted", authorized);

    printf("    status: %u  error: %u\n", result.status, result.error_code);

    // ── AC Heartbeat loop (3 beats) ──────────────────────
    printf("\n  Sending 3 AC heartbeats...\n");
    bool allAcksOk = true;

    for (uint32_t seq = 1; seq <= 3; ++seq) {
        skr::protocol::AcHeartbeatMessage beat{};
        beat.version = skr::protocol::kProtocolVersion;
        beat.sequence = seq;
        auto now = std::chrono::steady_clock::now();
        beat.timestampMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count());
        beat.acState = 2;   // Ready
        beat.acVerdict = 0; // Clean

        bool sentBeat = SendFrame(sock, MsgType::AcHeartbeat,
                                  &beat, sizeof(beat));
        if (!sentBeat) { allAcksOk = false; break; }

        bool gotAck = RecvFrame(sock, type, buf, len);
        if (!gotAck || type != MsgType::AcHeartbeatAck) {
            allAcksOk = false;
            break;
        }

        skr::protocol::AcHeartbeatAck ack{};
        memcpy(&ack, buf, sizeof(ack));

        bool ackOk = ack.sequence == seq && ack.action == 0;
        printf("    heartbeat #%u → ack seq=%u action=%u %s\n",
               seq, ack.sequence, ack.action,
               ackOk ? "[OK]" : "[FAIL]");

        if (!ackOk) allAcksOk = false;

        if (seq < 3) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    Step("All AC heartbeats acknowledged", allAcksOk);

    // ── Cleanup ──────────────────────────────────────────
    closesocket(sock);
    WSACleanup();

    printf("\n=== %d/%d steps passed ===\n", g_passed, g_steps);
    return g_passed == g_steps ? 0 : 1;
}
