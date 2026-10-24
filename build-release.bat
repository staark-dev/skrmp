@echo off
echo === SKR Enhanced Release Build ===
echo Start: %date% %time%

REM Find VS 2022
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found
    goto :error
)
for /f "delims=" %%i in ('"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSDIR=%%i"
if not defined VSDIR (
    echo ERROR: Visual Studio 2022 with C++ tools not found
    goto :error
)
echo Found VS at: %VSDIR%

REM Initialize VS environment
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=amd64 >nul 2>&1

REM Verify cmake
where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake not in PATH after VsDevCmd
    goto :error
)

cd /d "%~dp0"
echo Working directory: %cd%

REM Regenerate CMake to pick up any file changes
echo.
echo === CMake Configure ===
cmake -S . -B build-external-lobby -G "Visual Studio 17 2022" -A Win32 -DSKR_EXTERNAL_LOBBY=ON 2>&1
if errorlevel 1 (
    echo WARNING: CMake configure had issues, trying build anyway...
)

echo.
echo === Building SKRClient (Release) ===
cmake --build build-external-lobby --config Release --target SKRClient 2>&1
if errorlevel 1 (
    echo ERROR: SKRClient build FAILED
    goto :error
)

echo.
echo === Building SKRLauncher (Release) ===
cmake --build build-external-lobby --config Release --target SKRLauncher 2>&1
if errorlevel 1 (
    echo ERROR: SKRLauncher build FAILED
    goto :error
)

echo.
echo === Build Successful ===
echo End: %date% %time%
echo BUILD_OK > build-external-lobby\BUILD_STATUS.txt
goto :done

:error
echo.
echo === BUILD FAILED ===
echo End: %date% %time%
echo BUILD_FAILED > build-external-lobby\BUILD_STATUS.txt

:done
echo.
echo Press any key to close...
pause >nul
