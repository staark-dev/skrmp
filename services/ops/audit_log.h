#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>

namespace skr::ops {

constexpr size_t kMaxAuditEntries = 1024;
constexpr size_t kMaxEventType = 32;
constexpr size_t kMaxAuditDetail = 128;
constexpr size_t kMaxActorId = 64;

enum class AuditCategory : uint8_t {
    Auth      = 0,
    Ticket    = 1,
    Gate      = 2,
    Key       = 3,
    Ac        = 4,
    Ops       = 5
};

struct AuditEntry {
    uint64_t timestamp = 0;
    uint64_t sequenceId = 0;
    AuditCategory category = AuditCategory::Auth;
    char eventType[kMaxEventType]{};
    char actorId[kMaxActorId]{};
    uint32_t accountId = 0;
    uint16_t playerId = 0;
    char detail[kMaxAuditDetail]{};
    bool success = true;
};

class AuditLog {
public:
    void Record(AuditCategory category, const char* eventType,
                const char* actorId, uint32_t accountId,
                uint16_t playerId, const char* detail, bool success,
                uint64_t timestamp);

    uint32_t GetEntryCount() const;
    const AuditEntry* GetEntry(uint32_t index) const;

    // Query recent entries by category
    uint32_t QueryByCategory(AuditCategory category,
                             AuditEntry* out, uint32_t maxOut) const;

    // Query by account
    uint32_t QueryByAccount(uint32_t accountId,
                            AuditEntry* out, uint32_t maxOut) const;

    void Clear();

private:
    mutable std::mutex mutex_;
    AuditEntry entries_[kMaxAuditEntries]{};
    uint32_t count_ = 0;
    uint64_t nextSeq_ = 1;
    uint32_t writePos_ = 0; // circular buffer position
};

} // namespace skr::ops
