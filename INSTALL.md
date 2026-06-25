# VR Game Converter — Installation Guide

## Prerequisites

### Build Requirements
- **Visual Studio 2026** (or 2022) with C++ Desktop Development workload
- **Windows SDK** 10.0.26100.0 (or later)
- **CMake** 3.20+

### Runtime Requirements
- **OpenXR runtime** — Install **SteamVR** (recommended) or Windows Mixed Reality
- **VR headset** connected and working
- **Target game** using DirectX 11 or 12

## Build

```powershell
cd C:\Users\shiro\vr-game-converter
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake --build build
```

Output: `build/vr_converter.dll` (DLL) and optionally `build/vrc_test_app.exe` (test harness)

## Usage

### 1. Start target game / test app

```powershell
Start-Process -WindowStyle Normal .\build\vrc_test_app.exe
```

### 2. Inject DLL

```powershell
python tools\injector.py --target vrc_test_app.exe --dll build\vr_converter.dll
```

Or with PID:

```powershell
python tools\injector.py --pid 12345 --dll build\vr_converter.dll
```

### 3. In-game controls

| Key | Action |
|-----|--------|
| F2  | Toggle config overlay |

## Logs

```
%APPDATA%\VRGameConverter\vr_converter.log
```

## Verification

Check the log for:

- `"Present hook installed for API 1"` — D3D11 hook active
- `"OpenXR initialization successful"` — VR runtime connected
- `"StereoRenderer Phase 2 initialized successfully"` — full pipeline ready

If you see `"xrCreateInstance failed"`, no OpenXR runtime is installed — install SteamVR.
