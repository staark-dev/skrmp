#include "skrac/external_adapter.h"
#include <cstring>

namespace skr::ac {

ExternalAcAdapter::~ExternalAcAdapter() {
    if (started_) Stop();
    Unload();
}

bool ExternalAcAdapter::LoadFromPath(const wchar_t* dllPath) {
    if (!dllPath) return false;
    Unload();

    module_ = LoadLibraryW(dllPath);
    if (!module_) return false;

    fnVersion_ = reinterpret_cast<SkracVersionFn>(GetProcAddress(module_, "skrac_version"));
    if (!fnVersion_) { Unload(); return false; }

    uint32_t ver = fnVersion_();
    if (ver < kAcApiVersion) { Unload(); return false; }

    fnInit_ = reinterpret_cast<SkracInitFn>(GetProcAddress(module_, "skrac_init"));
    fnPoll_ = reinterpret_cast<SkracPollFn>(GetProcAddress(module_, "skrac_poll"));
    fnBeat_ = reinterpret_cast<SkracBeatFn>(GetProcAddress(module_, "skrac_heartbeat"));
    fnStop_ = reinterpret_cast<SkracStopFn>(GetProcAddress(module_, "skrac_stop"));

    if (!fnInit_ || !fnPoll_ || !fnStop_) {
        Unload();
        return false;
    }

    return true;
}

void ExternalAcAdapter::Unload() noexcept {
    fnInit_ = nullptr;
    fnPoll_ = nullptr;
    fnBeat_ = nullptr;
    fnStop_ = nullptr;
    fnVersion_ = nullptr;
    if (module_) {
        FreeLibrary(module_);
        module_ = nullptr;
    }
    cachedState_ = AcState::Unknown;
    started_ = false;
}

bool ExternalAcAdapter::Start(const char* sessionId) {
    if (!module_ || !fnInit_ || !sessionId) return false;

    AcInitParams params{};
    params.structSize = sizeof(AcInitParams);
    params.apiVersion = kAcApiVersion;
    strncpy(params.sessionId, sessionId, kMaxSessionIdLen - 1);
    params.flags = 0;

    int32_t result = fnInit_(&params);
    if (result != 0) {
        cachedState_ = AcState::Failed;
        return false;
    }

    started_ = true;
    cachedState_ = AcState::Starting;
    return true;
}

AcState ExternalAcAdapter::GetState() {
    if (!started_) return AcState::Unknown;

    AcStatusResult status{};
    if (PollStatus(status)) {
        return status.state;
    }
    return cachedState_;
}

bool ExternalAcAdapter::PollStatus(AcStatusResult& out) {
    if (!started_ || !fnPoll_) return false;

    memset(&out, 0, sizeof(out));
    out.structSize = sizeof(AcStatusResult);

    int32_t result = fnPoll_(&out);
    if (result != 0) return false;

    if (out.structSize != sizeof(AcStatusResult)) return false;

    cachedState_ = out.state;
    return true;
}

bool ExternalAcAdapter::GetHeartbeat(AcHeartbeat& out) {
    if (!started_ || !fnBeat_) return false;

    memset(&out, 0, sizeof(out));
    out.structSize = sizeof(AcHeartbeat);

    int32_t result = fnBeat_(&out);
    if (result != 0) return false;

    if (out.structSize != sizeof(AcHeartbeat)) return false;

    return true;
}

void ExternalAcAdapter::Stop() noexcept {
    if (started_ && fnStop_) {
        fnStop_();
    }
    started_ = false;
    cachedState_ = AcState::Unknown;
}

} // namespace skr::ac
