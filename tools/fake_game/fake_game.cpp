// fake_game.exe — Loads SKR.Client.dll and exercises init → auth → AC flow
// without GTA SA. Can run standalone or against SKRTestServer.
//
// Usage: fake_game.exe [server_port]
//   If server_port is given, connects to 127.0.0.1:<port> for full auth flow.
//   Without it, tests DLL load + AC policy in isolation.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

#include "protocol/messages.h"
#include "crypto/crypto.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

// Function pointer types for DLL exports
typedef BOOL (__cdecl *SkrInitFn)();
typedef void (__cdecl *SkrShutdownFn)();

// Framing (matches test_server)
enum class MsgType : uint8_t {
    Challenge = 1, Proof = 2, Result = 3,
    AcHeartbeat = 4, AcHeartbeatAck = 5,
    IssueTicket = 10, TicketResponse = 11,
};

static bool SendFrame(SOCKET s, MsgType t, const void* p, uint16_t len) {
    uint8_t h[3] = { uint8_t(t), uint8_t(len >> 8), uint8_t(len) };
    if (send(s, (char*)h, 3, 0) != 3) return false;
    if (len > 0) return send(s, (const char*)p, len, 0) == len;
    return true;
}

static bool RecvFrame(SOCKET s, MsgType& t, uint8_t* buf, uint16_t& len) {
    uint8_t h[3]; int r = 0;
    while (r < 3) { int n = recv(s, (char*)h + r, 3 - r, 0); if (n <= 0) return false; r += n; }
    t = MsgType(h[0]); len = uint16_t((h[1] << 8) | h[2]);
    if (len > 4096) return false;
    r = 0;
    while (r < len) { int n = recv(s, (char*)buf + r, len - r, 0); if (n <= 0) return false; r += n; }
    return true;
}

static int g_steps = 0, g_pass = 0;
static void Step(const char* name, bool ok) {
    ++g_steps;
    printf("  [%s] %s\n", ok ? "OK" : "FAIL", name);
    if (ok) ++g_pass;
}

// ── Phase 1: DLL load test ───────────────────────────────

static bool TestDllLoad(const wchar_t* dllPath) {
    printf("\n--- Phase 1: DLL Load ---\n");

    HMODULE hMod = LoadLibraryW(dllPath);
    Step("LoadLibraryW(SKR.Client.dll)", hMod != nullptr);
    if (!hMod) {
        printf("    LoadLibrary error: %lu\n", GetLastError());
        return false;
    }

    auto initFn = (SkrInitFn)GetProcAddress(hMod, "SkrInitialize");
    Step("GetProcAddress(SkrInitialize)", initFn != nullptr);

    auto shutdownFn = (SkrShutdownFn)GetProcAddress(hMod, "SkrShutdown");
    Step("GetProcAddress(SkrShutdown)", shutdownFn != nullptr);

    // SkrInitialize will fail because there's no D3D9 device — that's expected.
    // The important thing is the DLL loaded and exports are resolvable.
    if (initFn) {
        BOOL initOk = initFn();
        // D3D9 hook failure is expected in fake_game (no GPU context)
        printf("    SkrInitialize returned: %s (D3D9 failure expected)\n",
               initOk ? "TRUE" : "FALSE");
    }

    if (shutdownFn) {
        shutdownFn();
        Step("SkrShutdown() called", true);
    }

    FreeLibrary(hMod);
    Step("FreeLibrary", true);
    return true;
}

// ── Phase 2: Auth flow against test server ───────────────

