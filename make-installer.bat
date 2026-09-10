@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ---------------------------------------------------------------------------
rem  FilePathX release build: stripped exe -> portable zip -> NSIS installer.
rem
rem     build\stage\FilePathX.exe            staged payload (zip + installer)
rem     build\FilePathX-win64.zip            auto-update asset
rem     build\FilePathX-<version>-setup.exe  installer
rem
rem  Version comes from src\version.h, so bump it (and the git tag) first.
rem ---------------------------------------------------------------------------

for /f "tokens=3" %%v in ('findstr /b /c:"#define APP_VERSION " src\version.h') do set APPVER=%%~v
if "%APPVER%"=="" (
    echo ERROR: could not read APP_VERSION from src\version.h
    exit /b 1
)
set APPVER4=%APPVER%.0

rem w64devkit ships with the repo; prefer it when gcc is not on PATH
where gcc >nul 2>nul
if errorlevel 1 set "PATH=%~dp0w64devkit\bin;%PATH%"

where gcc >nul 2>nul
if errorlevel 1 (
    echo ERROR: gcc not found. Install w64devkit or add its bin\ to PATH.
    exit /b 1
)

set MAKENSIS=makensis
if exist "%ProgramFiles(x86)%\NSIS\makensis.exe" set "MAKENSIS=%ProgramFiles(x86)%\NSIS\makensis.exe"
where makensis >nul 2>nul
if errorlevel 1 if not exist "%MAKENSIS%" (
    echo ERROR: makensis not found. Install NSIS 3.x from https://nsis.sourceforge.io/
    exit /b 1
)

if not exist build\stage mkdir build\stage
if not exist build\stage\themes mkdir build\stage\themes

echo === Compiling FilePathX %APPVER% ===
windres -I src src\resource.rc -O coff -o build\resource.o || exit /b 1
gcc -O2 -s -Wall -o build\stage\FilePathX.exe ^
    src\main.c src\render.c src\ui.c src\update.c -Isrc build\resource.o ^
    -lopengl32 -lgdi32 -luser32 -lshell32 -lshlwapi ^
    -ldwmapi -lole32 -luuid -luxtheme -lcomctl32 -lpropsys -lwinhttp -mwindows || exit /b 1

copy /y README.md build\stage\README.md >nul || exit /b 1
copy /y LICENSE   build\stage\LICENSE   >nul || exit /b 1
copy /y themes\*.ini build\stage\themes\ >nul || exit /b 1
copy /y themes\README.md build\stage\themes\README.md >nul

echo === Packaging auto-update zip ===
powershell -NoProfile -Command ^
    "Compress-Archive -Path 'build\stage\*' -DestinationPath 'build\FilePathX-win64.zip' -Force" || exit /b 1

echo === Building installer ===
"%MAKENSIS%" /V2 /DAPP_VERSION=%APPVER% /DAPP_VERSION4=%APPVER4% installer.nsi || exit /b 1

echo.
echo Artifacts:
for %%f in (build\FilePathX-win64.zip "build\FilePathX-%APPVER%-setup.exe") do (
    for %%s in (%%~f) do echo   %%~nxs  %%~zs bytes
)
echo.
echo Note: build\release\FilePathX.exe is only refreshed when it is not running.
copy /y build\stage\FilePathX.exe build\release\FilePathX.exe >nul 2>nul
if errorlevel 1 echo   (skipped: build\release\FilePathX.exe is in use)

endlocal
