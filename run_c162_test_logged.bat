@echo off
cd /d "C:\Users\Staark\Desktop\SKR Platform"

echo ================================================================ > c162_test_output.log
echo  C1.6.3 Build and Test Run >> c162_test_output.log
echo  %DATE% %TIME% >> c162_test_output.log
echo ================================================================ >> c162_test_output.log

echo. >> c162_test_output.log
echo [STEP 1] Building SKRC162Harness (Release)... >> c162_test_output.log
cmake --build build-external-lobby --config Release --target SKRC162Harness >> c162_test_output.log 2>&1
echo BUILD EXIT CODE: %ERRORLEVEL% >> c162_test_output.log

echo. >> c162_test_output.log
echo [STEP 2] Locating executable... >> c162_test_output.log
set "EXE="
if exist "build-external-lobby\Release\SKRC162Harness.exe" set "EXE=build-external-lobby\Release\SKRC162Harness.exe"
if not defined EXE if exist "build-external-lobby\Debug\SKRC162Harness.exe" set "EXE=build-external-lobby\Debug\SKRC162Harness.exe"
if not defined EXE (
    echo ERROR: SKRC162Harness.exe not found >> c162_test_output.log
    dir /s /b build-external-lobby\SKRC162Harness.exe >> c162_test_output.log 2>&1
    exit /b 1
)
echo Found: %EXE% >> c162_test_output.log

echo. >> c162_test_output.log
echo [STEP 3] Running harness (no canonical file)... >> c162_test_output.log
"%EXE%" >> c162_test_output.log 2>&1
echo HARNESS EXIT CODE: %ERRORLEVEL% >> c162_test_output.log

echo. >> c162_test_output.log
echo [STEP 4] Searching for GTA SA executable... >> c162_test_output.log
set "GTA_EXE="
for %%p in (
    "C:\Program Files (x86)\Rockstar Games\GTA San Andreas\gta_sa.exe"
    "C:\Program Files\Rockstar Games\GTA San Andreas\gta_sa.exe"
    "D:\Games\GTA San Andreas\gta_sa.exe"
    "D:\GTA San Andreas\gta_sa.exe"
    "C:\Games\GTA San Andreas\gta_sa.exe"
    "C:\GTA San Andreas\gta_sa.exe"
    "E:\Games\GTA San Andreas\gta_sa.exe"
    "E:\GTA San Andreas\gta_sa.exe"
) do (
    if exist %%p (
        set "GTA_EXE=%%~p"
        echo Found GTA at: %%~p >> c162_test_output.log
    )
)
if not defined GTA_EXE (
    echo GTA SA not found in standard locations >> c162_test_output.log
)

echo. >> c162_test_output.log
echo [STEP 5] Runtime dependencies... >> c162_test_output.log
echo SKR.Client.dll: >> c162_test_output.log
if exist "build-external-lobby\Release\SKR.Client.dll" (echo   FOUND >> c162_test_output.log) else (echo   NOT FOUND >> c162_test_output.log)
echo samp.dll: >> c162_test_output.log
if exist "build-external-lobby\Release\samp\samp.dll" (echo   FOUND in Release\samp\ >> c162_test_output.log) else (echo   NOT FOUND in Release\samp\ >> c162_test_output.log)
echo CEF: >> c162_test_output.log
if exist "build-external-lobby\Release\SKR\cef" (echo   FOUND at Release\SKR\cef >> c162_test_output.log) else (echo   NOT FOUND at Release\SKR\cef >> c162_test_output.log)
if exist "build-external-lobby\Release\cef" (echo   FOUND at Release\cef >> c162_test_output.log) else (echo   NOT FOUND at Release\cef >> c162_test_output.log)

echo. >> c162_test_output.log
echo [STEP 6] Preflight log... >> c162_test_output.log
if exist "%APPDATA%\SKR\preflight.log" (
    type "%APPDATA%\SKR\preflight.log" >> c162_test_output.log 2>&1
) else (
    echo No preflight.log found >> c162_test_output.log
)

echo. >> c162_test_output.log
if defined GTA_EXE (
    echo [STEP 7] SHA-256 of %GTA_EXE%... >> c162_test_output.log
    certutil -hashfile "%GTA_EXE%" SHA256 >> c162_test_output.log 2>&1

    echo. >> c162_test_output.log
    echo [STEP 8] Running harness WITH canonical file... >> c162_test_output.log
    "%EXE%" "%GTA_EXE%" >> c162_test_output.log 2>&1
    echo HARNESS+FILE EXIT CODE: %ERRORLEVEL% >> c162_test_output.log
)

echo. >> c162_test_output.log
echo [STEP 9] Build dir listing... >> c162_test_output.log
dir "build-external-lobby\Release\*.exe" >> c162_test_output.log 2>&1
dir "build-external-lobby\Release\*.dll" >> c162_test_output.log 2>&1

echo. >> c162_test_output.log
echo ================================================================ >> c162_test_output.log
echo  DONE >> c162_test_output.log
echo ================================================================ >> c162_test_output.log
