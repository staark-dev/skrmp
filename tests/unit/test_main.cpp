#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>

#include "protocol/messages.h"
#include "crypto/crypto.h"
#include "updater/manifest.h"
#include "updater/release_gate.h"

using namespace skr;

static int g_total = 0, g_passed = 0, g_failed = 0;

#define RUN_TEST(fn) do { \
    ++g_total; \
    printf("  [TEST] %-50s ", #fn); \
    if (fn()) { printf("PASS\n"); ++g_passed; } \
    else { printf("FAIL\n"); ++g_failed; } \
} while(0)

// ==========================================================
// Transcript tests (Section 13: parser, transcript)
// ==========================================================

static bool test_transcript_deterministic() {
    protocol::ChallengeMessage ch{};
    ch.version = protocol::kProtocolVersion;
    memcpy(ch.server_id, "skr-test", 8);
    ch.server_id_len = 8;
    memset(ch.connection_id, 0xAA, protocol::kConnectionIdSize);
    ch.generation = 42;
    memset(ch.nonce, 0xBB, protocol::kNonceSize);
    ch.expires_at = 1000;

    uint8_t joinId[protocol::kJoinIdSize];
    memset(joinId, 0xCC, protocol::kJoinIdSize);

    protocol::TranscriptBuilder t1, t2;
    if (!t1.Build(ch, joinId)) return false;
    if (!t2.Build(ch, joinId)) return false;
    return t1.length == t2.length &&
           memcmp(t1.buffer, t2.buffer, t1.length) == 0;
}

static bool test_transcript_changes_with_nonce() {
    protocol::ChallengeMessage ch{};
    ch.version = protocol::kProtocolVersion;
    ch.server_id_len = 4;
    memcpy(ch.server_id, "test", 4);
    memset(ch.nonce, 0x11, protocol::kNonceSize);
    uint8_t joinId[protocol::kJoinIdSize]{};

    protocol::TranscriptBuilder t1;
    if (!t1.Build(ch, joinId)) return false;

    memset(ch.nonce, 0x22, protocol::kNonceSize);
    protocol::TranscriptBuilder t2;
    if (!t2.Build(ch, joinId)) return false;

    return memcmp(t1.buffer, t2.buffer, t1.length) != 0;
}

// ==========================================================
// Crypto tests (Section 13: known vectors, modified transcript)
// ==========================================================

static bool test_sign_verify_roundtrip() {
    crypto::MockSigningProvider mock;
    crypto::KeyPair kp{};
    if (!mock.GenerateKeyPair(kp)) return false;

    uint8_t msg[] = "test message";
    uint8_t sig[crypto::kSignatureSize]{};
    if (!mock.Sign(kp.privateKey, msg, sizeof(msg) - 1, sig)) return false;
    return mock.Verify(kp.publicKey, msg, sizeof(msg) - 1, sig);
}

static bool test_rejects_tampered_message() {
    crypto::MockSigningProvider mock;
    crypto::KeyPair kp{};
    if (!mock.GenerateKeyPair(kp)) return false;

    uint8_t msg[] = "original";
    uint8_t sig[crypto::kSignatureSize]{};
    if (!mock.Sign(kp.privateKey, msg, sizeof(msg) - 1, sig)) return false;

    uint8_t tampered[] = "tampered";
    return !mock.Verify(kp.publicKey, tampered, sizeof(tampered) - 1, sig);
}

static bool test_rejects_wrong_key() {
    crypto::MockSigningProvider mock;
    crypto::KeyPair kp1{}, kp2{};
    if (!mock.GenerateKeyPair(kp1)) return false;
    if (!mock.GenerateKeyPair(kp2)) return false;

    uint8_t msg[] = "test";
    uint8_t sig[crypto::kSignatureSize]{};
    if (!mock.Sign(kp1.privateKey, msg, sizeof(msg) - 1, sig)) return false;
    return !mock.Verify(kp2.publicKey, msg, sizeof(msg) - 1, sig);
}

static bool test_random_bytes_nonzero() {
    crypto::MockSigningProvider mock;
    uint8_t buf[32]{};
    if (!mock.RandomBytes(buf, 32)) return false;
    for (int i = 0; i < 32; ++i)
        if (buf[i] != 0) return true;
    return false;
}

// ==========================================================
// Manifest tests (Section 13: update — corruption, traversal)
// ==========================================================

static bool test_manifest_rejects_traversal() {
    updater::ManifestEntry e{};
    strcpy(e.relativePath, "../etc/passwd");
    e.sizeBytes = 100;
    memset(e.sha256, 0xFF, updater::kSha256Size);
    return !updater::ManifestSerializer::ValidateEntry(e);
}

static bool test_manifest_rejects_absolute() {
    updater::ManifestEntry e{};
    strcpy(e.relativePath, "C:\\Windows\\System32\\cmd.exe");
    e.sizeBytes = 100;
    memset(e.sha256, 0xFF, updater::kSha256Size);
    return !updater::ManifestSerializer::ValidateEntry(e);
}

