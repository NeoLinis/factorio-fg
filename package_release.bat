@echo off
rem Builds the release add-on and zips it with AMD's FSR frame generation DLLs, the README, changelog and licences: dist\factorio-fg-<version>.zip
setlocal
cd /d "%~dp0"
for /f "tokens=3" %%v in ('findstr /c:"#define ADDON_VERSION_STRING" src\version.h') do set "VER=%%~v"
if not defined VER (echo Could not read the version from src\version.h & goto :fail)
if not defined FG_NOPAUSE (set "FG_RESTORE_PAUSE=1" & set "FG_NOPAUSE=1")
call "%~dp0build.bat" release || goto :fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\fetch_fsr.ps1" || goto :fail
if defined FG_RESTORE_PAUSE set "FG_NOPAUSE="
set "OUT=dist\factorio-fg-%VER%"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" || goto :fail
copy /y factorio_fg.addon64 "%OUT%\" >nul || goto :fail
copy /y third_party\fidelityfx\bin\amd_fidelityfx_loader_dx12.dll "%OUT%\" >nul || goto :fail
copy /y third_party\fidelityfx\bin\amd_fidelityfx_framegeneration_dx12.dll "%OUT%\" >nul || goto :fail
copy /y third_party\fidelityfx\license.md "%OUT%\AMD_FidelityFX_license.md" >nul || goto :fail
copy /y README.md "%OUT%\" >nul || goto :fail
copy /y CHANGELOG.md "%OUT%\" >nul || goto :fail
copy /y LICENSE "%OUT%\LICENSE.txt" >nul || goto :fail
copy /y THIRD_PARTY_NOTICES.md "%OUT%\" >nul || goto :fail
if exist "%OUT%.zip" del "%OUT%.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath '%OUT%.zip'" || goto :fail
echo.
echo Packaged: %~dp0%OUT%.zip
if not defined FG_NOPAUSE pause
exit /b 0

:fail
echo.
echo Packaging failed.
if not defined FG_NOPAUSE pause
exit /b 1
