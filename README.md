# VR Game Converter

Runtime video game converter that transforms standard flat-screen PC/console games
into fully immersive VR experiences without modifying original game files.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                   Game Process                      │
│  ┌──────────┐   ┌──────────┐   ┌─────────────────┐  │
│  │  Game    │   │  D3D11/  │   │  Input          │  │
│  │  Logic   │──▶│  D3D12  │──▶│  (KBM/Gamepad)  │  │
│  └──────────┘   │  Render  │   └─────────────────┘  │
│                 └────┬─────┘                        │
│                      │                              │
│                 ┌────▼─────┐                        │
│                 │  Present │                        │
│                 │  (Hooked)│                        │
│                 └────┬─────┘                        │
├──────────────────────┼──────────────────────────────┤
│         VR Converter DLL (Injected)                 │
│  ┌──────────────────┐┌──────────────────────────┐   │
│  │  Stereo Renderer ││  Input Proxy + Mapper    │   │
│  │  ┌────────────┐  ││  ┌──────────────────┐    │   │
│  │  │ Camera Rig │  ││  │ Control Profiles │    │   │
│  │  └────────────┘  ││  └──────────────────┘    │   │
│  │  ┌────────────┐  ││  ┌──────────────────┐    │   │
│  │  │ Lens Dist  │  ││  │ VR Controller    │    │   │
│  │  │ Correction │  ││  │ Mapping          │    │   │
│  │  └────────────┘  ││  └──────────────────┘    │   │
│  └────────┬─────────┘└──────────────────────────┘   │
│           │                                         │
│  ┌────────▼─────────┐  ┌────────────────────────┐   │
│  │  OpenXR Runtime  │  │  ImGui Config Overlay  │   │
│  │  (Head Tracking) │  │  (F2 to toggle)        │   │
│  └──────────────────┘  └────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

## Building

### Prerequisites
- Visual Studio 2022 Build Tools (or full VS 2022)
- CMake 3.20+
- Windows 10/11 SDK

### Build
```powershell
# Using build script
.\tools\build.ps1 -Config Release

# Or CMake directly
cmake -S . -B build/release -G "Visual Studio 17 2022" -A x64
cmake --build build/release --config Release
```

### Output
- `build/Release/bin/vr_converter.dll` - Main injection DLL
- `build/Release/bin/profiles/` - Default input profiles

## Usage

1. Build the project to produce `vr_converter.dll`
2. Launch your target game
3. Inject the DLL using the injector tool:
   ```bash
   python tools\injector.py --target game.exe --dll build\Release\bin\vr_converter.dll
   ```
4. Press F2 in-game to toggle the configuration overlay
5. Configure VR settings, controls, and rendering options

## Phases

- **Phase 1**: Core hooking framework + mono→stereo render pass prototype ✅
- **Phase 2**: 6DoF head tracking + latency compensation
- **Phase 3**: Universal input proxy + profile-based control mapper
- **Phase 4**: Performance optimization, compatibility tier system
- **Phase 5**: Polish, crash recovery, anti-cheat handling, distribution

## Compatibility

| API       | Hook      | Stereo | Overlay |
|-----------|-----------|--------|---------|
| D3D11     | ✅ VTable | ✅ MVP | ✅ ImGui |
| D3D12     | ✅ VTable | ✅ MVP | ⏳ Planned |
| OpenGL    | ⏳ Planned|        |         |
| Vulkan    | ⏳ Planned|        |         |
