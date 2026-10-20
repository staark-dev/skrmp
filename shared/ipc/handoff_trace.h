#pragma once
#include <windows.h>
#include <cstdio>
#include <string>
namespace skr::ipc {
inline void TraceHandoff(const char* phase) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    std::wstring folder(path);
    folder.resize(folder.find_last_of(L'\\') + 1);
    folder += L"SKR";
    CreateDirectoryW(folder.c_str(), nullptr);
    FILE* file = nullptr;
    _wfopen_s(&file, (folder + L"\\handoff.log").c_str(), L"a");
    if (!file) return;
    fprintf(file, "[%llu] pid=%lu %s\n", GetTickCount64(), GetCurrentProcessId(), phase);
    fclose(file);
}
}
