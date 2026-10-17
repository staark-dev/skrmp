#pragma once
#include "skrac/ac_types.h"
#include "skrac/mock_adapter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace skr::ac {

class ExternalAcAdapter final : public IAcAdapter {
public:
    ~ExternalAcAdapter() override;

    bool LoadFromPath(const wchar_t* dllPath);
    bool IsLoaded() const noexcept { return module_ != nullptr; }

    bool Start(const char* sessionId) override;
    AcState GetState() override;
    bool PollStatus(AcStatusResult& out) override;
    bool GetHeartbeat(AcHeartbeat& out) override;
    void Stop() noexcept override;
    bool IsMock() const noexcept override { return false; }
    const char* GetEngineName() const noexcept override { return "skrac"; }

private:
    void Unload() noexcept;

    HMODULE module_ = nullptr;
    SkracInitFn    fnInit_    = nullptr;
    SkracPollFn    fnPoll_    = nullptr;
    SkracBeatFn    fnBeat_    = nullptr;
    SkracStopFn    fnStop_    = nullptr;
    SkracVersionFn fnVersion_ = nullptr;

    AcState cachedState_ = AcState::Unknown;
    bool started_ = false;
};

} // namespace skr::ac
