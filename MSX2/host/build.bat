@echo off
rem Build the PC test harness (Visual Studio 2026)
rem   build.bat <output folder>
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%~dp0build"
if not exist "%OUT%" mkdir "%OUT%"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
set "SRC=%~dp0.."
set "INC=/I"%~dp0." /I"%SRC%""
cl /nologo /c /O2 /W1 /utf-8 /DDEBUG %INC% /Fo"%OUT%/" /TC "%SRC%\Z80.c" || exit /b 1
cl /nologo /O2 /W3 /wd5033 /utf-8 /EHsc /std:c++17 %INC% /Fo"%OUT%/" /Fe"%OUT%\msx2host.exe" "%SRC%\msx2.cpp" "%SRC%\v9958.cpp" "%SRC%\wd2793.cpp" "%~dp0main.cpp" "%OUT%\Z80.obj" /link /nologo || exit /b 1
endlocal
