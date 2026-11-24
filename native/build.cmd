@echo off
rem Build the native helpers (needs the Visual Studio C++ toolset). Output: ..\bin
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo Visual Studio not found & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist ..\bin mkdir ..\bin
set "CLOPT=/nologo /std:c++17 /O2 /EHsc /MT /utf-8 /W3"
cl %CLOPT% /DUNICODE /D_UNICODE lc-audio.cpp /Fe:..\bin\lc-audio.exe /Fo:"%TEMP%\lc-audio.obj" /link /SUBSYSTEM:CONSOLE ole32.lib mmdevapi.lib version.lib user32.lib avrt.lib || exit /b 1
cl %CLOPT% lc-capture.cpp /Fe:..\bin\lc-capture.exe /Fo:"%TEMP%\lc-capture.obj" /link /SUBSYSTEM:CONSOLE || exit /b 1
echo OK
