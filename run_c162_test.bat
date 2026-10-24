@echo off
setlocal EnableDelayedExpansion

echo ================================================================
echo  C1.6.3 — Build and Run SKRC162Harness
echo  %DATE% %TIME%
echo ================================================================

cd /d "C:\Users\Staark\Desktop\SKR Platform"

echo.
echo [STEP 1] Building SKRC162Harness (Release)...
echo.

cmake --build build-external-lobby --config Release --target SKRC162Harness 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo BUILD FAILED with error code %ERRORLEVEL%
    echo Trying to reconfigure first...
    cmake -S . -B build-external-lobby 2>&1
    cmake --build build-external-lobby --config Release --target SKRC162Harness 2>&1
    if !ERRORLEVEL! NEQ 0 (
        echo REBUILD ALSO FAILED
        goto :end
    )
)

echo.
echo [STEP 2] Checking for built executable...
echo.

set "EXE=build-external-lobby\Release\SKRC162Harness.exe"
if not exist "%EXE%" (
    set "EXE=build-external-lobby\Debug\SKRC162Harness.exe"
)
if not exist "%EXE%" (
    echo ERROR: Cannot find SKRC162Harness.exe in build output
    dir /s /b build-external-lobby\SKRC162Harness.exe 2>nul
    goto :end
)

echo Found: %EXE%

echo.
echo [STEP 3] Running test harness (no canonical file)...
echo.

"%EXE%" 2>&1

echo.
echo EXIT CODE: %ERRORLEVEL%

echo.
echo [STEP 4] Checking for canonical Hoodlum executable...
echo.

REM Check common GTA SA locations
set "GTA_EXE="
for %%p in (
    "C:\Program Files (x86)\Rockstar Games\GTA San Andreas\gta_sa.exe"
    "C:\Program Files\Rockstar Games\GTA San Andreas\gta_sa.exe"
    "D:\Games\GTA San Andreas\gta_sa.exe"
    "D:\GTA San Andreas\gta_sa.exe"
    "C:\Games\GTA San Andreas\gta_sa.exe"
    "C:\GTA San Andreas\gta_sa.exe"
) do (
    if exist %%p (
        set "GTA_EXE=%%~p"
        echo Found GTA at: %%p
    )
)

REM Also search the preflight log for GTA path
if exist "%APPDATA%\SKR\preflight.log" (
    echo.
    echo [INFO] Preflight log found at %APPDATA%\SKR\preflight.log
    type "%APPDATA%\SKR\preflight.log" 2>nul
)

REM Check launcher directory for runtime files
echo.
echo [STEP 5] Checking runtime dependencies...
echo.
echo Launcher dir contents:
dir "C:\Users\Staark\Desktop\SKR Platform\build-external-lobby\Release\*.dll" 2>nul
dir "C:\Users\Staark\Desktop\SKR Platform\build-external-lobby\Release\*.exe" 2>nul
echo.
echo Checking SKR.Client.dll:
if exist "build-external-lobby\Release\SKR.Client.dll" (echo   FOUND) else (echo   NOT FOUND)
echo Checking samp subdirectory:
if exist "build-external-lobby\Release\samp\samp.dll" (echo   samp.dll FOUND) else (echo   samp.dll NOT FOUND in Release\samp\)
echo Checking CEF:
if exist "build-external-lobby\Release\SKR\cef" (echo   CEF dir FOUND) else (echo   CEF dir NOT FOUND in Release\SKR\cef)
if exist "build-external-lobby\Release\cef" (echo   CEF dir FOUND at Release\cef) else (echo   CEF dir NOT FOUND at Release\cef)

echo.
echo [STEP 6] SHA-256 of canonical executable (if found)...
echo.
if defined GTA_EXE (
    echo Hashing: %GTA_EXE%
    certutil -hashfile "%GTA_EXE%" SHA256 2>&1

    echo.
    echo [STEP 7] Running test harness WITH canonical file...
    echo.
    "%EXE%" "%GTA_EXE%" 2>&1
    echo.
    echo EXIT CODE: %ERRORLEVEL%
) else (
    echo No GTA SA executable found in standard locations.
    echo To run T14 manually: SKRC162Harness.exe "path\to\gta_sa.exe"
)

:end
echo.
echo ================================================================
echo  C1.6.3 test run complete.
echo ================================================================
pause
