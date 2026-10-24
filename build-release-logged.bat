@echo off
REM Redirects all output to build-external-lobby\release-build.log
call "%~dp0build-release.bat" > "%~dp0build-external-lobby\release-build.log" 2>&1
