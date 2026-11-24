# Build script for VR Game Converter (CMake + Ninja + MSVC).
#
# Output goes to build\<Config>\ (e.g. build\Release\vr_converter.dll,
# build\Release\vrc_injector.exe, build\Release\vrc_big_walk_mod.dll,
# build\Release\d3d11_proxy.dll, build\Release\vrc_unit_tests.exe).
# The companion app also searches build\Release\ and build\Debug\.
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Config = "Debug",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$RootDir = Split-Path -Parent $PSScriptRoot
$BuildDir = "$RootDir\build\$Config"

# Find VS Build Tools (VS 18 / 2026 first, then VS 2022 editions)
$VcVars = $null
$possible = @(
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
)
foreach ($p in $possible) {
    if (Test-Path $p) { $VcVars = $p; break }
}
if (-not $VcVars) {
    Write-Error "Visual Studio Build Tools (vcvars64.bat) not found. Install the C++ build tools."
    exit 1
}

# Find CMake
$Cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $Cmake) {
    foreach ($p in @("C:\Program Files\CMake\bin\cmake.exe",
                     "$env:LOCALAPPDATA\CMake\cmake-3.31.6-windows-x86_64\bin\cmake.exe")) {
        if (Test-Path $p) { $Cmake = $p; break }
    }
}
if (-not $Cmake) {
    Write-Error "CMake not found. Install it via: winget install Kitware.CMake"
    exit 1
}

# Find Ninja (the only supported generator: it produces build\<Config>\*.dll
# directly, without the extra per-config subfolder of Visual Studio generators)
$Ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $Ninja -and (Test-Path "$env:LOCALAPPDATA\Ninja\ninja.exe")) {
    $Ninja = "$env:LOCALAPPDATA\Ninja\ninja.exe"
}
if (-not $Ninja) {
    Write-Error "Ninja not found. Install it via: winget install Ninja-build.Ninja"
    exit 1
}

Write-Host "=== VR Game Converter Build ===" -ForegroundColor Cyan
Write-Host "Config: $Config"
Write-Host "Root:   $RootDir"
Write-Host "Build:  $BuildDir"
Write-Host ""

# Clean if requested
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

# Set up VS environment (import the variables vcvars64.bat sets)
Write-Host "Setting up Visual Studio environment..." -ForegroundColor Gray
$EnvDump = "$env:TEMP\vrc_vcvars_$Config.txt"
$EnvBat = "$env:TEMP\vrc_vcvars_$Config.bat"
# A helper .bat avoids cmd /c quote-stripping of the "(x86)" path.
Set-Content -Path $EnvBat -Encoding ASCII -Value @(
    "@call `"$VcVars`" >nul || exit /b 1",
    "@set > `"$EnvDump`""
)
cmd /c $EnvBat
if ($LASTEXITCODE -ne 0) {
    Write-Error "vcvars64.bat failed"
    exit 1
}
Get-Content $EnvDump | ForEach-Object {
    if ($_ -match "^(.*?)=(.*)$") {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
}

# Configure
Write-Host "Configuring with CMake..." -ForegroundColor Gray
& $Cmake -S $RootDir -B $BuildDir -G Ninja `
    "-DCMAKE_MAKE_PROGRAM:FILEPATH=$Ninja" `
    "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed"
    exit 1
}

# Build
Write-Host "Building..." -ForegroundColor Green
& $Cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed"
    exit 1
}

Write-Host ""
Write-Host "=== Build Succeeded ===" -ForegroundColor Green
Write-Host "Output: $BuildDir\vr_converter.dll"
Write-Host "Unit tests: ctest --test-dir `"$BuildDir`" --output-on-failure"
