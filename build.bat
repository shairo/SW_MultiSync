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
   /link kernel32.lib user32.lib
if errorlevel 1 exit /b 1

echo === inject.exe ===
cl /nologo /std:c++17 /O2 /EHsc /W3 ^
   /Fe:"%OUT%\inject.exe" /Fo:"%OUT%\\" ^
   "%ROOT%src\inject\inject.cpp" ^
   /link kernel32.lib
if errorlevel 1 exit /b 1

copy /y "%OUT%\swhook.dll" "%OUT%\..\swhook.dll" >nul
echo === done: %OUT% ===