static bool TestAuthFlow(uint16_t port) {
    printf("\n--- Phase 2: Auth Flow (port %u) ---\n", port);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("    WSAStartup failed\n");
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) { WSACleanup(); return false; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("    connect() failed: %d — is SKRTestServer running on port %u?\n",
               WSAGetLastError(), port);
        closesocket(sock);
        WSACleanup();
        return false;
    }
    Step("Connected to test server", true);

    skr::crypto::MockSigningProvider crypto;
    skr::crypto::KeyPair kp{};
    Step("Generated keypair", crypto.GenerateKeyPair(kp));

    uint8_t buf[4096]; MsgType type; uint16_t len;

    // Receive challenge
    bool gotChallenge = RecvFrame(sock, type, buf, len);
    Step("Received challenge",
         gotChallenge && type == MsgType::Challenge &&
         len == sizeof(skr::protocol::ChallengeMessage));

    skr::protocol::ChallengeMessage challenge{};
    memcpy(&challenge, buf, sizeof(challenge));

    // Request ticket
    uint32_t accountId = 42;
    uint8_t ticketReq[4 + skr::crypto::kKeySize];
    ticketReq[0] = uint8_t(accountId >> 24);
    ticketReq[1] = uint8_t(accountId >> 16);
    ticketReq[2] = uint8_t(accountId >> 8);
    ticketReq[3] = uint8_t(accountId);
    memcpy(ticketReq + 4, kp.publicKey, skr::crypto::kKeySize);
    Step("Sent ticket request",
         SendFrame(sock, MsgType::IssueTicket, ticketReq, sizeof(ticketReq)));

    bool gotTicket = RecvFrame(sock, type, buf, len);
    Step("Received join_id",
         gotTicket && type == MsgType::TicketResponse &&
         len == skr::protocol::kJoinIdSize);

    uint8_t joinId[skr::protocol::kJoinIdSize];
    memcpy(joinId, buf, skr::protocol::kJoinIdSize);

    // Sign transcript
    skr::protocol::TranscriptBuilder transcript;
    Step("Built transcript", transcript.Build(challenge, joinId));

    uint8_t sig[skr::crypto::kSignatureSize];
    Step("Signed transcript",
         crypto.Sign(kp.privateKey, transcript.buffer, transcript.length, sig));

    // Send proof
    skr::protocol::ProofMessage proof{};
    proof.version = skr::protocol::kProtocolVersion;
    memcpy(proof.join_id, joinId, skr::protocol::kJoinIdSize);
    memcpy(proof.signature, sig, skr::crypto::kSignatureSize);
    Step("Sent proof", SendFrame(sock, MsgType::Proof, &proof, sizeof(proof)));

    // Receive result
    bool gotResult = RecvFrame(sock, type, buf, len);
    Step("Received result",
         gotResult && type == MsgType::Result &&
         len == sizeof(skr::protocol::ResultMessage));

    skr::protocol::ResultMessage result{};
    if (gotResult) memcpy(&result, buf, sizeof(result));
    Step("Authorized",
         result.status == 1 &&
         result.error_code == uint8_t(skr::protocol::AuthError::None));

    // AC heartbeat loop
    printf("\n--- Phase 3: AC Heartbeats ---\n");
    bool allOk = true;
    for (uint32_t seq = 1; seq <= 5; ++seq) {
        skr::protocol::AcHeartbeatMessage beat{};
        beat.version = skr::protocol::kProtocolVersion;
        beat.sequence = seq;
        auto now = std::chrono::steady_clock::now();
        beat.timestampMs = uint64_t(std::chrono::duration_cast<
            std::chrono::milliseconds>(now.time_since_epoch()).count());
        beat.acState = 2;  // Ready
        beat.acVerdict = 0; // Clean

        if (!SendFrame(sock, MsgType::AcHeartbeat, &beat, sizeof(beat))) {
            allOk = false; break;
        }
        if (!RecvFrame(sock, type, buf, len) || type != MsgType::AcHeartbeatAck) {
            allOk = false; break;
        }

        skr::protocol::AcHeartbeatAck ack{};
        memcpy(&ack, buf, sizeof(ack));
        bool ackOk = ack.sequence == seq && ack.action == 0;
        printf("    heartbeat #%u → ack seq=%u action=%u %s\n",
               seq, ack.sequence, ack.action, ackOk ? "[OK]" : "[FAIL]");
        if (!ackOk) allOk = false;

        if (seq < 5) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Step("All 5 AC heartbeats acknowledged", allOk);

    closesocket(sock);
    WSACleanup();
    return true;
}

// ── Main ─────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    printf("=== SKR Fake Game (DLL + Auth + AC smoke test) ===\n");

    // Find the DLL next to this exe
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dllPath(exePath);
    auto pos = dllPath.rfind(L'\\');
    if (pos != std::wstring::npos) dllPath.resize(pos);
    dllPath += L"\\SKR.Client.dll";

    // Phase 1: DLL load
    TestDllLoad(dllPath.c_str());

    // Phase 2+3: Auth flow (if server port given)
    if (argc > 1) {
        int port = atoi(argv[1]);
        if (port > 0 && port < 65536)
            TestAuthFlow(static_cast<uint16_t>(port));
        else
            printf("\nSkipping auth flow (invalid port: %s)\n", argv[1]);
    } else {
        printf("\nSkipping auth flow (pass server port as argument to test)\n");
    }

    printf("\n=== %d/%d steps passed ===\n", g_pass, g_steps);
    return g_pass == g_steps ? 0 : 1;
}
