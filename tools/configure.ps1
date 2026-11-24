param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug"
)

# Configure-only helper. Output after building: build\<Config>\vr_converter.dll
# (build with: cmake --build build\<Config>, from a vcvars64 environment).
$Cmake = "$env:LOCALAPPDATA\CMake\cmake-3.31.6-windows-x86_64\bin\cmake.exe"
if (-not (Test-Path $Cmake)) {
    $found = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if ($found) { $Cmake = $found } else { $Cmake = "C:\Program Files\CMake\bin\cmake.exe" }
}
$Ninja = "$env:LOCALAPPDATA\Ninja\ninja.exe"
$VcVars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
# Derive the project root from this script's location (tools\ -> project\).
# The old hard-coded parent path had no CMakeLists.txt and could never configure.
$ProjectPath = Split-Path -Parent $PSScriptRoot
$BuildPath = "$ProjectPath\build\$Config"

Remove-Item -Recurse -Force $BuildPath -ErrorAction SilentlyContinue

# Set up VS environment
Push-Location $env:TEMP
cmd /c "`"$VcVars`" x64 2>&1 && set > vcvars_env_$Config.txt"
Get-Content "vcvars_env_$Config.txt" | ForEach-Object {
    if ($_ -match '^(\w+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
Pop-Location

# Add Ninja and CMake to PATH
$env:Path = "$env:LOCALAPPDATA\CMake\cmake-3.31.6-windows-x86_64\bin;$env:LOCALAPPDATA\Ninja;$env:Path"

# Configure
& $Cmake -S $ProjectPath -B $BuildPath -G Ninja `
    -DCMAKE_MAKE_PROGRAM:FILEPATH="$Ninja" `
    -DCMAKE_BUILD_TYPE=$Config

if ($LASTEXITCODE -eq 0) {
    Write-Host "Configuration successful!" -ForegroundColor Green
} else {
    Write-Error "Configuration failed"
    exit 1
}
