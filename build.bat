@echo off
setlocal enabledelayedexpansion

set ARCHITECTURE=x64
set BUILD_TYPE=Release
set BUILD_DIR=build

cd /d "%~dp0"

if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /C:"RenderCore" "%BUILD_DIR%\CMakeCache.txt" >nul
    if errorlevel 1 (
        echo [CMake] Wrong cache detected, deleting...
        rmdir /s /q "%BUILD_DIR%"
    )
)

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

echo [CMake] Generating project...
cmake -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A %ARCHITECTURE% -DBUILD_SHARED_LIBS=OFF -DASSIMP_BUILD_ZLIB=ON -DLUA_BUILD_SHARED_LIBS=OFF

if errorlevel 1 (
    echo [ERROR] CMake generation failed.
    pause
    exit /b 1
)

echo [CMake] Building project...
cmake --build %BUILD_DIR% --config %BUILD_TYPE%
if errorlevel 1 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

echo.
echo [OK] Build finished!
echo Executable is located at: %BUILD_DIR%\%BUILD_TYPE%\RenderCore.exe
pause