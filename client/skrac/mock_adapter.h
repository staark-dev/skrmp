#pragma once
#include "skrac/ac_types.h"
#include <cstdint>

namespace skr::ac {

class IAcAdapter {
public:
    virtual ~IAcAdapter() = default;
    virtual bool Start(const char* sessionId) = 0;
    virtual AcState GetState() = 0;
    virtual bool PollStatus(AcStatusResult& out) = 0;
    virtual bool GetHeartbeat(AcHeartbeat& out) = 0;
    virtual void Stop() noexcept = 0;
    virtual bool IsMock() const noexcept = 0;
    virtual const char* GetEngineName() const noexcept = 0;
};

enum class MockScenario : uint8_t {
    AlwaysReady,
    DelayedStart,
    IntermittentDegraded,
    TimeoutOnPoll,
    FailAfterN
};

class MockAcAdapter final : public IAcAdapter {
public:
    explicit MockAcAdapter(MockScenario scenario = MockScenario::AlwaysReady);

    bool Start(const char* sessionId) override;
    AcState GetState() override;
    bool PollStatus(AcStatusResult& out) override;
    bool GetHeartbeat(AcHeartbeat& out) override;
    void Stop() noexcept override;
    bool IsMock() const noexcept override { return true; }
    const char* GetEngineName() const noexcept override { return "mock"; }

private:
    AcState state_ = AcState::Unknown;
    MockScenario scenario_;
    uint32_t pollCount_ = 0;
    uint32_t heartbeatSeq_ = 0;
    uint64_t startTimeMs_ = 0;
};

} // namespace skr::ac
