#include "operation215/op215_preconditions.h"
#include <cstdio>
#include <cstring>

namespace skr::operation215 {

D0Failure ValidateD0Snapshot(const D0Snapshot& s) noexcept {
    if (!s.executableIdentity) return D0Failure::ExecutableIdentity;
    if (!s.relocationPrepared) return D0Failure::RelocationState;
    if (!s.patchSites) return D0Failure::PatchSites;
    if (!s.primaryThread) return D0Failure::PrimaryThread;
    if (!s.sampR5) return D0Failure::SampR5;
    if (s.constructed != kExpectedConstructed) return D0Failure::ConstructedCount;
    if (s.vehicleCount != 212) return D0Failure::VehicleCount;
    if (!s.model612Null) return D0Failure::Model612;
    if (!s.model613Null) return D0Failure::Model613;
    if (!s.model614Null) return D0Failure::Model614;
    if (!s.descriptor212Unused) return D0Failure::Descriptor212Used;
    if (!s.descriptor213Unused) return D0Failure::Descriptor213Used;
    if (!s.descriptor214Unused) return D0Failure::Descriptor214Used;
#ifdef SKR_OP213_CAPACITY_216
    // Model 615 is vanilla veg_treeb1 (never NULL); only the descriptor slot must be untouched.
    if (!s.descriptor215Unused) return D0Failure::Descriptor215Used;
#endif
    if (!s.model400Correct) return D0Failure::Model400;
    if (!s.model560Correct) return D0Failure::Model560;
    if (!s.model611Correct) return D0Failure::Model611;
    if (!s.originalsIntact) return D0Failure::Originals;
    return D0Failure::None;
}

const char* FailureName(D0Failure failure) noexcept {
    switch (failure) {
        case D0Failure::None: return "none";
        case D0Failure::ExecutableIdentity: return "executable_identity";
        case D0Failure::RelocationState: return "relocation_state";
        case D0Failure::PatchSites: return "patch_sites";
        case D0Failure::PrimaryThread: return "primary_thread";
        case D0Failure::SampR5: return "samp_r5";
        case D0Failure::ConstructedCount: return "constructed_count";
        case D0Failure::VehicleCount: return "vehicle_count";
        case D0Failure::Model612: return "model612";
        case D0Failure::Model613: return "model613";
        case D0Failure::Model614: return "model614";
        case D0Failure::Descriptor212Used: return "descriptor212_used";
        case D0Failure::Descriptor213Used: return "descriptor213_used";
        case D0Failure::Descriptor214Used: return "descriptor214_used";
        case D0Failure::Model400: return "model400";
        case D0Failure::Model560: return "model560";
        case D0Failure::Model611: return "model611";
        case D0Failure::Originals: return "originals";
        case D0Failure::Model615: return "model615";
        case D0Failure::Descriptor215Used: return "descriptor215_used";
    }
    return "unknown";
}

AssociationFailure ValidateAssociationChanges(const StreamAssociationEntry* manifest, size_t manifestCount,
                                              const StreamAssociationEntry* changes, size_t changeCount) noexcept {
    if (!manifest || !manifestCount) return AssociationFailure::ManifestEmpty;
    for (size_t i = 0; i < manifestCount; ++i) {
        const auto& a = manifest[i];
        if (!a.sectors || a.sector + a.sectors < a.sector) return AssociationFailure::ManifestInvalidEntry;
        for (size_t j = i + 1; j < manifestCount; ++j) {
            const auto& b = manifest[j];
            if (a.streamId == b.streamId) return AssociationFailure::ManifestDuplicateId;
            if (a.imageId == b.imageId && a.sector < b.sector + b.sectors && b.sector < a.sector + a.sectors)
                return AssociationFailure::ManifestOverlap;
        }
    }
    if (changeCount && !changes) return AssociationFailure::MissingExpected;
    for (size_t i = 0; i < changeCount; ++i)
        for (size_t j = i + 1; j < changeCount; ++j)
            if (changes[i].streamId == changes[j].streamId) return AssociationFailure::DuplicateChange;
    if (changeCount > manifestCount) return AssociationFailure::ExtraChange;
    for (size_t i = 0; i < changeCount; ++i) {
        const StreamAssociationEntry* expected = nullptr;
        for (size_t j = 0; j < manifestCount && !expected; ++j)
            if (manifest[j].streamId == changes[i].streamId) expected = &manifest[j];
        if (!expected) return AssociationFailure::UnexpectedId;
        if (changes[i].imageId != expected->imageId) return AssociationFailure::ImageId;
        if (changes[i].sector != expected->sector) return AssociationFailure::Sector;
        if (changes[i].sectors != expected->sectors) return AssociationFailure::SectorCount;
    }
    // Every change is a distinct manifest ID, so equal counts means full coverage.
    if (changeCount < manifestCount) return AssociationFailure::MissingExpected;
    return AssociationFailure::None;
}

const char* AssociationFailureName(AssociationFailure failure) noexcept {
    switch (failure) {
        case AssociationFailure::None: return "none";
        case AssociationFailure::ManifestEmpty: return "manifest_empty";
        case AssociationFailure::ManifestInvalidEntry: return "manifest_invalid_entry";
        case AssociationFailure::ManifestDuplicateId: return "manifest_duplicate_id";
        case AssociationFailure::ManifestOverlap: return "manifest_overlap";
        case AssociationFailure::MissingExpected: return "missing_expected";
        case AssociationFailure::ExtraChange: return "extra_change";
        case AssociationFailure::DuplicateChange: return "duplicate_change";
        case AssociationFailure::UnexpectedId: return "unexpected_id";
        case AssociationFailure::ImageId: return "image_id";
        case AssociationFailure::Sector: return "sector";
        case AssociationFailure::SectorCount: return "sector_count";
    }
    return "unknown";
}

PlayerSlotFailure ValidatePlayerImageSlot(int32_t index, const char* name, size_t nameCapacity,
                                          uint8_t notPlayerImgFlag, uint8_t customImageId) noexcept {
    if (index < 0 || static_cast<size_t>(index) >= kStreamingFileCount) return PlayerSlotFailure::IndexRange;
    if (static_cast<uint32_t>(index) == customImageId) return PlayerSlotFailure::CustomImage;
    constexpr char expected[] = "MODELS\\PLAYER.IMG";
    if (!name || nameCapacity < sizeof(expected)) return PlayerSlotFailure::Name;
    for (size_t i = 0; i < sizeof(expected); ++i) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c != expected[i]) return PlayerSlotFailure::Name; // includes the terminator
    }
    if (notPlayerImgFlag != 0) return PlayerSlotFailure::ScanFlag;
    return PlayerSlotFailure::None;
}

