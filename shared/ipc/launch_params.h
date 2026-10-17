#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace skr::ipc {

// Pipe name includes PID to avoid collisions between multiple instances.
// Launcher creates: \\.\pipe\SKR_<child_pid>
// DLL reads from the same pipe using its own GetCurrentProcessId().
inline void MakePipeName(wchar_t* buf, size_t bufChars, DWORD pid) {
    _snwprintf_s(buf, bufChars, _TRUNCATE, L"\\\\.\\pipe\\SKR_%lu", pid);
}

static constexpr uint32_t kLaunchParamsMagic   = 0x534B5231; // "SKR1"
static constexpr uint32_t kLaunchParamsVersion = 1;
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
    uint8_t  reserved[15];
};

static_assert(sizeof(LaunchParams) < 4096, "LaunchParams must fit in a single pipe buffer");

// --- Writer (launcher side) ---

// Creates named pipe, writes params, waits for client to connect and read.
// Must be called after DLL injection succeeds, before ResumeThread.
// Returns true if the client read the data within timeoutMs.
inline bool WriteLaunchParams(DWORD childPid, const LaunchParams& params, DWORD timeoutMs = 5000) {
    wchar_t pipeName[128];
    MakePipeName(pipeName, 128, childPid);

    HANDLE hPipe = CreateNamedPipeW(
        pipeName,
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,                          // single instance
        sizeof(LaunchParams),       // out buffer
        0,                          // in buffer (not used)
        timeoutMs,
        nullptr
    );
    if (hPipe == INVALID_HANDLE_VALUE) return false;

    // Wait for the DLL to connect
    BOOL connected = ConnectNamedPipe(hPipe, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(hPipe);
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(hPipe, &params, sizeof(params), &written, nullptr);
    FlushFileBuffers(hPipe);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);

    return ok && written == sizeof(params);
}

// --- Reader (DLL side) ---

// Connects to the pipe the launcher created, reads LaunchParams.
// Called early in SkrInitialize before any network activity.
inline bool ReadLaunchParams(LaunchParams& out, DWORD timeoutMs = 5000) {
    wchar_t pipeName[128];
    MakePipeName(pipeName, 128, GetCurrentProcessId());

    // The pipe may not exist yet if the launcher thread hasn't reached CreateNamedPipe.
    // Retry with short sleeps.
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    DWORD deadline = GetTickCount() + timeoutMs;
    while (GetTickCount() < deadline) {
        hPipe = CreateFileW(
            pipeName,
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (hPipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) break;
        Sleep(10);
    }
    if (hPipe == INVALID_HANDLE_VALUE) return false;

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hPipe, &out, sizeof(out), &bytesRead, nullptr);
    CloseHandle(hPipe);

    if (!ok || bytesRead != sizeof(out)) return false;
    if (out.magic != kLaunchParamsMagic || out.version != kLaunchParamsVersion) return false;

    return true;
}

} // namespace skr::ipc