static bool test_manifest_rejects_backslash() {
    updater::ManifestEntry e{};
    strcpy(e.relativePath, "client\\SKR.Client.dll");
    e.sizeBytes = 100;
    memset(e.sha256, 0xFF, updater::kSha256Size);
    return !updater::ManifestSerializer::ValidateEntry(e);
}

static bool test_manifest_accepts_valid() {
    updater::ManifestEntry e{};
    strcpy(e.relativePath, "client/SKR.Client.dll");
    e.sizeBytes = 100;
    memset(e.sha256, 0xFF, updater::kSha256Size);
    return updater::ManifestSerializer::ValidateEntry(e);
}

static bool test_manifest_roundtrip() {
    updater::ReleaseManifest m{};
    strcpy(m.version, "0.1.0");
    strcpy(m.channel, "beta");
    strcpy(m.platform, "windows");
    strcpy(m.architecture, "x86");
    m.timestamp = 1700000000;

    updater::ManifestEntry e{};
    strcpy(e.relativePath, "client/SKR.Client.dll");
    memset(e.sha256, 0xAB, updater::kSha256Size);
    e.sizeBytes = 12345;
    m.entries.push_back(e);

    std::vector<uint8_t> buf;
    if (!updater::ManifestSerializer::Serialize(m, buf)) return false;

    updater::ReleaseManifest out{};
    if (!updater::ManifestSerializer::Deserialize(buf.data(), buf.size(), out)) return false;
    return strcmp(out.version, "0.1.0") == 0 &&
           out.entries.size() == 1 &&
           out.entries[0].sizeBytes == 12345;
}

static bool test_manifest_rejects_truncated() {
    updater::ReleaseManifest out{};
    uint8_t garbage[10] = {1,2,3,4,5,6,7,8,9,10};
    return !updater::ManifestSerializer::Deserialize(garbage, sizeof(garbage), out);
}

static bool test_manifest_rejects_zero_hash() {
    updater::ManifestEntry e{};
    strcpy(e.relativePath, "client/test.dll");
    e.sizeBytes = 100;
    memset(e.sha256, 0, updater::kSha256Size);
    return !updater::ManifestSerializer::ValidateEntry(e);
}

// ==========================================================
// Release gate tests (Section 13: version + channel)
// ==========================================================

static bool test_version_parse_valid() {
    updater::SemanticVersion v{};
    if (!updater::ParseVersion("1.2.3", v)) return false;
    return v.major == 1 && v.minor == 2 && v.patch == 3;
}

static bool test_version_parse_invalid() {
    updater::SemanticVersion v{};
    return !updater::ParseVersion("abc", v) &&
           !updater::ParseVersion(nullptr, v) &&
           !updater::ParseVersion("1.2", v);
}

static bool test_gate_allows_valid() {
    updater::ReleaseGate gate;
    updater::ChannelPolicy p{};
    p.channel = updater::Channel::Beta;
    p.minVersion = {0,1,0};
    gate.SetPolicy(p);
    gate.SetCurrentVersion({0,1,0});

    updater::ReleaseManifest m{};
    strcpy(m.version, "0.2.0");
    strcpy(m.channel, "beta");
    return gate.Evaluate(m) == updater::GateDecision::Allow;
}

static bool test_gate_force_update_old_version() {
    updater::ReleaseGate gate;
    updater::ChannelPolicy p{};
    p.channel = updater::Channel::Stable;
    p.minVersion = {0,2,0};
    gate.SetPolicy(p);
    gate.SetCurrentVersion({0,1,0});

    updater::ReleaseManifest m{};
    strcpy(m.version, "0.3.0");
    strcpy(m.channel, "stable");
    return gate.Evaluate(m) == updater::GateDecision::ForceUpdate;
}

static bool test_gate_blocks_maintenance() {
    updater::ReleaseGate gate;
    updater::ChannelPolicy p{};
    p.maintenanceMode = true;
    gate.SetPolicy(p);

    updater::ReleaseManifest m{};
    strcpy(m.version, "1.0.0");
    strcpy(m.channel, "stable");
    return gate.Evaluate(m) == updater::GateDecision::BlockMaintenance;
}

static bool test_gate_blocks_wrong_channel() {
    updater::ReleaseGate gate;
    updater::ChannelPolicy p{};
    p.channel = updater::Channel::Stable;
    gate.SetPolicy(p);
    gate.SetCurrentVersion({1,0,0});

    updater::ReleaseManifest m{};
    strcpy(m.version, "1.1.0");
    strcpy(m.channel, "canary");
    return gate.Evaluate(m) == updater::GateDecision::BlockUnsupported;
}

static bool test_channel_parsing() {
    return updater::ReleaseGate::ParseChannel("stable") == updater::Channel::Stable &&
           updater::ReleaseGate::ParseChannel("beta") == updater::Channel::Beta &&
           updater::ReleaseGate::ParseChannel("canary") == updater::Channel::Canary;
}

// ==========================================================
// Ed25519 tests (production crypto)
// ==========================================================

