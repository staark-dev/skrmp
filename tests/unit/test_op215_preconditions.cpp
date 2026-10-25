#include "operation215/op215_preconditions.h"
#include <cstdio>

#include <vector>

using skr::operation215::AssociationFailure;
using skr::operation215::AssociationFailureName;
using skr::operation215::D0Failure;
using skr::operation215::D0Snapshot;
using skr::operation215::StreamAssociationEntry;
using skr::operation215::ValidateAssociationChanges;
using skr::operation215::ValidateD0Snapshot;

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    if (!condition) { std::printf("FAIL %s\n", name); ++failures; }
}

D0Snapshot Passing() {
    D0Snapshot s{};
    s.executableIdentity = s.relocationPrepared = s.patchSites = true;
    s.primaryThread = s.sampR5 = true;
    s.constructed = skr::operation215::kExpectedConstructed;
    s.vehicleCount = 212;
    s.model612Null = s.model613Null = s.model614Null = true;
    s.descriptor212Unused = s.descriptor213Unused = s.descriptor214Unused = true;
    s.model615Null = s.descriptor215Unused = true;
    s.model400Correct = s.model560Correct = s.model611Correct = true;
    s.originalsIntact = true;
    return s;
}

// Pinned CUSTOM.IMG manifest: expveh.dff, original NoModelFile.txd, expveh.txd.
const std::vector<StreamAssociationEntry> kManifest{
    {612, 0, 21, 122},
    {23058, 0, 2, 2},
    {23280, 0, 4, 17},
};

void Association(std::vector<StreamAssociationEntry> manifest, std::vector<StreamAssociationEntry> changes,
                 AssociationFailure expected, const char* name) {
    const AssociationFailure actual = ValidateAssociationChanges(manifest.data(), manifest.size(),
                                                                 changes.data(), changes.size());
    if (actual != expected) std::printf("  got=%s expected=%s\n", AssociationFailureName(actual), AssociationFailureName(expected));
    Check(actual == expected, name);
}

void AssociationTests() {
    const auto exact = kManifest;
    Association(kManifest, exact, AssociationFailure::None, "assoc exact three accepted");
    Association(kManifest, {exact[2], exact[0], exact[1]}, AssociationFailure::None, "assoc order independent");

    auto fourth = exact; fourth.push_back({23281, 0, 143, 1});
    Association(kManifest, fourth, AssociationFailure::ExtraChange, "assoc fourth rejected");
    auto fourthKnownRange = exact; fourthKnownRange.push_back({400, 0, 21, 122});
    Association(kManifest, fourthKnownRange, AssociationFailure::ExtraChange, "assoc fourth aliasing dff rejected");

    auto wrongSector = exact; wrongSector[0].sector = 22;
    Association(kManifest, wrongSector, AssociationFailure::Sector, "assoc wrong dff sector rejected");
    auto wrongTxdSector = exact; wrongTxdSector[2].sector = 3;
    Association(kManifest, wrongTxdSector, AssociationFailure::Sector, "assoc wrong txd sector rejected");
    auto wrongCount = exact; wrongCount[2].sectors = 16;
    Association(kManifest, wrongCount, AssociationFailure::SectorCount, "assoc wrong sector count rejected");
    auto wrongPlaceholderCount = exact; wrongPlaceholderCount[1].sectors = 3;
    Association(kManifest, wrongPlaceholderCount, AssociationFailure::SectorCount, "assoc wrong placeholder size rejected");
    auto wrongImage = exact; wrongImage[1].imageId = 1;
    Association(kManifest, wrongImage, AssociationFailure::ImageId, "assoc wrong img id rejected");

    auto wrongPlaceholderId = exact; wrongPlaceholderId[1].streamId = 23059;
    Association(kManifest, wrongPlaceholderId, AssociationFailure::UnexpectedId, "assoc wrong original txd id rejected");
    auto unrelated = exact; unrelated[0].streamId = 611;
    Association(kManifest, unrelated, AssociationFailure::UnexpectedId, "assoc unrelated model id rejected");

    Association(kManifest, {exact[0], exact[2]}, AssociationFailure::MissingExpected, "assoc missing placeholder rejected");
    Association(kManifest, {exact[1], exact[2]}, AssociationFailure::MissingExpected, "assoc missing dff rejected");
    Association(kManifest, {}, AssociationFailure::MissingExpected, "assoc no changes rejected");
    Association(kManifest, {exact[0], exact[0], exact[2]}, AssociationFailure::DuplicateChange, "assoc duplicate change rejected");

    Association({}, exact, AssociationFailure::ManifestEmpty, "assoc empty manifest rejected");
    auto duplicateManifest = kManifest; duplicateManifest[2].streamId = 612;
    Association(duplicateManifest, exact, AssociationFailure::ManifestDuplicateId, "assoc duplicate manifest id rejected");
    auto overlapManifest = kManifest; overlapManifest[1].sectors = 3; // 2..5 overlaps txd at 4
    Association(overlapManifest, exact, AssociationFailure::ManifestOverlap, "assoc overlapping manifest range rejected");
    auto zeroManifest = kManifest; zeroManifest[0].sectors = 0;
    Association(zeroManifest, exact, AssociationFailure::ManifestInvalidEntry, "assoc zero-size manifest entry rejected");
    auto otherImage = kManifest; otherImage[1].imageId = 1; otherImage[1].sector = 4; // same range, different image
    auto otherImageChanges = exact; otherImageChanges[1] = otherImage[1];
    Association(otherImage, otherImageChanges, AssociationFailure::None, "assoc same range other image allowed");
}

