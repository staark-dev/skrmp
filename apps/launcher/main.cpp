#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    MessageBoxW(nullptr,
                L"SKR Launcher: development skeleton.\n"
                L"No authentication or game launch implemented yet.",
                L"SKR Client", MB_OK | MB_ICONINFORMATION);
    return 0;
}