static bool test_ed25519_sign_verify_roundtrip() {
    crypto::Ed25519Provider ed;
    crypto::KeyPair kp{};
    if (!ed.GenerateKeyPair(kp)) return false;

    uint8_t msg[] = "ed25519 test message";
    uint8_t sig[crypto::kSignatureSize]{};
    if (!ed.Sign(kp.privateKey, msg, sizeof(msg) - 1, sig)) return false;
    return ed.Verify(kp.publicKey, msg, sizeof(msg) - 1, sig);
}

static bool test_ed25519_rejects_tampered() {
    crypto::Ed25519Provider ed;
    crypto::KeyPair kp{};
    if (!ed.GenerateKeyPair(kp)) return false;

    uint8_t msg[] = "original message";
    uint8_t sig[crypto::kSignatureSize]{};
    if (!ed.Sign(kp.privateKey, msg, sizeof(msg) - 1, sig)) return false;

    uint8_t tampered[] = "tampered message";
    return !ed.Verify(kp.publicKey, tampered, sizeof(tampered) - 1, sig);
}

static bool test_ed25519_rejects_wrong_key() {
    crypto::Ed25519Provider ed;
    crypto::KeyPair kp1{}, kp2{};
    if (!ed.GenerateKeyPair(kp1)) return false;
    if (!ed.GenerateKeyPair(kp2)) return false;

    uint8_t msg[] = "cross-key test";
    uint8_t sig[crypto::kSignatureSize]{};
    if (!ed.Sign(kp1.privateKey, msg, sizeof(msg) - 1, sig)) return false;
    return !ed.Verify(kp2.publicKey, msg, sizeof(msg) - 1, sig);
}

static bool test_ed25519_rejects_tampered_signature() {
    crypto::Ed25519Provider ed;
    crypto::KeyPair kp{};
    if (!ed.GenerateKeyPair(kp)) return false;

    uint8_t msg[] = "sig tamper test";
    uint8_t sig[crypto::kSignatureSize]{};
    if (!ed.Sign(kp.privateKey, msg, sizeof(msg) - 1, sig)) return false;

    sig[0] ^= 0x01;
    return !ed.Verify(kp.publicKey, msg, sizeof(msg) - 1, sig);
}

static bool test_ed25519_random_bytes() {
    crypto::Ed25519Provider ed;
    uint8_t buf[32]{};
    if (!ed.RandomBytes(buf, 32)) return false;
    for (int i = 0; i < 32; ++i)
        if (buf[i] != 0) return true;
    return false;
}

static bool test_ed25519_deterministic_signatures() {
    crypto::Ed25519Provider ed;
    crypto::KeyPair kp{};
    if (!ed.GenerateKeyPair(kp)) return false;

    uint8_t msg[] = "determinism check";
    uint8_t sig1[crypto::kSignatureSize]{}, sig2[crypto::kSignatureSize]{};
    if (!ed.Sign(kp.privateKey, msg, sizeof(msg) - 1, sig1)) return false;
    if (!ed.Sign(kp.privateKey, msg, sizeof(msg) - 1, sig2)) return false;
    return memcmp(sig1, sig2, crypto::kSignatureSize) == 0;
}

// ==========================================================

int main() {
    printf("=== SKR Client Unit Tests ===\n\n");

    printf("[Transcript]\n");
    RUN_TEST(test_transcript_deterministic);
    RUN_TEST(test_transcript_changes_with_nonce);

    printf("\n[Crypto - Mock]\n");
    RUN_TEST(test_sign_verify_roundtrip);
    RUN_TEST(test_rejects_tampered_message);
    RUN_TEST(test_rejects_wrong_key);
    RUN_TEST(test_random_bytes_nonzero);

    printf("\n[Manifest]\n");
    RUN_TEST(test_manifest_rejects_traversal);
    RUN_TEST(test_manifest_rejects_absolute);
    RUN_TEST(test_manifest_rejects_backslash);
    RUN_TEST(test_manifest_accepts_valid);
    RUN_TEST(test_manifest_roundtrip);
    RUN_TEST(test_manifest_rejects_truncated);
    RUN_TEST(test_manifest_rejects_zero_hash);

    printf("\n[Crypto - Ed25519]\n");
    RUN_TEST(test_ed25519_sign_verify_roundtrip);
    RUN_TEST(test_ed25519_rejects_tampered);
    RUN_TEST(test_ed25519_rejects_wrong_key);
    RUN_TEST(test_ed25519_rejects_tampered_signature);
    RUN_TEST(test_ed25519_random_bytes);
    RUN_TEST(test_ed25519_deterministic_signatures);

    printf("\n[Release Gate]\n");
    RUN_TEST(test_version_parse_valid);
    RUN_TEST(test_version_parse_invalid);
    RUN_TEST(test_gate_allows_valid);
    RUN_TEST(test_gate_force_update_old_version);
    RUN_TEST(test_gate_blocks_maintenance);
    RUN_TEST(test_gate_blocks_wrong_channel);
    RUN_TEST(test_channel_parsing);

    printf("\n=== %d/%d passed", g_passed, g_total);
    if (g_failed > 0) printf(", %d FAILED", g_failed);
    printf(" ===\n");
    return g_failed > 0 ? 1 : 0;
}
