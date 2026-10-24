@echo off
REM ═══════════════════════════════════════════════════════════════════
REM  SKR Enhanced — TEST_ONLY Certification Build
REM  CODEX C1.6.3: Experimental Hoodlum runtime certification
REM
REM  THIS BUILD IS FOR CONTROLLED TESTING ONLY.
REM  DO NOT DISTRIBUTE OR USE IN PRODUCTION.
REM ═══════════════════════════════════════════════════════════════════

setlocal EnableDelayedExpansion

echo.
echo ══════════════════════════════════════════════════════════
echo  SKR Enhanced — TEST_ONLY Certification Build
echo  WARNING: This is an experimental build.
echo  DO NOT distribute or use in production.
echo ══════════════════════════════════════════════════════════
echo.

REM --- Step 1: Configure with TEST_ONLY flags ---
echo [1/3] Configuring CMake with SKR_TEST_CERTIFICATION=ON...

if not exist "build-test-certification" mkdir build-test-certification

cmake -S . -B build-test-certification ^
  -G "Visual Studio 18 2026" ^
  -DSKR_EXTERNAL_LOBBY=ON ^
  -DSKR_TEST_CERTIFICATION=ON

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] CMake configuration failed.
    echo Check that Visual Studio 2026 and CMake are installed.
    pause
    exit /b 1
)

echo.
echo [2/3] Building Release configuration...

cmake --build build-test-certification --config Release

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Build failed. Check compiler output above.
    pause
    exit /b 1
)

echo.
echo [3/3] Build complete.
echo.
echo ══════════════════════════════════════════════════════════
echo  TEST_ONLY build output:
echo    build-test-certification\Release\SKRLauncher.exe
echo.
echo  To launch the test:
echo    run_test_certification.bat
echo ══════════════════════════════════════════════════════════
echo.

pause
