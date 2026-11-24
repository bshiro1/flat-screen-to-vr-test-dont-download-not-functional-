# VR Game Converter

Runtime injector that shows standard flat-screen PC games in a VR headset
through OpenXR, without modifying the original game files.

Full progress: rendering was reworked and the old inverted-eyes and settings
issues were addressed, pending in-headset testing. Treat everything below as
untested in a headset until that has happened.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                   Game Process                      │
│  ┌──────────┐   ┌──────────┐   ┌─────────────────┐  │
│  │  Game    │   │  D3D11/  │   │  Input          │  │
│  │  Logic   │──▶│  D3D12   │   │  (KBM/Gamepad)  │  │
│  └──────────┘   │  Render  │   └────────▲────────┘  │
│                 └────┬─────┘            │           │
│                 ┌────▼─────┐            │           │
│                 │  Present │            │           │
│                 │ (Hooked) │            │           │
│                 └────┬─────┘            │           │
├──────────────────────┼──────────────────┼───────────┤
│         VR Converter DLL (Injected)     │           │
│  ┌───────────────────▼──────┐ ┌─────────┴────────┐  │
│  │  Frame capture           │ │ Input proxy +    │  │
│  │  ├ Screen: quad layer    │ │ profile mapper   │  │
│  │  └ Depth3D: depth-based  │ │ (SendInput) and  │  │
│  │    per-eye reprojection  │ │ 6DoF bridge      │  │
│  └───────────┬──────────────┘ └──────────────────┘  │
│  ┌───────────▼──────────────┐ ┌──────────────────┐  │
│  │  OpenXR session          │ │ ImGui overlay    │  │
│  │  (frame thread, layers)  │ │ (F2 to toggle)   │  │
│  └──────────────────────────┘ └──────────────────┘  │
└─────────────────────────────────────────────────────┘
```

The DLL hooks `IDXGISwapChain::Present`, copies each finished game frame and
hands it to an OpenXR session running on its own frame thread. The headset
runtime does the per-eye rendering of the virtual screen; the game itself is
not modified and still renders a single (mono) view.

## Building

### Prerequisites
- Visual Studio 2026 (18) or 2022 Build Tools with the C++ desktop workload
- Windows 10/11 SDK
- CMake 3.25+ (presets) and Ninja

Dependencies (MinHook, Dear ImGui, OpenXR SDK, nlohmann/json) are fetched by
CMake `FetchContent` on the first configure, so the first configure needs
network access.

### Build

From a shell where `vcvars64.bat` has been run (or use the scripts, which do
that for you):

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Alternatives:

```powershell
.\tools\build.ps1 -Config Release     # configures + builds into build\Release\
cmake --preset release; cmake --build --preset release   # also build\Release\ (needs vcvars64)
```

`-DVRC_BUILD_UNIT_TESTS=OFF` skips the unit tests; `-DVRC_BUILD_TESTS=ON`
also builds the D3D11/D3D12 test harness applications.

### Output (single-config Ninja build into `build\`)
- `build\vr_converter.dll` - main DLL that is injected into the game
- `build\vrc_injector.exe` - DLL injector used by the companion's **Play in VR**
- `build\d3d11_proxy.dll` - optional `d3d11.dll` proxy for D3D11 games (see below)
- `build\vrc_big_walk_mod.dll` - optional Big Walk controller-hands game-side mod
- `build\vrc_outlast_mod.dll` - Outlast Trials bridge-discovery scaffold
- `build\vrc_unit_tests.exe` - header-only math unit tests

The build scripts and presets use `build\Release\` or `build\Debug\` instead;
the companion app looks in `build\`, `build\Release\`, `build\RelWithDebInfo\`
and `build\Debug\`. Config and input profiles live in `profiles\` in the
source tree; at runtime the DLL reads them from `%APPDATA%\VRGameConverter\`.

## Usage

1. Build the project.
2. Start your OpenXR runtime (e.g. SteamVR) and the target game.
3. Inject the DLL, either with the companion app's **Play in VR** or by hand:
   ```bat
   build\vrc_injector.exe game.exe "%CD%\build\vr_converter.dll"
   ```
   (`vrc_injector.exe` passes the path to `LoadLibrary` inside the game, so it
   must be absolute), or
   ```bat
   python tools\injector.py --target game.exe --dll build\vr_converter.dll
   ```
4. Press F2 in-game to toggle the configuration overlay.

### D3D11 proxy deployment (no injector)

For D3D11 games you can instead copy two files into the folder that contains
the game's executable:

- `build\d3d11_proxy.dll`, renamed to `d3d11.dll`
- `build\vr_converter.dll`

The proxy loads the real `d3d11.dll` from System32, forwards every export to
it and loads `vr_converter.dll` from its own folder. No `d3d11_orig.dll` copy
is needed. Remove the two files to uninstall.

### Render modes

- **Screen (default)** - the game frame is shown on a flat virtual screen,
  submitted to the runtime as an OpenXR quad layer. The runtime renders the
  quad for each eye, so stereo and IPD are correct by construction (the image
  itself is flat). The screen can be world-locked (fixed in the room) or
  head-locked. Works for D3D11 and D3D12 games.
- **Depth3D (experimental, D3D11 only)** - per-pixel reprojection of the game
  frame for each eye using the game's captured depth buffer, which gives real
  but approximate depth parallax. Enable **Reversed-Z depth** for Unity and
  Unreal games. The **game FOV** setting must match the game's own field of
  view, otherwise depth is reconstructed wrongly. Without a usable depth buffer
  (or on D3D12) it falls back to Screen.

The former **True Stereo** mode was removed. It rotated the game camera
between alternate frames, but a rotation cannot produce depth-dependent
parallax, and the injected mouse-look offset made the player's aim jitter.
Config files that still contain `render_mode: 2` are loaded as Screen.

### Settings

| Overlay setting | `config.json` key | Meaning |
|---|---|---|
| Render mode | `render_mode` | `0` = Screen, `1` = Depth3D (`2` loads as Screen) |
| Screen Distance | `convergence_distance` | Distance from you to the virtual screen, in metres |
| Screen Size FOV | `fov_override` | Horizontal angular size of the screen in degrees; `0` = 60° |
| Screen Scale | `world_scale` | Scale multiplier for the screen; also published to mods through the bridge |
| World-locked screen | `enable_head_tracking` | On = screen fixed in the room, off = screen follows your head |
| Render Scale | `render_scale` | Swapchain resolution multiplier; applies on the next launch, not live |
| Reversed-Z depth | `depth_reversed_z` | Depth3D: the game uses reversed-Z (Unity, Unreal) |
| Game FOV | `depth_fov_degrees` | Depth3D: must match the game's field of view |
| Depth near/far | `depth_near_plane`, `depth_far_plane` | Depth3D: the game camera's clip planes |
| Parallax Strength | `parallax_strength` | Depth3D: `0` = flat, `1` = full reprojection |
| Swap Eyes / eye debug | `swap_eyes`, `show_eye_debug` | Depth3D only |

The config file is `%APPDATA%\VRGameConverter\config.json`.

These keys are still accepted in `config.json` (so old files keep loading) but
are now **ignored** by the DLL: `eye_height`, `enable_chaperone`,
`foveated_rendering`, `dynamic_resolution`, `stereo_pair_delta_pixels`. The
companion app still shows them; changing them has no effect.

### Big Walk controller hands

The `big_walk_hands` config profile selects the `big_walk` input profile with
the 6DoF Bridge enabled and legacy mouse/keyboard forwarding still on. The
bridge publishes real controller grip and aim poses, trigger/grip axes and
button state in `VrcXRState` (see `src/input/xr_shared_state.h`) through
`VRC_CopyXRState()` (size-checked) or the legacy `VRC_GetXRState()` export and
named shared-memory mapping. When a runtime exposes `XR_EXT_hand_tracking`, all
26 hand joints per hand are published too, with `VrcState_HandTrackingActive`
set while either hand is active. The state also carries the runtime's predicted
display period, measured IPD and per-eye frustum tangents, so mods do not have to
guess the headset projection from config values. A game-side mod can read those
poses and drive the character's hand IK.

A profile by itself cannot make the unmodified game treat the controllers as
IK hands. The `big_walk_hands` profile therefore sets `game_mod_enabled = true`
with an empty `game_mod_path`, which makes the companion's **Play in VR** inject
the bundled `vrc_big_walk_mod.dll` immediately after `vr_converter.dll`. The mod
hooks `PlayerArms::LateUpdate`, `PlayerCharacter::LateUpdate`,
`UpdateScreenSpaceArms`, and the private `ScreenSpacize()` helper. It reads
`VrcXRState` through `VRC_CopyXRState()`, maps tracked grip poses (falling back to
aim poses, then the last accepted pose) to the local player's arm/hand transforms
and `PlayerHands.grasper`, hides stretched `LimbSpline` meshes, resets hand local
scales, and suppresses the screen-space rescale that otherwise stretches the
visible hands. Check
`%TEMP%\vrc_big_walk_mod.log` for hook/bridge status; the previous run is
rotated to a timestamped suffix on startup. The RVA-based hooks target the Big
Walk 31/08/2026 IL2CPP build and need updating if the game is patched.
The profile still stores `render_mode: 2` from the removed True Stereo mode,
which now loads as Screen.

The input profile also maps controller inputs to native Big Walk actions as a
fallback: left grip = `Q`, right grip = `E`, right trigger = left mouse
(grab/use/throw), left trigger = `F` (interact), A/jump, B/crouch, left stick
click/sprint.

### Outlast Trials (early scaffold)

> **Anti-cheat warning.** The Outlast Trials ships Easy Anti-Cheat (EAC).
> Injecting this DLL (or any DLL) into an EAC-protected game can get your
> account banned or the game process blocked, whatever session type you play
> (solo, private or online). Only use it with EAC disabled or with offline
> builds where that is permitted. There is no claim of anti-cheat safety.

The `outlast_trials` config profile selects the `outlast_trials` input profile
(`TOTClient-Win64-Shipping.exe`), keeps Legacy SendInput forwarding on, and
injects `vrc_outlast_mod.dll` after the converter. That DLL is currently a safe
**discovery scaffold**: it resolves `VRC_CopyXRState()` /
`VRC_RequestHaptic()` dynamically, validates the bridge ABI, and logs pose
health to `%TEMP%\vrc_outlast_mod.log`. It installs **no game hooks yet**.

The interaction plan document referred to in older notes is not in this
repository. The game is a custom UE 4.27.1 fork, ships both D3D11 and D3D12
paths and is always online. The shipped game PDB is a Steam debug-symbol
download, not present in a normal install, so discovery requires either
installing debug symbols or generating a build-fresh SDK dump before any
interaction hooks are written. In Steam, open the game's
**Properties → Installed Files → Download Debug Symbols**; after it finishes,
run:

```powershell
.\build\vrc_pdb_dump.exe "C:\Program Files (x86)\Steam\steamapps\common\The Outlast Trials\OPP\Binaries\Win64\TOTClient-Win64-Shipping.pdb" ProcessEvent UObject UWorld FName
```

Alternatively, `tools\install_outlast_symbols.ps1` prompts for the Steam account
that owns the game and runs `steamcmd +app_debug_install 1304930`. It does not
save the password. Close the Steam desktop client first if SteamCMD reports that
the account is already logged in. `tools\dump_outlast_symbols.ps1` then finds the
PDB in the game folder or the debug-symbol download folder and writes a broader
symbol dump to `reports\outlast_pdb_symbols.txt` (the script creates that
folder).

The utility emits `0xRVA symbol` lines and a `matches=` count. Do not use the
historical `0x1A16300` `ProcessEvent` offset from public writeups; re-derive it
from the installed build.

## Status

Nothing here has been verified in a headset since the rendering rework.

**Works in principle (needs in-headset testing)**
- Present hooking for D3D11 and D3D12, injection via `vrc_injector.exe` or the
  D3D11 proxy
- Screen mode: the game frame on a world-locked or head-locked virtual screen
  (D3D11, and D3D12 through D3D11On12)
- ImGui configuration overlay (F2), D3D11 and D3D12
- Input proxy with profile-based mapping of VR controllers to keyboard, mouse
  and gamepad input, and the 6DoF shared-memory bridge for game-side mods

**Experimental**
- Depth3D reprojection (D3D11 only; needs correct game FOV and depth settings)
- Big Walk controller-hands mod (tied to one game build)
- Outlast Trials discovery scaffold (no hooks; see the anti-cheat warning)

**Not implemented**
- OpenGL and Vulkan games
- Real engine-level stereo, where the game renders a separate view per eye.
  That needs hooks into the engine's camera and rendering, not just the
  swapchain. For Unreal Engine games [UEVR](https://github.com/praydog/UEVR)
  is the established open-source example of that approach.

## Compatibility

| API    | Hook      | Screen mode | Depth3D | Overlay |
|--------|-----------|-------------|---------|---------|
| D3D11  | Yes       | Yes         | Experimental | Yes |
| D3D12  | Yes       | Yes (D3D11On12) | No (falls back to Screen) | Yes (D3D11On12) |
| OpenGL | No        | No          | No      | No      |
| Vulkan | No        | No          | No      | No      |
