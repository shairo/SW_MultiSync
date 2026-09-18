@echo off
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars64 failed & exit /b 1)

set ROOT=%~dp0
set OUT=%ROOT%build
if not exist "%OUT%" mkdir "%OUT%"

echo === swhook.dll ===
cl /nologo /std:c++17 /O2 /LD /EHsc /W3 ^
   /Fe:"%OUT%\swhook.dll" /Fo:"%OUT%\\" ^
   "%ROOT%src\hook\dllmain.cpp" ^
   /link kernel32.lib user32.lib ws2_32.lib
if errorlevel 1 exit /b 1

echo === inject.exe ===
cl /nologo /std:c++17 /O2 /EHsc /W3 ^
   /Fe:"%OUT%\inject.exe" /Fo:"%OUT%\\" ^
   "%ROOT%src\inject\inject.cpp" ^
   /link kernel32.lib
if errorlevel 1 exit /b 1

echo === swctl.exe ===
cl /nologo /std:c++17 /O2 /EHsc /W3 ^
   /Fe:"%OUT%\swctl.exe" /Fo:"%OUT%\\" ^
   "%ROOT%src\swctl\swctl.cpp" ^
   /link kernel32.lib advapi32.lib ws2_32.lib
if errorlevel 1 exit /b 1

echo === SWMultiSync.exe (GUI) ===
cl /nologo /std:c++17 /O2 /EHsc /W3 /utf-8 ^
   /Fe:"%OUT%\SWMultiSync.exe" /Fo:"%OUT%\\" ^
   "%ROOT%src\gui\gui.cpp" ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
   kernel32.lib user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ws2_32.lib
if errorlevel 1 exit /b 1

copy /y "%OUT%\swhook.dll" "%OUT%\..\swhook.dll" >nul
echo === done: %OUT% ===
