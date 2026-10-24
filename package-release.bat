@echo off
setlocal enabledelayedexpansion
echo ========================================
echo  SKR Enhanced Client 1.0.0 — Release Packaging
echo ========================================
echo Start: %date% %time%
echo.

REM ── Configuration ──────────────────────────────────────────────────
set "SRC=%~dp0"
set "BUILD=%SRC%build-external-lobby\Release"
set "CEF_BIN=%SRC%deps\omp-cef\deps\cef\cef_binary_148.0.10+g7ee53f5+chromium-148.0.7778.218_windows32"
set "OMP_CEF_BUILD=%SRC%deps\omp-cef\build\Release"
set "LOBBY_ASSETS=%SRC%out\lobby-test\SKR"
set "DEST=%USERPROFILE%\Desktop\SKR Enhanced — Release Packaging\SKR Enhanced Client 1.0.0"
set "ERRORS=0"

REM ── Verify source artifacts exist ──────────────────────────────────
echo === Verifying source artifacts ===

if not exist "%BUILD%\SKRLauncher.exe" (
    echo ERROR: SKRLauncher.exe not found at %BUILD%
    echo        Run build-release.bat first.
    set /a ERRORS+=1
)
if not exist "%BUILD%\SKR.Client.dll" (
    echo ERROR: SKR.Client.dll not found at %BUILD%
    echo        Run build-release.bat first.
    set /a ERRORS+=1
)

REM omp-cef client.dll — check multiple possible locations
set "CLIENT_DLL="
if exist "%OMP_CEF_BUILD%\client.dll" (
    set "CLIENT_DLL=%OMP_CEF_BUILD%\client.dll"
) else if exist "%SRC%out\lobby-test\cef\client.dll" (
    set "CLIENT_DLL=%SRC%out\lobby-test\cef\client.dll"
) else (
    echo ERROR: omp-cef client.dll not found.
    echo        Build omp-cef or copy client.dll to out\lobby-test\cef\
    set /a ERRORS+=1
)

REM omp-cef renderer.exe
set "RENDERER_EXE="
if exist "%OMP_CEF_BUILD%\renderer.exe" (
    set "RENDERER_EXE=%OMP_CEF_BUILD%\renderer.exe"
) else if exist "%SRC%out\lobby-test\cef\renderer.exe" (
    set "RENDERER_EXE=%SRC%out\lobby-test\cef\renderer.exe"
) else (
    echo ERROR: omp-cef renderer.exe not found.
    echo        Build omp-cef or copy renderer.exe to out\lobby-test\cef\
    set /a ERRORS+=1
)

REM SA-MP R5 samp.dll — check bundled location or known paths
set "SAMP_DLL="
if exist "%SRC%samp\samp.dll" (
    set "SAMP_DLL=%SRC%samp\samp.dll"
) else if exist "%SRC%out\samp\samp.dll" (
    set "SAMP_DLL=%SRC%out\samp\samp.dll"
) else (
    echo ERROR: samp.dll not found.
    echo        Place the pinned SA-MP R5 samp.dll at %SRC%samp\samp.dll
    set /a ERRORS+=1
)

if not exist "%CEF_BIN%\Release\libcef.dll" (
    echo ERROR: CEF binaries not found at %CEF_BIN%\Release
    set /a ERRORS+=1
)
if not exist "%CEF_BIN%\Resources\icudtl.dat" (
    echo ERROR: CEF resources not found at %CEF_BIN%\Resources
    set /a ERRORS+=1
)
if not exist "%LOBBY_ASSETS%\lobby\lobby.html" (
    echo ERROR: Lobby assets not found at %LOBBY_ASSETS%
    set /a ERRORS+=1
)

if !ERRORS! GTR 0 (
    echo.
    echo %ERRORS% source artifact(s) missing. Fix the above errors before packaging.
    goto :error
)
echo All source artifacts found.
echo.

