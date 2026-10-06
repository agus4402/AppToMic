@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul && goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
if not exist build mkdir build
rc /nologo /i src /fo build\app.res src\app.rc || exit /b 1
cl /nologo /std:c++17 /utf-8 /O1 /GL /MT /EHsc /W3 /GS- ^
   /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00 ^
   /Fobuild\ src\main.cpp src\engine.cpp src\capture.cpp src\sessions.cpp build\app.res ^
   /Fe:AppToMic.exe ^
   /link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
   ole32.lib mmdevapi.lib avrt.lib comctl32.lib user32.lib shell32.lib version.lib advapi32.lib || exit /b 1
echo.
echo Listo: %~dp0AppToMic.exe
exit /b 0

:novs
echo No se encontro Visual Studio Build Tools con C++.
echo Instalalo con:
echo   winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
exit /b 1
