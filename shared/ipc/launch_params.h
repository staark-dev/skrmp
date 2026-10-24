#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace skr::ipc {

// Pipe name includes PID to avoid collisions between multiple instances.
// Launcher creates: \\.\pipe\SKR_<child_pid>
// DLL reads from the same pipe using its own GetCurrentProcessId().
inline void MakePipeName(wchar_t* buf, size_t bufChars, DWORD pid) {
    _snwprintf_s(buf, bufChars, _TRUNCATE, L"\\\\.\\pipe\\SKR_%lu", pid);
}

static constexpr uint32_t kLaunchParamsMagic   = 0x534B5231; // "SKR1"
// 5: gameplayConfirmedHandle. 6: sessionId + milestone handles; ack echoes sessionId.
static constexpr uint32_t kLaunchParamsVersion = 6;
static constexpr size_t   kMaxAddress          = 256;
static constexpr size_t   kMaxPlayerName       = 64;
static constexpr size_t   kJoinTicketSize      = 32;

// Fixed-size struct written by launcher, read by DLL.
// All char fields are UTF-8, null-terminated.
struct LaunchParams {
    uint32_t magic;
    uint32_t version;
    char     serverAddress[kMaxAddress];
    uint16_t serverPort;
    char     playerName[kMaxPlayerName];
    uint8_t  joinTicket[kJoinTicketSize];
    uint8_t  hasJoinTicket;  // 1 if joinTicket is populated
    uint8_t  enhancedAdvertised; // skr_client=2; discovery hint, never authentication
    uint8_t  reserved[2];
    uint32_t frameReadyHandle = 0;
    uint32_t takeoverHandle = 0;
    uint32_t sessionReadyHandle = 0;
    uint32_t gameplayConfirmedHandle = 0;  // client: in world, healthy (watchdog disarm)
    uint32_t clientInitializedHandle = 0;  // launch_milestones.h CLIENT_INITIALIZED
    uint32_t sampLoadedHandle = 0;         // SAMP_LOADED
    uint32_t sampReadyHandle = 0;          // SAMP_READY
    uint32_t reservedHandle = 0;
    uint64_t sessionId = 0;                // launcher-chosen nonce, echoed in the ack
};

static_assert(sizeof(LaunchParams) < 4096, "LaunchParams must fit in a single pipe buffer");

// v2 transport: bounded overlapped I/O and an explicit receipt acknowledgement.
// See Microsoft Named Pipe Server Using Overlapped I/O and CancelIoEx docs.
namespace detail {
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
inline DWORD Remaining(ULONGLONG deadline) {
    const auto now = GetTickCount64();
    return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
}
inline bool Complete(HANDLE pipe, OVERLAPPED& op, BOOL immediate,
                     ULONGLONG deadline, DWORD& bytes) {
    if (immediate) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(op.hEvent, Remaining(deadline)) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &op);
        // The OVERLAPPED and its buffer must remain alive until cancellation completes.
        GetOverlappedResult(pipe, &op, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &op, &bytes, FALSE) != FALSE;
}
inline bool Transfer(HANDLE pipe, void* buffer, DWORD size, bool write, ULONGLONG deadline) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.value || !Remaining(deadline)) return false;
    OVERLAPPED op{}; op.hEvent = event.value;
    DWORD bytes = 0;
    BOOL immediate = write ? WriteFile(pipe, buffer, size, &bytes, &op)
                           : ReadFile(pipe, buffer, size, &bytes, &op);
    return Complete(pipe, op, immediate, deadline, bytes) && bytes == size;
}
inline bool Valid(const LaunchParams& p) {
    return p.magic == kLaunchParamsMagic && p.version == kLaunchParamsVersion &&
        p.hasJoinTicket <= 1 && p.enhancedAdvertised <= 1 && memchr(p.serverAddress, 0, sizeof(p.serverAddress)) &&
        memchr(p.playerName, 0, sizeof(p.playerName));
}
} // namespace detail

inline bool WriteLaunchParams(DWORD childPid, const LaunchParams& params, DWORD timeoutMs = 5000) {
    if (!detail::Valid(params)) return false;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    wchar_t name[128]; MakePipeName(name, 128, childPid);
    detail::Handle pipe(CreateNamedPipeW(name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, sizeof(LaunchParams), 1, timeoutMs, nullptr));
    if (pipe.value == INVALID_HANDLE_VALUE) return false;
    detail::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.value) return false;
    OVERLAPPED op{}; op.hEvent = event.value;
    DWORD bytes = 0;
    BOOL connected = ConnectNamedPipe(pipe.value, &op);
    if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) connected = TRUE;
    if (!detail::Complete(pipe.value, op, connected, deadline, bytes)) return false;
    ULONG peer = 0;
    if (!GetNamedPipeClientProcessId(pipe.value, &peer) || peer != childPid) return false;
    auto copy = params;
    bool sent = detail::Transfer(pipe.value, &copy, sizeof(copy), true, deadline);
    SecureZeroMemory(&copy, sizeof(copy));
    if (!sent) return false;
    // The reader echoes the session id it accepted: both peers agree on the launch.
    uint64_t ack = ~params.sessionId;
    return detail::Transfer(pipe.value, &ack, sizeof(ack), false, deadline) && ack == params.sessionId;
}

inline bool ReadLaunchParams(LaunchParams& out, DWORD timeoutMs = 5000) {
    SecureZeroMemory(&out, sizeof(out));
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    wchar_t name[128]; MakePipeName(name, 128, GetCurrentProcessId());
    HANDLE opened = INVALID_HANDLE_VALUE;
    while (detail::Remaining(deadline)) {
        opened = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, nullptr);
        if (opened != INVALID_HANDLE_VALUE) break;
        DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return false;
        Sleep((std::min)(DWORD(10), detail::Remaining(deadline)));
    }
    detail::Handle pipe(opened);
    if (opened == INVALID_HANDLE_VALUE) return false;
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(opened, &mode, nullptr, nullptr)) return false;
    LaunchParams received{};
    bool valid = detail::Transfer(opened, &received, sizeof(received), false, deadline) && detail::Valid(received);
    uint64_t ack = valid ? received.sessionId : ~received.sessionId;
    bool acknowledged = detail::Transfer(opened, &ack, sizeof(ack), true, deadline);
    if (valid && acknowledged) out = received;
    SecureZeroMemory(&received, sizeof(received));
    return valid && acknowledged;
}

} // namespace skr::ipc