const char* PlayerSlotFailureName(PlayerSlotFailure failure) noexcept {
    switch (failure) {
        case PlayerSlotFailure::None: return "none";
        case PlayerSlotFailure::IndexRange: return "index_range";
        case PlayerSlotFailure::CustomImage: return "custom_image";
        case PlayerSlotFailure::Name: return "name";
        case PlayerSlotFailure::ScanFlag: return "scan_flag";
    }
    return "unknown";
}

DriftFailure ClassifyDelta(const PlayerImageContext& c, const AssociationDelta& d,
                           const uint32_t* protectedIds, size_t protectedCount) noexcept {
    if (!c.archiveSectors || !c.entries || !c.entryCount || !c.txdSlots || c.txdSlotCount != kPlayerTxdSlotCount ||
        c.imageId >= kStreamingFileCount) return DriftFailure::InvalidContext;
    for (size_t i = 0; i < protectedCount; ++i)
        if (protectedIds && protectedIds[i] == d.streamId) return DriftFailure::ProtectedId;
    bool reserved = d.streamId >= kPlayerReservedModelFirst &&
        d.streamId < kPlayerReservedModelFirst + kPlayerReservedModelCount;
    for (size_t i = 0; i < c.txdSlotCount && !reserved; ++i)
        reserved = c.txdSlots[i] >= 0 && static_cast<uint32_t>(c.txdSlots[i]) < kStreamTxdSlots &&
            d.streamId == kStreamTxdBase + static_cast<uint32_t>(c.txdSlots[i]);
    if (!reserved) return DriftFailure::NotReserved;
    const bool unmapped = d.before.imageId == 0 && d.before.sector == 0 && d.before.sectors == 0;
    if (!unmapped && d.before.imageId != c.imageId) return DriftFailure::BeforeState;
    if (d.after.imageId != c.imageId) return DriftFailure::AfterImage;
    if (!d.after.sectors || d.after.sector + d.after.sectors < d.after.sector ||
        d.after.sector + d.after.sectors > c.archiveSectors) return DriftFailure::ArchiveBounds;
    for (size_t i = 0; i < c.entryCount; ++i)
        if (c.entries[i].sector == d.after.sector && c.entries[i].sectors == d.after.sectors) return DriftFailure::None;
    return DriftFailure::NotInDirectory;
}

const char* DriftFailureName(DriftFailure failure) noexcept {
    switch (failure) {
        case DriftFailure::None: return "none";
        case DriftFailure::InvalidContext: return "invalid_context";
        case DriftFailure::ProtectedId: return "protected_id";
        case DriftFailure::NotReserved: return "not_reserved";
        case DriftFailure::BeforeState: return "before_state";
        case DriftFailure::AfterImage: return "after_image";
        case DriftFailure::ArchiveBounds: return "archive_bounds";
        case DriftFailure::NotInDirectory: return "not_in_directory";
    }
    return "unknown";
}