REM ── Clean and create destination ───────────────────────────────────
echo === Creating release directory ===
if exist "%DEST%" (
    echo Cleaning existing release directory...
    rmdir /s /q "%DEST%"
)
mkdir "%DEST%"
mkdir "%DEST%\samp"
mkdir "%DEST%\cef"
mkdir "%DEST%\cef\locales"
mkdir "%DEST%\SKR"
mkdir "%DEST%\SKR\lobby"
mkdir "%DEST%\SKR\lobby\assets"
mkdir "%DEST%\LICENSES"
echo.

REM ── Copy core binaries ─────────────────────────────────────────────
echo === Copying core binaries ===
copy /y "%BUILD%\SKRLauncher.exe" "%DEST%\" >nul || (echo FAILED: SKRLauncher.exe & set /a ERRORS+=1)
copy /y "%BUILD%\SKR.Client.dll" "%DEST%\" >nul || (echo FAILED: SKR.Client.dll & set /a ERRORS+=1)
echo   SKRLauncher.exe, SKR.Client.dll

REM ── Copy SA-MP runtime ─────────────────────────────────────────────
echo === Copying SA-MP runtime ===
copy /y "%SAMP_DLL%" "%DEST%\samp\samp.dll" >nul || (echo FAILED: samp.dll & set /a ERRORS+=1)
echo   samp\samp.dll

REM ── Copy CEF runtime ───────────────────────────────────────────────
echo === Copying CEF runtime ===
copy /y "%CLIENT_DLL%" "%DEST%\cef\" >nul || (echo FAILED: client.dll & set /a ERRORS+=1)
copy /y "%RENDERER_EXE%" "%DEST%\cef\" >nul || (echo FAILED: renderer.exe & set /a ERRORS+=1)

REM CEF core binaries (mandatory)
for %%F in (libcef.dll chrome_elf.dll) do (
    copy /y "%CEF_BIN%\Release\%%F" "%DEST%\cef\" >nul || (echo FAILED: %%F & set /a ERRORS+=1)
)
REM CEF support DLLs (required for rendering)
for %%F in (d3dcompiler_47.dll libEGL.dll libGLESv2.dll v8_context_snapshot.bin) do (
    if exist "%CEF_BIN%\Release\%%F" (
        copy /y "%CEF_BIN%\Release\%%F" "%DEST%\cef\" >nul || (echo FAILED: %%F & set /a ERRORS+=1)
    )
)
REM CEF resources (mandatory)
for %%F in (icudtl.dat resources.pak chrome_100_percent.pak chrome_200_percent.pak) do (
    if exist "%CEF_BIN%\Resources\%%F" (
        copy /y "%CEF_BIN%\Resources\%%F" "%DEST%\cef\" >nul || (echo FAILED: %%F & set /a ERRORS+=1)
    )
)
REM CEF locale — en-US only (per task spec: ship only .pak/.manifest pairs)
copy /y "%CEF_BIN%\Resources\locales\en-US.pak" "%DEST%\cef\locales\" >nul || (echo FAILED: en-US.pak & set /a ERRORS+=1)
echo   cef\ (client.dll, renderer.exe, libcef.dll, resources, locales)

REM ── Copy SKR client resources ──────────────────────────────────────
echo === Copying SKR client resources ===
xcopy /y /e /q "%LOBBY_ASSETS%\*" "%DEST%\SKR\" >nul || (echo FAILED: lobby assets & set /a ERRORS+=1)
echo   SKR\lobby\ (HTML, CSS, JS, assets)

echo.
echo === Packaging complete ===

if !ERRORS! GTR 0 (
    echo WARNING: %ERRORS% file(s) failed to copy.
    goto :error
)

echo All files copied successfully.
echo.
echo Release directory: %DEST%
echo.
echo === File listing ===
dir /s /b "%DEST%"
echo.

REM ── Generate release manifest ──────────────────────────────────────
echo === Generating release manifest ===
echo Manifest generation requires validate-release.py (run separately).
echo.
echo End: %date% %time%
echo PACKAGE_OK > "%DEST%\PACKAGE_STATUS.txt"
goto :done

:error
echo.
echo === PACKAGING FAILED ===
echo End: %date% %time%

:done
echo.
echo Press any key to close...
pause >nul
