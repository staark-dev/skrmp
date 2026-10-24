@echo off
REM ═══════════════════════════════════════════════════════════════════
REM  SKR Enhanced — TEST_ONLY Runtime Certification Launch
REM  CODEX C1.6.3: Controlled GTA SA launch via experimental build
REM
REM  THIS SCRIPT LAUNCHES GTA SAN ANDREAS THROUGH THE TEST_ONLY
REM  SKRLauncher. DO NOT USE IN PRODUCTION.
REM ═══════════════════════════════════════════════════════════════════

setlocal EnableDelayedExpansion

echo.
echo ══════════════════════════════════════════════════════════
echo  SKR Enhanced — TEST_ONLY Runtime Launch
echo  Target: gta_sa_hoodlum.exe (canonical SHA-256)
echo ══════════════════════════════════════════════════════════
echo.

set LAUNCHER=build-test-certification\Release\SKRLauncher.exe
set GTA_EXE=C:\Users\Staark\Desktop\gta_sa compare\gta_sa_hoodlum.exe
set LOG_DIR=%APPDATA%\SKR

REM --- Verify the launcher exists ---
if not exist "%LAUNCHER%" (
    echo [ERROR] TEST_ONLY launcher not found at:
    echo   %LAUNCHER%
    echo.
    echo Run build_test_certification.bat first.
    pause
    exit /b 1
)

REM --- Verify the GTA executable exists ---
if not exist "%GTA_EXE%" (
    echo [ERROR] Canonical Hoodlum executable not found at:
    echo   %GTA_EXE%
    echo.
    echo Expected SHA-256: A559AA772FD136379155EFA71F00C47AAD34BBFEAE6196B0FE1047D0645CBD26
    pause
    exit /b 1
)

REM --- Verify SHA-256 of GTA executable ---
echo [1/4] Verifying Hoodlum executable SHA-256...
for /f "tokens=1" %%h in ('certutil -hashfile "%GTA_EXE%" SHA256 ^| findstr /v "hash certutil"') do (
    set HASH=%%h
)

set EXPECTED=a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26
if /i not "!HASH!"=="!EXPECTED!" (
    echo [ERROR] SHA-256 mismatch!
    echo   Got:      !HASH!
    echo   Expected: !EXPECTED!
    echo.
    echo This is NOT the canonical Hoodlum executable.
    pause
    exit /b 1
)
echo   SHA-256 verified: !HASH!

REM --- Check for SKR.Client.dll ---
echo.
echo [2/4] Checking runtime prerequisites...

set CLIENT_DLL_FOUND=0
if exist "build-test-certification\Release\SKR.Client.dll" (
    echo   SKR.Client.dll: FOUND (build output^)
    set CLIENT_DLL_FOUND=1
)
if exist "%~dp0SKR.Client.dll" (
    echo   SKR.Client.dll: FOUND (project root^)
    set CLIENT_DLL_FOUND=1
)
if !CLIENT_DLL_FOUND!==0 (
    echo   [WARNING] SKR.Client.dll not found — injection will fail.
    echo   Expected at: build-test-certification\Release\SKR.Client.dll
)

REM --- Check for samp.dll ---
set SAMP_FOUND=0
if exist "%~dp0samp\samp.dll" (
    echo   samp.dll: FOUND (samp\^)
    set SAMP_FOUND=1
)
if exist "%~dp0samp.dll" (
    echo   samp.dll: FOUND (root^)
    set SAMP_FOUND=1
)
if !SAMP_FOUND!==0 (
    echo   [WARNING] samp.dll not found — SA-MP integration will fail.
)

REM --- Prepare log directory ---
echo.
echo [3/4] Preparing log directory: %LOG_DIR%
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

REM --- Launch ---
echo.
echo [4/4] Launching TEST_ONLY SKRLauncher...
echo   Launcher: %LAUNCHER%
echo   GTA exe:  %GTA_EXE%
echo.
echo ──────────────────────────────────────────────────────────
echo  The launcher should now open the lobby window.
echo  Press Play to start the GTA launch sequence.
echo  Monitor %LOG_DIR% for runtime logs.
echo ──────────────────────────────────────────────────────────
echo.

start "" "%LAUNCHER%"

echo.
echo Launcher started. Check the window.
echo After testing, collect logs from: %LOG_DIR%
echo   - preflight.log
echo   - crash_trace.log (if crash)
echo   - exit_trace.log
echo.
pause
