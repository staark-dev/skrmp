#pragma once

#include <cstddef>
#include <cstdint>

namespace skr::operation215 {

struct D0Snapshot {
    bool executableIdentity = false;
    bool relocationPrepared = false;
    bool patchSites = false;
    bool primaryThread = false;
    bool sampR5 = false;
    uint32_t constructed = 0;
    uint32_t vehicleCount = 0;
    bool model612Null = false;
    bool model613Null = false;
    bool model614Null = false;
    bool descriptor212Unused = false;
    bool descriptor213Unused = false;
    bool descriptor214Unused = false;
    bool model400Correct = false;
    bool model560Correct = false;
    bool model611Correct = false;
    bool originalsIntact = false;
    // Capacity-216 builds only: descriptor 215 must be untouched at D0 (no model pointer to it).
    // model615Null is informational: 615 is vanilla veg_treeb1 and is expected to be non-null.
    bool model615Null = false;
    bool descriptor215Unused = false;
};

#ifdef SKR_OP213_CAPACITY_216
constexpr uint32_t kExpectedConstructed = 216;
#else
constexpr uint32_t kExpectedConstructed = 215;
#endif

enum class D0Failure : uint32_t {
    None = 0,
    ExecutableIdentity,
    RelocationState,
    PatchSites,
    PrimaryThread,
    SampR5,
    ConstructedCount,
    VehicleCount,
    Model612,
    Model613,
    Model614,
    Descriptor212Used,
    Descriptor213Used,
    Descriptor214Used,
    Model400,
    Model560,
    Model611,
    Originals,
    Model615,
    Descriptor215Used,
};

D0Failure ValidateD0Snapshot(const D0Snapshot& snapshot) noexcept;
const char* FailureName(D0Failure failure) noexcept;

// One streaming association (CStreamingInfo image/sector/size) as published by
// the native CUSTOM.IMG directory scanner.
struct StreamAssociationEntry {
    uint32_t streamId;
    uint8_t imageId;
    uint32_t sector;
    uint32_t sectors;
};

enum class AssociationFailure : uint32_t {
    None = 0,
    ManifestEmpty,
    ManifestInvalidEntry,
    ManifestDuplicateId,
    ManifestOverlap,
    MissingExpected,
    ExtraChange,
    DuplicateChange,
    UnexpectedId,
    ImageId,
    Sector,
    SectorCount,
};

// Accepts the observed set of changed associations only if it is exactly the
// manifest: same streaming IDs (each once), same image, sector and size.
// The manifest itself must have unique IDs and non-overlapping sector ranges.
AssociationFailure ValidateAssociationChanges(const StreamAssociationEntry* manifest, size_t manifestCount,
                                              const StreamAssociationEntry* changes, size_t changeCount) noexcept;
const char* AssociationFailureName(AssociationFailure failure) noexcept;

// ---- Post-request drift: native player-model rebuild from PLAYER.IMG ----
// Binary-derived (pinned 1.0 US exe):
//  * CClothes::Init (0x5A80D0) registers MODELS\PLAYER.IMG via AddImageToList with
//    bNotPlayerImg=0 and stores the slot at 0xBC12F8; LoadCdDirectory() skips it.
//  * CStreaming::RequestFile (0x40A080) remaps a streaming slot to that image; its
//    callers use model IDs 384..393 (loop bound `id-384 < 10` at 0x5A56A2) and TXD
//    slots from the 4-entry table at 0xC88004 (filled by CTxdStore::Initialise).
constexpr uint32_t kPlayerReservedModelFirst = 384;
constexpr uint32_t kPlayerReservedModelCount = 10;
constexpr size_t kPlayerTxdSlotCount = 4;
constexpr uint32_t kStreamTxdBase = 20000;
constexpr uint32_t kStreamTxdSlots = 5000;
constexpr size_t kStreamingFileCount = 8;

struct Association { uint8_t imageId; uint32_t sector; uint32_t sectors; };
struct AssociationDelta { uint32_t streamId; Association before; Association after; };
struct ImgDirEntry { uint32_t sector; uint32_t sectors; };

struct PlayerImageContext {
    uint8_t imageId;
    uint32_t archiveSectors;
    const int16_t* txdSlots;
    size_t txdSlotCount;
    const ImgDirEntry* entries;
    size_t entryCount;
};

enum class PlayerSlotFailure : uint32_t { None = 0, IndexRange, CustomImage, Name, ScanFlag };
// `name` is the CStreamingFile name (at most 40 bytes, need not be terminated).
PlayerSlotFailure ValidatePlayerImageSlot(int32_t index, const char* name, size_t nameCapacity,
                                          uint8_t notPlayerImgFlag, uint8_t customImageId) noexcept;
const char* PlayerSlotFailureName(PlayerSlotFailure failure) noexcept;

enum class DriftFailure : uint32_t {
    None = 0, InvalidContext, ProtectedId, NotReserved, BeforeState, AfterImage, ArchiveBounds, NotInDirectory,
};
// A single changed association is legitimate only if it is a proven reserved
// player slot, moved from unmapped (or from PLAYER.IMG) to an exact PLAYER.IMG
// directory entry that lies inside the archive.
DriftFailure ClassifyDelta(const PlayerImageContext& context, const AssociationDelta& delta,
                           const uint32_t* protectedIds, size_t protectedCount) noexcept;
const char* DriftFailureName(DriftFailure failure) noexcept;

// ---- Live inspection checkpoint (client <-> harness) ----
constexpr uint32_t kCheckpointMagic = 0x34423244; // "D2B4"
constexpr uint32_t kCheckpointVersion = 1;
constexpr uint32_t kCheckpointReady = 1;
constexpr uint32_t kCheckpointCompleteExit = 0x00D2B4AC;
constexpr uint32_t kCheckpointAckTimeoutMs = 20000;

struct CheckpointHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t tid;
    uint64_t nonce;
    uint32_t state;
    uint32_t streamingCount;
    int32_t playerImageId;
    int32_t expTxdSlot;
    // followed by streamingCount Association records (pre-request baseline)
};

