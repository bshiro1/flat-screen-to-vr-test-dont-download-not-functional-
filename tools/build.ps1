# Build script for VR Game Converter
param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$RootDir = Split-Path -Parent $PSScriptRoot
$BuildDir = "$RootDir\build\$Config"

# Find VS Build Tools
$VcVars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $VcVars)) {
    # Try VS 2022 Community/Professional/Enterprise
    $possible = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($p in $possible) {
        if (Test-Path $p) { $VcVars = $p; break }
    }
}

if (-not (Test-Path $VcVars)) {
    Write-Error "Visual Studio 2022 Build Tools not found. Please install them."
    exit 1
}

# Find CMake
$Cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $Cmake) {
    $CmakePath = "$env:LOCALAPPDATA\CMake\cmake-3.31.6-windows-x86_64\bin\cmake.exe"
    if (Test-Path $CmakePath) {
        $Cmake = $CmakePath
    } else {
        Write-Error "CMake not found. Install it via: winget install Kitware.CMake"
        exit 1
    }
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

# Create build directory
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

# Set up VS environment
Write-Host "Setting up Visual Studio environment..." -ForegroundColor Gray
Push-Location $BuildDir
cmd /c "`"$VcVars`" x64 && set > %TEMP%\vcvars.txt" | Out-Null
Get-Content "$env:TEMP\vcvars.txt" | ForEach-Object {
    if ($_ -match "^(.*?)=(.*)$") {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
}
Pop-Location

# Configure with CMake
Write-Host "Configuring with CMake..." -ForegroundColor Gray
& $Cmake -S $RootDir -B $BuildDir -G "Ninja" `
    -DCMAKE_BUILD_TYPE=$Config `
    -DCMAKE_CXX_COMPILER:FILEPATH="$env:VCINSTALLDIR\Tools\MSVC\$env:VCTOOLSVERSION\bin\Hostx64\x64\cl.exe"

if ($LASTEXITCODE -ne 0) {
    # Fall back to Visual Studio generator
    & $Cmake -S $RootDir -B $BuildDir -G "Visual Studio 17 2022" `
        -A x64 `
        -DCMAKE_BUILD_TYPE=$Config
}

if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed"
    exit 1
}

# Build
Write-Host "Building..." -ForegroundColor Green
& $Cmake --build $BuildDir --config $Config

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "=== Build Succeeded ===" -ForegroundColor Green
    Write-Host "Output: $BuildDir\bin\vr_converter.dll"
} else {
    Write-Error "Build failed"
    exit 1
}
