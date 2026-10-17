#include "audit_log.h"

namespace skr::ops {

void AuditLog::Record(AuditCategory category, const char* eventType,
                      const char* actorId, uint32_t accountId,
                      uint16_t playerId, const char* detail, bool success,
                      uint64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& entry = entries_[writePos_];
    entry.timestamp = timestamp;
    entry.sequenceId = nextSeq_++;
    entry.category = category;
    entry.accountId = accountId;
    entry.playerId = playerId;
    entry.success = success;

    memset(entry.eventType, 0, kMaxEventType);
    memset(entry.actorId, 0, kMaxActorId);
    memset(entry.detail, 0, kMaxAuditDetail);

    if (eventType) strncpy(entry.eventType, eventType, kMaxEventType - 1);
    if (actorId) strncpy(entry.actorId, actorId, kMaxActorId - 1);
    if (detail) strncpy(entry.detail, detail, kMaxAuditDetail - 1);

    writePos_ = (writePos_ + 1) % kMaxAuditEntries;
    if (count_ < kMaxAuditEntries) ++count_;
}

uint32_t AuditLog::GetEntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

const AuditEntry* AuditLog::GetEntry(uint32_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= count_) return nullptr;

    // Entries are in circular buffer; oldest is at (writePos_ - count_)
    uint32_t realIdx;
    if (count_ < kMaxAuditEntries) {
        realIdx = index;
    } else {
        realIdx = (writePos_ + index) % kMaxAuditEntries;
    }
    return &entries_[realIdx];
}

uint32_t AuditLog::QueryByCategory(AuditCategory category,
                                    AuditEntry* out, uint32_t maxOut) const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t found = 0;

    for (uint32_t i = 0; i < count_ && found < maxOut; ++i) {
        uint32_t idx = (count_ < kMaxAuditEntries)
            ? i
            : (writePos_ + i) % kMaxAuditEntries;
        if (entries_[idx].category == category) {
            out[found++] = entries_[idx];
        }
    }
    return found;
}

uint32_t AuditLog::QueryByAccount(uint32_t accountId,
                                   AuditEntry* out, uint32_t maxOut) const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t found = 0;

    for (uint32_t i = 0; i < count_ && found < maxOut; ++i) {
        uint32_t idx = (count_ < kMaxAuditEntries)
            ? i
            : (writePos_ + i) % kMaxAuditEntries;
        if (entries_[idx].accountId == accountId) {
            out[found++] = entries_[idx];
        }
    }
    return found;
}

void AuditLog::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    count_ = 0;
    writePos_ = 0;
    nextSeq_ = 1;
}

} // namespace skr::ops