enum class CheckpointIdentityFailure : uint32_t { None = 0, Magic, Version, Pid, Tid, Nonce, State, Count };
CheckpointIdentityFailure ValidateCheckpointIdentity(const CheckpointHeader& header, uint32_t pid, uint32_t tid,
                                                     uint64_t nonce, uint32_t streamingCount) noexcept;
const char* CheckpointIdentityFailureName(CheckpointIdentityFailure failure) noexcept;

// The first log line must start with "OP215_BUILD " and end with
// " tid=<tid> nonce=<16 uppercase hex> pid=<pid>" (CRLF or LF); nonce must be non-zero.
bool ParseSessionStamp(const char* text, size_t length, uint32_t pid, uint32_t tid, uint64_t& nonce) noexcept;

enum class AckOutcome : uint32_t { Acknowledged = 0, Timeout, WaitFailed };
AckOutcome ClassifyAckWait(uint32_t waitResult) noexcept; // WaitForSingleObject result

struct LiveCertObservation {
    bool readySeen;
    bool readyTimedOut;
    bool identityValid;
    bool aliveBeforeInspection;
    bool aliveAfterInspection;
    bool liveInvariants;
    bool ackDelivered;
    bool exitedAfterAck;
    uint32_t exitCode;
};
enum class LiveCertFailure : uint32_t {
    None = 0, NoReady, ReadyTimeout, Identity, ExitedBeforeInspection, ExitedDuringInspection,
    LiveInvariant, AckNotDelivered, NoControlledExit, ExitCode,
};
LiveCertFailure EvaluateLiveCertification(const LiveCertObservation& observation) noexcept;
const char* LiveCertFailureName(LiveCertFailure failure) noexcept;

} // namespace skr::operation215
