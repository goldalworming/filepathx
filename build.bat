@echo off
setlocal

where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo ERROR: cl.exe not found. Run from a Visual Studio Developer Command Prompt.
    exit /b 1
)

if not exist build mkdir build

echo Compiling resource...
rc /nologo /fo build\resource.res src\resource.rc

echo Building FilePathX...
cl /nologo /O2 /W3 /WX- ^
    src\main.c src\render.c src\ui.c ^
    /I src ^
    /Fe:build\FilePathX.exe ^
    /Fo:build\ ^
    /link /SUBSYSTEM:WINDOWS ^
    build\resource.res ^
    user32.lib gdi32.lib opengl32.lib shell32.lib shlwapi.lib dwmapi.lib ole32.lib uuid.lib uxtheme.lib

if %errorlevel% equ 0 (
    echo Build successful: build\FilePathX.exe
) else (
    echo Build FAILED.
)