namespace op = skr::operation215;

// Run 2 observed deltas, each an exact PLAYER.IMG (pinned) directory entry.
const std::vector<op::ImgDirEntry> kPlayerEntries{
    {7497, 61}, {4692, 78}, {4515, 40}, {5153, 46}, {6713, 31},
    {23075, 33}, {25606, 9}, {23117, 193}, {21507, 65},
};
const int16_t kClothesTxd[4]{0, 1, 2, 3};
const uint32_t kProtected[]{612, 23058, 23280};

op::PlayerImageContext PlayerContext() {
    return {7, 32587, kClothesTxd, 4, kPlayerEntries.data(), kPlayerEntries.size()};
}

void Drift(const op::PlayerImageContext& c, op::AssociationDelta d, op::DriftFailure expected, const char* name) {
    const op::DriftFailure actual = op::ClassifyDelta(c, d, kProtected, std::size(kProtected));
    if (actual != expected) std::printf("  got=%s expected=%s\n", op::DriftFailureName(actual), op::DriftFailureName(expected));
    Check(actual == expected, name);
}

void PlayerImageTests() {
    using F = op::PlayerSlotFailure;
    const char good[0x28] = "MODELS\\PLAYER.IMG";
    const char lower[0x28] = "models\\player.img";
    const char cutscene[0x28] = "MODELS\\CUTSCENE.IMG";
    const char longer[0x28] = "MODELS\\PLAYER.IMGX";
    Check(op::ValidatePlayerImageSlot(7, good, sizeof(good), 0, 0) == F::None, "player slot 7 accepted");
    Check(op::ValidatePlayerImageSlot(3, good, sizeof(good), 0, 0) == F::None, "player slot resolved, not assumed 7");
    Check(op::ValidatePlayerImageSlot(7, lower, sizeof(lower), 0, 0) == F::None, "player slot name case-insensitive");
    Check(op::ValidatePlayerImageSlot(6, cutscene, sizeof(cutscene), 0, 0) == F::Name, "wrong PLAYER.IMG slot rejected");
    Check(op::ValidatePlayerImageSlot(7, longer, sizeof(longer), 0, 0) == F::Name, "player name suffix rejected");
    Check(op::ValidatePlayerImageSlot(8, good, sizeof(good), 0, 0) == F::IndexRange, "player slot 8 rejected");
    Check(op::ValidatePlayerImageSlot(-1, good, sizeof(good), 0, 0) == F::IndexRange, "player slot -1 rejected");
    Check(op::ValidatePlayerImageSlot(0, good, sizeof(good), 0, 0) == F::CustomImage, "player slot = custom img rejected");
    Check(op::ValidatePlayerImageSlot(7, good, sizeof(good), 1, 0) == F::ScanFlag, "player slot scanned flag rejected");
    Check(op::ValidatePlayerImageSlot(7, nullptr, 0, 0, 0) == F::Name, "player slot null name rejected");

    using D = op::DriftFailure;
    const auto c = PlayerContext();
    const op::Association unmapped{0, 0, 0};
    const uint32_t ids[]{384, 385, 386, 387, 388, 20000, 20001, 20002, 20003};
    for (size_t i = 0; i < std::size(ids); ++i)
        Drift(c, {ids[i], unmapped, {7, kPlayerEntries[i].sector, kPlayerEntries[i].sectors}}, D::None, "run2 delta accepted");
    Drift(c, {393, unmapped, {7, 7497, 61}}, D::None, "reserved model 393 accepted");
    Drift(c, {384, {7, 4692, 78}, {7, 7497, 61}}, D::None, "player-to-player remap accepted");

    Drift(c, {383, unmapped, {7, 7497, 61}}, D::NotReserved, "model 383 rejected");
    Drift(c, {394, unmapped, {7, 7497, 61}}, D::NotReserved, "model 394 rejected");
    Drift(c, {611, unmapped, {7, 7497, 61}}, D::NotReserved, "vehicle 611 rejected");
    Drift(c, {20004, unmapped, {7, 23075, 33}}, D::NotReserved, "txd 20004 rejected");
    Drift(c, {1000, unmapped, {1, 100, 2}}, D::NotReserved, "fourth-party mutation rejected");
    Drift(c, {384, {2, 500, 10}, {7, 7497, 61}}, D::BeforeState, "remap from gta3.img rejected");
    Drift(c, {384, unmapped, {6, 7497, 61}}, D::AfterImage, "wrong after image rejected");
    Drift(c, {384, unmapped, {7, 7498, 61}}, D::NotInDirectory, "incorrect sector rejected");
    Drift(c, {384, unmapped, {7, 7497, 60}}, D::NotInDirectory, "incorrect size rejected");
    Drift(c, {384, unmapped, {7, 32580, 10}}, D::ArchiveBounds, "beyond archive rejected");
    Drift(c, {384, unmapped, {7, 7497, 0}}, D::ArchiveBounds, "zero size rejected");
    auto small = c; small.archiveSectors = 7500;
    Drift(small, {384, unmapped, {7, 7497, 61}}, D::ArchiveBounds, "incorrect archive size rejected");

    Drift(c, {612, unmapped, {7, 7497, 61}}, D::ProtectedId, "model 612 drift rejected");
    Drift(c, {23058, unmapped, {7, 7497, 61}}, D::ProtectedId, "custom placeholder drift rejected");
    Drift(c, {23280, {0, 4, 17}, {0, 5, 17}}, D::ProtectedId, "expveh txd drift rejected");
    const int16_t overlapTxd[4]{3280, 1, 2, 3}; // protected wins over reserved
    auto overlap = c; overlap.txdSlots = overlapTxd;
    Drift(overlap, {23280, unmapped, {7, 23075, 33}}, D::ProtectedId, "protected beats reserved txd");

    auto badContext = c; badContext.txdSlotCount = 3;
    Drift(badContext, {384, unmapped, {7, 7497, 61}}, D::InvalidContext, "invalid context rejected");
    auto noDirectory = c; noDirectory.entryCount = 0;
    Drift(noDirectory, {384, unmapped, {7, 7497, 61}}, D::InvalidContext, "missing directory rejected");
}

