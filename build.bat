@echo off
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools
rem vcvars64 tries vswhere.exe (not on PATH here) before its fixed-path fallback; hide that noise.
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo vcvars64 failed & exit /b 1)

set ROOT=%~dp0
set OUT=%ROOT%build
if not exist "%OUT%" mkdir "%OUT%"
set RCSRC=%ROOT%res\version.rc

echo === swhook.dll ===
rc /nologo /fo "%OUT%\swhook.res" "/dRC_FILETYPE=VFT_DLL" "/dRC_FILE_DESC=\"SW_MultiSync hook DLL\"" "/dRC_ORIG_NAME=\"swhook.dll\"" "%RCSRC%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /LD /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:"%OUT%\swhook.dll" /Fo:"%OUT%\\" ^
   "%ROOT%src\hook\dllmain.cpp" "%OUT%\swhook.res" ^
   /link kernel32.lib user32.lib ws2_32.lib
if errorlevel 1 exit /b 1

echo === inject.exe ===
rc /nologo /fo "%OUT%\inject.res" "/dRC_FILETYPE=VFT_APP" "/dRC_FILE_DESC=\"SW_MultiSync injector\"" "/dRC_ORIG_NAME=\"inject.exe\"" "%RCSRC%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:"%OUT%\inject.exe" /Fo:"%OUT%\\" ^
   "%ROOT%src\inject\inject.cpp" "%OUT%\inject.res" ^
   /link kernel32.lib
if errorlevel 1 exit /b 1

echo === swctl.exe ===
rc /nologo /fo "%OUT%\swctl.res" "/dRC_FILETYPE=VFT_APP" "/dRC_FILE_DESC=\"SW_MultiSync CLI\"" "/dRC_ORIG_NAME=\"swctl.exe\"" "%RCSRC%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:"%OUT%\swctl.exe" /Fo:"%OUT%\\" ^
   "%ROOT%src\swctl\swctl.cpp" "%OUT%\swctl.res" ^
   /link kernel32.lib advapi32.lib ws2_32.lib
if errorlevel 1 exit /b 1

echo === SWMultiSync.exe (GUI) ===
rc /nologo /dRC_ICON /fo "%OUT%\gui.res" "/dRC_FILETYPE=VFT_APP" "/dRC_FILE_DESC=\"SW_MultiSync\"" "/dRC_ORIG_NAME=\"SWMultiSync.exe\"" "%RCSRC%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:"%OUT%\SWMultiSync.exe" /Fo:"%OUT%\\" ^
   "%ROOT%src\gui\gui.cpp" "%OUT%\gui.res" ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
   kernel32.lib user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib ws2_32.lib
if errorlevel 1 exit /b 1

copy /y "%OUT%\swhook.dll" "%OUT%\..\swhook.dll" >nul
echo === done: %OUT% ===
