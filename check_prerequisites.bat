@echo off
REM ═══════════════════════════════════════════════════════════════════
REM  SKR Enhanced — C1.6.3 Runtime Prerequisites Check
REM ═══════════════════════════════════════════════════════════════════

setlocal EnableDelayedExpansion

echo.
echo ══════════════════════════════════════════════════════════
echo  SKR Enhanced — Runtime Prerequisites Check
echo ══════════════════════════════════════════════════════════
echo.

set PASS=0
set WARN=0
set FAIL=0

REM --- 1. Canonical Hoodlum executable ---
set GTA_EXE=C:\Users\Staark\Desktop\gta_sa compare\gta_sa_hoodlum.exe
echo [1] Hoodlum executable
if exist "%GTA_EXE%" (
    for /f "tokens=1" %%h in ('certutil -hashfile "%GTA_EXE%" SHA256 ^| findstr /v "hash certutil"') do set HASH=%%h
    set EXPECTED=a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26
    if /i "!HASH!"=="!EXPECTED!" (
        echo     PASS — SHA-256 verified
        set /a PASS+=1
    ) else (
        echo     FAIL — SHA-256 mismatch: !HASH!
        set /a FAIL+=1
    )
) else (
    echo     FAIL — Not found at %GTA_EXE%
    set /a FAIL+=1
)

REM --- 2. SKR.Client.dll ---
echo [2] SKR.Client.dll
set FOUND=0
for %%p in ("build-test-certification\Release\SKR.Client.dll" "SKR.Client.dll") do (
    if exist "%%~p" (
        echo     PASS — Found at %%~p
        set /a PASS+=1
        set FOUND=1
    )
)
if !FOUND!==0 (
    echo     FAIL — Not found
    set /a FAIL+=1
)

REM --- 3. samp.dll ---
echo [3] samp.dll
set FOUND=0
for %%p in ("samp\samp.dll" "samp.dll") do (
    if exist "%%~p" (
        echo     PASS — Found at %%~p
        set /a PASS+=1
        set FOUND=1
    )
)
if !FOUND!==0 (
    echo     FAIL — Not found
    set /a FAIL+=1
)

REM --- 4. DirectX ---
echo [4] DirectX (d3d9.dll)
if exist "%SystemRoot%\System32\d3d9.dll" (
    echo     PASS — d3d9.dll present
    set /a PASS+=1
) else (
    echo     FAIL — d3d9.dll missing
    set /a FAIL+=1
)

REM --- 5. VC++ Runtime ---
echo [5] VC++ Runtime
set VC_FOUND=0
for %%d in ("%SystemRoot%\System32\msvcp140.dll" "%SystemRoot%\System32\vcruntime140.dll") do (
    if exist "%%~d" set VC_FOUND=1
)
if !VC_FOUND!==1 (
    echo     PASS — VC++ redistributable present
    set /a PASS+=1
) else (
    echo     WARN — VC++ runtime DLLs not found in System32
    set /a WARN+=1
)

REM --- 6. WebView2 ---
echo [6] WebView2 Runtime
reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BEB-235B8DE4E6A5}" /v pv >nul 2>&1
if %ERRORLEVEL%==0 (
    echo     PASS — WebView2 Runtime installed
    set /a PASS+=1
) else (
    reg query "HKLM\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BEB-235B8DE4E6A5}" /v pv >nul 2>&1
    if !ERRORLEVEL!==0 (
        echo     PASS — WebView2 Runtime installed
        set /a PASS+=1
    ) else (
        echo     WARN — WebView2 Runtime not detected (lobby may not render^)
        set /a WARN+=1
    )
)

REM --- 7. CEF ---
echo [7] CEF components
set CEF_FOUND=0
for %%p in ("cef\libcef.dll" "Release\libcef.dll" "build-test-certification\Release\libcef.dll") do (
    if exist "%%~p" (
        echo     PASS — libcef.dll found at %%~p
        set /a PASS+=1
        set CEF_FOUND=1
    )
)
if !CEF_FOUND!==0 (
    echo     WARN — CEF not found (warning only, not mandatory^)
    set /a WARN+=1
)

REM --- 8. CMake and VS ---
echo [8] Build tools
where cmake >nul 2>&1
if %ERRORLEVEL%==0 (
    echo     PASS — CMake found
    set /a PASS+=1
) else (
    echo     FAIL — CMake not in PATH
    set /a FAIL+=1
)

REM --- Summary ---
echo.
echo ══════════════════════════════════════════════════════════
echo  Results: !PASS! PASS, !WARN! WARN, !FAIL! FAIL
echo ══════════════════════════════════════════════════════════
echo.

if !FAIL! gtr 0 (
    echo  Some prerequisites FAILED. Fix before proceeding.
) else if !WARN! gtr 0 (
    echo  All critical checks passed. Warnings are non-blocking.
) else (
    echo  All prerequisites passed. Ready to build.
)

echo.
pause
