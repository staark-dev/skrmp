@echo off
REM ═══════════════════════════════════════════════════════════════════════
REM SKR Enhanced — Dear ImGui v1.91.6 Setup
REM Downloads and extracts ImGui to deps\imgui\ for native pause menu.
REM Run from the SKR Platform root directory.
REM ═══════════════════════════════════════════════════════════════════════

setlocal

set IMGUI_VERSION=1.91.6
set IMGUI_DIR=deps\imgui
set IMGUI_URL=https://github.com/ocornut/imgui/archive/refs/tags/v%IMGUI_VERSION%.tar.gz
set IMGUI_ARCHIVE=deps\imgui-%IMGUI_VERSION%.tar.gz

if exist "%IMGUI_DIR%\imgui.h" (
    echo [OK] Dear ImGui already present at %IMGUI_DIR%
    goto :eof
)

echo [SETUP] Downloading Dear ImGui v%IMGUI_VERSION%...

REM Try curl first (available on Windows 10+)
where curl >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    curl -sL -o "%IMGUI_ARCHIVE%" "%IMGUI_URL%"
    if %ERRORLEVEL% NEQ 0 (
        echo [ERROR] curl download failed. Please download manually:
        echo   %IMGUI_URL%
        echo   Extract to %IMGUI_DIR%\
        exit /b 1
    )
) else (
    echo [ERROR] curl not found. Please download manually:
    echo   %IMGUI_URL%
    echo   Extract to %IMGUI_DIR%\
    exit /b 1
)

echo [SETUP] Extracting...

REM Create target directory
if not exist "%IMGUI_DIR%" mkdir "%IMGUI_DIR%"

REM Use tar (available on Windows 10 1803+)
tar -xzf "%IMGUI_ARCHIVE%" -C deps\ 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] tar extraction failed. Please extract manually.
    exit /b 1
)

REM Move from imgui-1.91.6\ to imgui\
if exist "deps\imgui-%IMGUI_VERSION%" (
    xcopy /E /Y /Q "deps\imgui-%IMGUI_VERSION%\*" "%IMGUI_DIR%\" >nul
    rmdir /S /Q "deps\imgui-%IMGUI_VERSION%"
)

REM Clean up archive
del "%IMGUI_ARCHIVE%" 2>nul

REM Verify
if exist "%IMGUI_DIR%\imgui.h" (
    echo [OK] Dear ImGui v%IMGUI_VERSION% installed to %IMGUI_DIR%
) else (
    echo [ERROR] Installation failed — imgui.h not found
    exit /b 1
)

echo [OK] Setup complete. You can now build with:
echo   cmake --build build-external-lobby --config Release -j4
