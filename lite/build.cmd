@echo off
rem Build the WebView2 shell (LiveCord Lite). Needs the Visual Studio C++ toolset and the WebView2 SDK in .\webview2sdk
rem (unzip the Microsoft.Web.WebView2 NuGet package there: build\native\include and build\native\x64). Output: .\out
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo Visual Studio not found & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
set "SDK=webview2sdk\build\native"
rc /nologo /fo "%TEMP%\lc-lite.res" lite.rc || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /utf-8 /W3 /DUNICODE /D_UNICODE /I "%SDK%\include" lc-lite.cpp "%TEMP%\lc-lite.res" /Fe:out\LiveCordLite.exe /Fo:"%TEMP%\lc-lite.obj" /link /SUBSYSTEM:WINDOWS /MANIFEST:NO "%SDK%\x64\WebView2LoaderStatic.lib" || exit /b 1
echo OK
