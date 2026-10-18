@echo off
rem Builds the add-on with the Visual Studio 2022 Build Tools. The result is copied next to this file.
rem   build.bat           developer build: hudless_probe.addon64 (all tools visible)
rem   build.bat release   release build:   factorio_fg.addon64  (developer tools hidden by default)
setlocal
set "FG_RELEASE=OFF"
set "BUILD_DIR=build"
set "OUTPUT=hudless_probe.addon64"
if /i "%~1"=="release" (
  set "FG_RELEASE=ON"
  set "BUILD_DIR=build_release"
  set "OUTPUT=factorio_fg.addon64"
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found. Install "Build Tools for Visual Studio 2022" with "Desktop development with C++".
  goto :fail
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo No Visual Studio installation with the C++ x64 tools was found.
  goto :fail
)

call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
if errorlevel 1 goto :fail

where cmake >nul 2>nul || (echo CMake not found. In the Visual Studio Installer, add "C++ CMake tools for Windows". & goto :fail)

cd /d "%~dp0"
cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFG_RELEASE=%FG_RELEASE% || goto :fail
cmake --build "%BUILD_DIR%" || goto :fail
copy /y "%BUILD_DIR%\%OUTPUT%" "%~dp0%OUTPUT%" >nul || goto :fail

echo.
echo Built: %~dp0%OUTPUT%
echo Copy it next to factorio.exe (bin\x64), replacing the old one.
echo Keep only one of hudless_probe.addon64 / factorio_fg.addon64 there.
if not defined FG_NOPAUSE pause
exit /b 0

:fail
echo.
echo Build failed.
if not defined FG_NOPAUSE pause
exit /b 1
