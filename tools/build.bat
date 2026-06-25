@echo off
setlocal enabledelayedexpansion

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set CMAKE=%LOCALAPPDATA%\CMake\cmake-3.31.6-windows-x86_64\bin\cmake.exe
set NINJA=%LOCALAPPDATA%\Ninja\ninja.exe
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set VSWHERE="C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set PROJECT=C:\Users\shiro\vr-game-converter
set BUILD=%PROJECT%\build\%CONFIG%

echo === VR Game Converter Build (%CONFIG%) ===
echo.

:: Check prerequisites
if not exist "%CMAKE%" (
    echo ERROR: CMake not found at %CMAKE%
    echo Install: winget install Kitware.CMake
    exit /b 1
)

if not exist "%NINJA%" (
    echo ERROR: Ninja not found at %NINJA%
    echo Install: Download ninja-win.zip from https://github.com/ninja-build/ninja/releases
    exit /b 1
)

if not exist "%VCVARS%" (
    echo ERROR: VS Build Tools 2022 not found at %VCVARS%
    echo Install: winget install Microsoft.VisualStudio.2022.BuildTools
    exit /b 1
)

:: Set up VS environment
echo Setting up Visual Studio environment...
call %VCVARS% x64
if ERRORLEVEL 1 (
    echo ERROR: vcvars64.bat failed to initialize
    echo Make sure Windows SDK is installed with the Build Tools.
    echo Run: vs_installer.exe modify --add Microsoft.VisualStudio.Component.Windows10SDK.20348
    exit /b 1
)

echo Checking Windows SDK availability...
if "%WindowsSdkDir%"=="" (
    echo WARNING: Windows SDK directory not set. The Windows 10/11 SDK
    echo may not be installed. Libraries like kernel32.lib will be missing.
    echo.
    echo To fix: Open "Visual Studio Installer", modify Build Tools,
    echo and add "Windows 10/11 SDK" component.
    echo.
    echo Attempting build anyway...
)

:: Add tools to PATH
set PATH=%LOCALAPPDATA%\CMake\cmake-3.31.6-windows-x86_64\bin;%LOCALAPPDATA%\Ninja;%PATH%

:: Clean
if exist "%BUILD%" rmdir /s /q "%BUILD%"

:: Configure
echo Configuring with CMake...
"%CMAKE%" -S "%PROJECT%" -B "%BUILD%" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM:FILEPATH="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=%CONFIG%
if %ERRORLEVEL% neq 0 (
    echo Configuration failed!
    exit /b 1
)

:: Build
echo Building...
"%CMAKE%" --build "%BUILD%" --config %CONFIG%
if %ERRORLEVEL% equ 0 (
    echo.
    echo === Build Succeeded ===
    echo Output: %BUILD%\bin\vr_converter.dll
    dir /s "%BUILD%\*.dll"
) else (
    echo Build failed!
    exit /b 1
)