CheckpointIdentityFailure ValidateCheckpointIdentity(const CheckpointHeader& h, uint32_t pid, uint32_t tid,
                                                     uint64_t nonce, uint32_t streamingCount) noexcept {
    if (h.magic != kCheckpointMagic) return CheckpointIdentityFailure::Magic;
    if (h.version != kCheckpointVersion) return CheckpointIdentityFailure::Version;
    if (h.pid != pid) return CheckpointIdentityFailure::Pid;
    if (h.tid != tid) return CheckpointIdentityFailure::Tid;
    if (!nonce || h.nonce != nonce) return CheckpointIdentityFailure::Nonce;
    if (h.state != kCheckpointReady) return CheckpointIdentityFailure::State;
    if (h.streamingCount != streamingCount) return CheckpointIdentityFailure::Count;
    return CheckpointIdentityFailure::None;
}

const char* CheckpointIdentityFailureName(CheckpointIdentityFailure failure) noexcept {
    switch (failure) {
        case CheckpointIdentityFailure::None: return "none";
        case CheckpointIdentityFailure::Magic: return "magic";
        case CheckpointIdentityFailure::Version: return "version";
        case CheckpointIdentityFailure::Pid: return "pid";
        case CheckpointIdentityFailure::Tid: return "tid";
        case CheckpointIdentityFailure::Nonce: return "nonce";
        case CheckpointIdentityFailure::State: return "state";
        case CheckpointIdentityFailure::Count: return "count";
    }
    return "unknown";
}

bool ParseSessionStamp(const char* text, size_t length, uint32_t pid, uint32_t tid, uint64_t& nonce) noexcept {
    nonce = 0;
    constexpr char prefix[] = "OP215_BUILD ";
    if (!text || length < sizeof(prefix) - 1 || std::memcmp(text, prefix, sizeof(prefix) - 1) != 0) return false;
    size_t end = 0;
    while (end < length && text[end] != '\n') ++end;
    if (end == length) return false;
    if (end && text[end - 1] == '\r') --end;
    char tail[32]{};
    char head[40]{};
    const int tailLength = std::snprintf(tail, sizeof(tail), " pid=%lu", static_cast<unsigned long>(pid));
    const int headLength = std::snprintf(head, sizeof(head), " tid=%lu nonce=", static_cast<unsigned long>(tid));
    const size_t required = static_cast<size_t>(headLength) + 16 + static_cast<size_t>(tailLength);
    if (tailLength <= 0 || headLength <= 0 || end < sizeof(prefix) - 1 + required) return false;
    const size_t headAt = end - required;
    if (std::memcmp(text + headAt, head, static_cast<size_t>(headLength)) != 0 ||
        std::memcmp(text + end - tailLength, tail, static_cast<size_t>(tailLength)) != 0) return false;
    uint64_t value = 0;
    for (size_t i = headAt + static_cast<size_t>(headLength), n = 0; n < 16; ++i, ++n) {
        const char c = text[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        value = (value << 4) | static_cast<uint64_t>(v);
    }
    if (!value) return false;
    nonce = value;
    return true;
}

AckOutcome ClassifyAckWait(uint32_t waitResult) noexcept {
    if (waitResult == 0x00000000u) return AckOutcome::Acknowledged; // WAIT_OBJECT_0
    if (waitResult == 0x00000102u) return AckOutcome::Timeout;      // WAIT_TIMEOUT
    return AckOutcome::WaitFailed;
}

LiveCertFailure EvaluateLiveCertification(const LiveCertObservation& o) noexcept {
    if (!o.readySeen) return o.readyTimedOut ? LiveCertFailure::ReadyTimeout : LiveCertFailure::NoReady;
    if (!o.identityValid) return LiveCertFailure::Identity;
    if (!o.aliveBeforeInspection) return LiveCertFailure::ExitedBeforeInspection;
    if (!o.aliveAfterInspection) return LiveCertFailure::ExitedDuringInspection;
    if (!o.liveInvariants) return LiveCertFailure::LiveInvariant;
    if (!o.ackDelivered) return LiveCertFailure::AckNotDelivered;
    if (!o.exitedAfterAck) return LiveCertFailure::NoControlledExit;
    if (o.exitCode != kCheckpointCompleteExit) return LiveCertFailure::ExitCode;
    return LiveCertFailure::None;
}

const char* LiveCertFailureName(LiveCertFailure failure) noexcept {
    switch (failure) {
        case LiveCertFailure::None: return "none";
        case LiveCertFailure::NoReady: return "no_ready";
        case LiveCertFailure::ReadyTimeout: return "ready_timeout";
        case LiveCertFailure::Identity: return "identity";
        case LiveCertFailure::ExitedBeforeInspection: return "exited_before_inspection";
        case LiveCertFailure::ExitedDuringInspection: return "exited_during_inspection";
        case LiveCertFailure::LiveInvariant: return "live_invariant";
        case LiveCertFailure::AckNotDelivered: return "ack_not_delivered";
        case LiveCertFailure::NoControlledExit: return "no_controlled_exit";
        case LiveCertFailure::ExitCode: return "exit_code";
    }
    return "unknown";
}

} // namespace skr::operation215
