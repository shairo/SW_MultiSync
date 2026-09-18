@echo off
rem inject.bat [--build] [--wait] [pid]
rem   Dev helper: inject build\swhook.dll (copied to repo root by build.bat) into server64.exe.
rem   --build : run build.bat first
rem   --wait  : poll until server64.exe appears, then inject
rem   pid     : target this pid instead of looking up server64.exe
setlocal
cd /d "%~dp0"

set BUILD=0
set WAIT=0
set TARGET=server64.exe
:args
if "%~1"=="" goto :argsdone
if /I "%~1"=="--build" set BUILD=1
if /I "%~1"=="--wait"  set WAIT=1
if /I "%~1"=="-b"      set BUILD=1
if /I "%~1"=="-w"      set WAIT=1
echo %~1| findstr /R "^[0-9][0-9]*$" >nul && set TARGET=%~1
shift
goto :args
:argsdone

rem --- elevate (OpenProcess PROCESS_ALL_ACCESS needs admin) ---
net session >nul 2>&1
if errorlevel 1 (
    echo elevating...
    powershell -Command "Start-Process -Verb RunAs -FilePath cmd -ArgumentList '/k','\"%~f0\" %*'"
    exit /b
)

if %BUILD%==1 (
    call "%~dp0build.bat" || (echo build failed & goto :err)
)

if not exist "%~dp0build\inject.exe" (echo build\inject.exe not found - run build.bat & goto :err)
if not exist "%~dp0swhook.dll"      (echo swhook.dll not found at repo root - run build.bat & goto :err)

if %WAIT%==1 (
    echo waiting for server64.exe...
    :waitloop
    tasklist /FI "IMAGENAME eq server64.exe" | find /I "server64.exe" >nul || (
        timeout /t 1 /nobreak >nul
        goto :waitloop
    )
)

rem swhook.dll at repo root => swhook.ini / captures/ / .log all land in the repo root.
"%~dp0build\inject.exe" %TARGET% "%~dp0swhook.dll"
if errorlevel 1 goto :err

echo.
echo injected. control: swctl.exe / GUI over 127.0.0.1:ipcPort (see swhook.ini)
echo log/captures: %~dp0
exit /b 0

:err
echo.
echo *** failed ***
pause
exit /b 1
