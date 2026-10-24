@echo off
echo Building SKRLauncher with new preflight UI...
"C:\Program Files\CMake\bin\cmake.exe" --build "C:\Users\Staark\Desktop\SKR Platform\build-external-lobby" --target SKRLauncher --config Release 2>&1
echo.
echo Build complete. Press any key to close.
pause >nul