void CheckpointTests() {
    using I = op::CheckpointIdentityFailure;
    const op::CheckpointHeader good{op::kCheckpointMagic, op::kCheckpointVersion, 2040, 9012, 0x1122334455667788ull,
                                    op::kCheckpointReady, 26316, 7, 3280};
    Check(op::ValidateCheckpointIdentity(good, 2040, 9012, 0x1122334455667788ull, 26316) == I::None, "checkpoint identity ok");
    Check(op::ValidateCheckpointIdentity(good, 2041, 9012, 0x1122334455667788ull, 26316) == I::Pid, "stale pid rejected");
    Check(op::ValidateCheckpointIdentity(good, 2040, 9013, 0x1122334455667788ull, 26316) == I::Tid, "stale tid rejected");
    Check(op::ValidateCheckpointIdentity(good, 2040, 9012, 0x1122334455667789ull, 26316) == I::Nonce, "stale nonce rejected");
    Check(op::ValidateCheckpointIdentity(good, 2040, 9012, 0, 26316) == I::Nonce, "zero nonce rejected");
    auto h = good; h.magic = 0;
    Check(op::ValidateCheckpointIdentity(h, 2040, 9012, good.nonce, 26316) == I::Magic, "bad magic rejected");
    h = good; h.version = 2;
    Check(op::ValidateCheckpointIdentity(h, 2040, 9012, good.nonce, 26316) == I::Version, "bad version rejected");
    h = good; h.state = 0;
    Check(op::ValidateCheckpointIdentity(h, 2040, 9012, good.nonce, 26316) == I::State, "not-ready checkpoint rejected");
    Check(op::ValidateCheckpointIdentity(good, 2040, 9012, good.nonce, 26315) == I::Count, "baseline count rejected");

    const char stamp[] = "OP215_BUILD x=ON tid=9012 nonce=1122334455667788 pid=2040\r\nOP215_D2B0 ...\n";
    uint64_t nonce = 0;
    Check(op::ParseSessionStamp(stamp, sizeof(stamp) - 1, 2040, 9012, nonce) && nonce == 0x1122334455667788ull,
          "session stamp parsed");
    Check(!op::ParseSessionStamp(stamp, sizeof(stamp) - 1, 2041, 9012, nonce), "stale session pid rejected");
    Check(!op::ParseSessionStamp(stamp, sizeof(stamp) - 1, 2040, 9011, nonce), "stale session tid rejected");
    const char oldFormat[] = "OP215_BUILD x=ON pid=2040\r\n";
    Check(!op::ParseSessionStamp(oldFormat, sizeof(oldFormat) - 1, 2040, 9012, nonce), "stamp without nonce rejected");
    const char zero[] = "OP215_BUILD tid=9012 nonce=0000000000000000 pid=2040\n";
    Check(!op::ParseSessionStamp(zero, sizeof(zero) - 1, 2040, 9012, nonce), "zero nonce stamp rejected");
    const char lowerHex[] = "OP215_BUILD tid=9012 nonce=112233445566778a pid=2040\n";
    Check(!op::ParseSessionStamp(lowerHex, sizeof(lowerHex) - 1, 2040, 9012, nonce), "malformed nonce rejected");
    const char noNewline[] = "OP215_BUILD tid=9012 nonce=1122334455667788 pid=2040";
    Check(!op::ParseSessionStamp(noNewline, sizeof(noNewline) - 1, 2040, 9012, nonce), "partial first line rejected");
    const char pidSuffix[] = "OP215_BUILD tid=9012 nonce=1122334455667788 pid=20401\n";
    Check(!op::ParseSessionStamp(pidSuffix, sizeof(pidSuffix) - 1, 2040, 9012, nonce), "pid prefix match rejected");

    Check(op::ClassifyAckWait(0) == op::AckOutcome::Acknowledged, "ack received");
    Check(op::ClassifyAckWait(0x102) == op::AckOutcome::Timeout, "missing ack -> timeout");
    Check(op::ClassifyAckWait(0xFFFFFFFFu) == op::AckOutcome::WaitFailed, "ack wait failure");

    using L = op::LiveCertFailure;
    const op::LiveCertObservation ok{true, false, true, true, true, true, true, true, op::kCheckpointCompleteExit};
    Check(op::EvaluateLiveCertification(ok) == L::None, "live cert pass");
    auto o = ok; o.readySeen = false; o.readyTimedOut = true;
    Check(op::EvaluateLiveCertification(o) == L::ReadyTimeout, "harness ready timeout");
    o = ok; o.readySeen = false;
    Check(op::EvaluateLiveCertification(o) == L::NoReady, "no ready (process died)");
    o = ok; o.identityValid = false;
    Check(op::EvaluateLiveCertification(o) == L::Identity, "stale identity rejected");
    o = ok; o.aliveBeforeInspection = false;
    Check(op::EvaluateLiveCertification(o) == L::ExitedBeforeInspection, "exit before inspection");
    o = ok; o.aliveAfterInspection = false;
    Check(op::EvaluateLiveCertification(o) == L::ExitedDuringInspection, "exit during inspection");
    o = ok; o.liveInvariants = false;
    Check(op::EvaluateLiveCertification(o) == L::LiveInvariant, "live invariant failure");
    o = ok; o.ackDelivered = false;
    Check(op::EvaluateLiveCertification(o) == L::AckNotDelivered, "missing live ack rejected");
    o = ok; o.exitedAfterAck = false;
    Check(op::EvaluateLiveCertification(o) == L::NoControlledExit, "no controlled exit after ack");
    o = ok; o.exitCode = 0xE215D1;
    Check(op::EvaluateLiveCertification(o) == L::ExitCode, "fail-closed exit is not a pass");
}
}

