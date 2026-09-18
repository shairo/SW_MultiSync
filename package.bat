@echo off
rem package.bat - build everything and produce the portable distribution zip:
rem   dist\SW_SyncTool_v<version>\   (staging folder = contents of the zip)
rem   dist\SW_SyncTool_v<version>.zip
rem The version comes from src\common\version.h (SWHOOK_VERSION). Ships: GUI, CLI, DLL, ini, README.
rem (ASCII only: cmd reads .bat files in the OEM codepage, non-ASCII bytes break parsing.)
setlocal
cd /d "%~dp0"

call "%~dp0build.bat"
if errorlevel 1 (echo build failed & exit /b 1)

for /f "tokens=3 delims= " %%v in ('findstr /C:"#define SWHOOK_VERSION " src\common\version.h') do set VER=%%~v
if "%VER%"=="" (echo could not read version & exit /b 1)

set NAME=SW_SyncTool_v%VER%
set STAGE=dist\%NAME%
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"

copy /y build\SWSyncTool.exe "%STAGE%\" >nul
copy /y build\swctl.exe      "%STAGE%\" >nul
copy /y build\swhook.dll     "%STAGE%\" >nul
copy /y packaging\README_*.txt "%STAGE%\" >nul
copy /y ipc-protocol.md      "%STAGE%\" >nul
rem Distribution ini = the repo ini with relay auto-ON (hands-off for end users). Dev keeps relay=0.
powershell -NoProfile -Command "(Get-Content -Raw swhook.ini) -replace '(?m)^relay\s*=\s*\d+', 'relay    = 1' | Set-Content -NoNewline -Encoding UTF8 '%STAGE%\swhook.ini'"
if errorlevel 1 (echo ini failed & exit /b 1)

if exist "dist\%NAME%.zip" del "dist\%NAME%.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath 'dist\%NAME%.zip' -CompressionLevel Optimal"
if errorlevel 1 (echo zip failed & exit /b 1)

echo === packaged: dist\%NAME%.zip ===
dir /b "%STAGE%"
