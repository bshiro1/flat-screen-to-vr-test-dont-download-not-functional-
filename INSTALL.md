# VR Game Converter — Installation Guide

> **Anti-cheat warning.** Do not inject into games protected by anti-cheat.
> The Outlast Trials ships Easy Anti-Cheat; injecting this DLL into an
> EAC-protected game can get the account banned or the process blocked,
> whatever session type is used. Only use it with EAC disabled or with offline
> builds where that is permitted.

## Prerequisites

### Build Requirements
- **Visual Studio 2026 (18)** or 2022 Build Tools with the C++ desktop workload
- **Windows SDK** 10.0.26100.0 (or later)
- **CMake** 3.25+ and **Ninja**
- Network access on the first configure (CMake downloads MinHook, Dear ImGui,
  the OpenXR SDK and nlohmann/json)

### Runtime Requirements
- **OpenXR runtime** — install **SteamVR** (recommended) or another OpenXR
  runtime and make it the active runtime
- **VR headset** connected and working
- **Target game** using DirectX 11 or 12 (OpenGL and Vulkan are not supported)

## Build

Run from the project folder (the one containing `CMakeLists.txt`), in `cmd`:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`vcvars64.bat` must run first in the same shell so CMake finds the MSVC
compiler and Windows SDK. If `ninja.exe` is not on `PATH`, add
`-DCMAKE_MAKE_PROGRAM=%LOCALAPPDATA%\Ninja\ninja.exe` to the configure line.
Alternatively, `.\tools\build.ps1 -Config Release` does all of this and
builds into `build\Release\`.

Output (in `build\`):
- `vr_converter.dll` — the DLL that is injected into the game
- `vrc_injector.exe` — injector used by the companion's **Play in VR**
- `d3d11_proxy.dll` — optional `d3d11.dll` proxy for D3D11 games
- `vrc_big_walk_mod.dll` — optional Big Walk hands mod
- `vrc_unit_tests.exe` — unit tests (skip with `-DVRC_BUILD_UNIT_TESTS=OFF`)
- `vrc_test_app.exe` — D3D11 test harness, only with `-DVRC_BUILD_TESTS=ON`

## Usage

### 1. Start the OpenXR runtime and the target game / test app

```bat
start "" build\vrc_test_app.exe
```

### 2a. Inject the DLL

```bat
build\vrc_injector.exe vrc_test_app.exe "%CD%\build\vr_converter.dll"
```

The DLL path must be absolute. The Python injector resolves relative paths
itself:

```bat
python tools\injector.py --target vrc_test_app.exe --dll build\vr_converter.dll
python tools\injector.py --pid 12345 --dll build\vr_converter.dll
```

### 2b. Or deploy the D3D11 proxy (D3D11 games only)

Copy into the folder that contains the game's executable:

- `build\d3d11_proxy.dll`, renamed to `d3d11.dll`
- `build\vr_converter.dll`

The proxy loads the real `d3d11.dll` from System32 and then loads
`vr_converter.dll` from its own folder; no `d3d11_orig.dll` is needed. Delete
both files to uninstall.

### 3. In-game controls

| Key | Action |
|-----|--------|
| F2  | Toggle config overlay |

Render modes and settings are described in `README.md`. Screen mode is the
default; Depth3D is experimental and D3D11 only.

## Logs and config

```
%APPDATA%\VRGameConverter\vr_converter.log
%APPDATA%\VRGameConverter\config.json
```

## Verification

Check the log for these lines (exact wording may change as the code evolves):

- `Present hook installed for D3D11` or `Present hook installed for D3D12` — the
  Present hook is active
- `OpenXR runtime:` — the OpenXR runtime was found (followed by its name and
  version)
- `OpenXR session begun` — the VR session is running
- `Frame thread started` — frames are being submitted to the headset

If you see `xrCreateInstance failed`, no OpenXR runtime is installed or active —
install SteamVR and set it as the OpenXR runtime.