int main() {
    auto s = Passing();
    Check(ValidateD0Snapshot(s) == D0Failure::None, "passing snapshot");
#define NEGATIVE(field, value, expected) do { auto n = Passing(); n.field = value; \
    Check(ValidateD0Snapshot(n) == D0Failure::expected, #field); } while (false)
    NEGATIVE(executableIdentity, false, ExecutableIdentity);
    NEGATIVE(relocationPrepared, false, RelocationState);
    NEGATIVE(patchSites, false, PatchSites);
    NEGATIVE(primaryThread, false, PrimaryThread);
    NEGATIVE(sampR5, false, SampR5);
    NEGATIVE(constructed, skr::operation215::kExpectedConstructed - 1, ConstructedCount);
    NEGATIVE(constructed, skr::operation215::kExpectedConstructed + 1, ConstructedCount);
#ifdef SKR_OP213_CAPACITY_216
    Check(skr::operation215::kExpectedConstructed == 216, "capacity216: D0 expects 216 constructed descriptors");
    NEGATIVE(constructed, 215, ConstructedCount);
    NEGATIVE(descriptor215Unused, false, Descriptor215Used);
    { auto n = Passing(); n.model615Null = false;   // 615 = vanilla veg_treeb1 (runtime census)
      Check(ValidateD0Snapshot(n) == D0Failure::None, "capacity216: vanilla model 615 (veg_treeb1) is not a D0 failure"); }
#else
    Check(skr::operation215::kExpectedConstructed == 215, "certified: D0 expects 215 constructed descriptors");
    { auto n = Passing(); n.model615Null = false; n.descriptor215Unused = false;
      Check(ValidateD0Snapshot(n) == D0Failure::None, "certified: slot 215 fields ignored at capacity 215"); }
#endif
    NEGATIVE(vehicleCount, 211, VehicleCount);
    NEGATIVE(model612Null, false, Model612);
    NEGATIVE(model613Null, false, Model613);
    NEGATIVE(model614Null, false, Model614);
    NEGATIVE(descriptor212Unused, false, Descriptor212Used);
    NEGATIVE(descriptor213Unused, false, Descriptor213Used);
    NEGATIVE(descriptor214Unused, false, Descriptor214Used);
    NEGATIVE(model400Correct, false, Model400);
    NEGATIVE(model560Correct, false, Model560);
    NEGATIVE(model611Correct, false, Model611);
    NEGATIVE(originalsIntact, false, Originals);
#undef NEGATIVE
    std::printf("OP215 negative preconditions: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    const int preconditionFailures = failures;
    AssociationTests();
    std::printf("OP215 association allowlist: %s (%d failures)\n",
                failures != preconditionFailures ? "FAIL" : "PASS", failures - preconditionFailures);
    const int allowlistFailures = failures;
    PlayerImageTests();
    std::printf("OP215 player-image drift classifier: %s (%d failures)\n",
                failures != allowlistFailures ? "FAIL" : "PASS", failures - allowlistFailures);
    const int driftFailures = failures;
    CheckpointTests();
    std::printf("OP215 live checkpoint identity/ack: %s (%d failures)\n",
                failures != driftFailures ? "FAIL" : "PASS", failures - driftFailures);
    return failures ? 1 : 0;
}
