#pragma once
#include <cstdint>

namespace skr::ac {

enum class AcState : uint8_t { Unknown, Starting, Ready, Degraded, Failed };

struct AcStatus {
    AcState state;
    const char* engineVersion;
    const char* policyVersion;
};

class IAcAdapter {
public:
    virtual ~IAcAdapter() = default;
    virtual bool Start(const char* sessionId) = 0;
    virtual AcStatus Poll() = 0;
    virtual void Stop() noexcept = 0;
};

class MockAcAdapter final : public IAcAdapter {
public:
    bool Start(const char* sessionId) override;
    AcStatus Poll() override;
    void Stop() noexcept override;
private:
    AcState state_ = AcState::Unknown;
};

} // namespace skr::ac
