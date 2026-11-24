cmake_minimum_required(VERSION 3.20)
project(vr-game-converter VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output layout: with the single-config Ninja build every target lands directly
# in the build directory (build/vr_converter.dll, build/vrc_injector.exe,
# build/vrc_big_walk_mod.dll, build/d3d11_proxy.dll). The companion app's
# converter_artifacts() looks in build/<BuildType>/, build/, build/Release/,
# build/RelWithDebInfo/ and build/Debug/, so do not redirect outputs elsewhere.

# ─── Dependencies ──────────────────────────────────────────────────────────────
# To configure offline, point FetchContent at already-downloaded sources, e.g.
#   -DFETCHCONTENT_SOURCE_DIR_MINHOOK=<dir> -DFETCHCONTENT_SOURCE_DIR_IMGUI=<dir>
#   -DFETCHCONTENT_SOURCE_DIR_OPENXR_LOADER=<dir>
#   -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=<dir>

include(FetchContent)

# MinHook - lightweight hooking library. Pinned to a commit for reproducible
# builds (MinHook has no recent release tag).
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG 8af6b4acae5a9388fd742b56fa79ece89d96f823
)
set(MINHOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MINHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(minhook)

# Dear ImGui - runtime overlay UI. ImGui has no CMakeLists.txt, so
# MakeAvailable only downloads it; its sources are compiled into vr_converter.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
# The overlay renders through D3D11 (D3D12 games via D3D11On12), so only the
# DX11 and Win32 backends are needed.
set(IMGUI_BACKEND_SOURCES
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

# OpenXR SDK - VR runtime interface (static loader on Windows)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    openxr_loader
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.42
)
FetchContent_MakeAvailable(openxr_loader)

# nlohmann/json - JSON config serialization
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ─── Source Files ──────────────────────────────────────────────────────────────

set(CORE_SOURCES
    src/core/config.cpp
    src/core/logging.cpp
    src/core/perf_monitor.cpp
)

set(HOOK_SOURCES
    src/hook/hook_manager.cpp
    src/hook/dx11_hook.cpp
    src/hook/dx12_hook.cpp
)

set(RENDER_SOURCES
    src/render/stereo_renderer.cpp
    src/render/depth_reprojection.cpp
)

set(VR_SOURCES
    src/vr/openxr_context.cpp
    src/vr/tracking.cpp
)

set(INPUT_SOURCES
    src/input/input_proxy.cpp
    src/input/input_mapper.cpp
    src/input/xr_input.cpp
    src/input/xr_shared_state.cpp
)

set(OVERLAY_SOURCES
    src/overlay/imgui_overlay.cpp
    src/overlay/config_editor.cpp
)

set(ALL_SOURCES
    src/dllmain.cpp
    ${CORE_SOURCES}
    ${HOOK_SOURCES}
    ${RENDER_SOURCES}
    ${VR_SOURCES}
    ${INPUT_SOURCES}
    ${OVERLAY_SOURCES}
    ${IMGUI_SOURCES}
    ${IMGUI_BACKEND_SOURCES}
)

# ─── Targets ───────────────────────────────────────────────────────────────────

add_library(vr_converter SHARED ${ALL_SOURCES})
# Only explicitly marked functions are exported: StartVRConverter,
# StopVRConverter and VRCbtProc (dllmain.cpp) plus the VRC_BRIDGE_API bridge
# functions in src/input/xr_shared_state.h. Exporting everything leaked ~8k
# symbols (all of ImGui and nlohmann) from the DLL.
set_target_properties(vr_converter PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

target_include_directories(vr_converter PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${minhook_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${openxr_loader_SOURCE_DIR}/include
    ${nlohmann_json_SOURCE_DIR}/include
)

# Desktop libraries only. Do NOT add windowsapp.lib (the UWP umbrella library):
# it redirects imports such as XInput to UWP DLLs (XINPUTUAP.dll).
target_link_libraries(vr_converter PRIVATE
    minhook
    openxr_loader
    nlohmann_json::nlohmann_json
    d3d11.lib
    d3d12.lib
    dxgi.lib
    dxguid.lib
    d3dcompiler.lib
)

# d3d12.dll is delay-loaded so injecting into a D3D11 game does not pull
# d3d12.dll into the process (which confused graphics-API detection). It is
# only loaded when a D3D12 code path actually calls into it. d3d11.dll and
# dxgi.dll stay as normal imports.
if(MSVC)
    target_link_libraries(vr_converter PRIVATE delayimp.lib)
    target_link_options(vr_converter PRIVATE /DELAYLOAD:d3d12.dll)
endif()

target_compile_definitions(vr_converter PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
    XR_USE_GRAPHICS_API_D3D11
    VRC_BRIDGE_EXPORTS
)

if(MSVC)
    target_compile_options(vr_converter PRIVATE /W4 /utf-8 /MP)
endif()

# ─── D3D11 Proxy DLL ────────────────────────────────────────────────────────────
# Optional deployment for D3D11 games: copy build/d3d11_proxy.dll into the game
# folder as d3d11.dll, next to vr_converter.dll. The proxy loads the real
# d3d11.dll from System32 and forwards to it (no d3d11_orig.dll copy needed),
# then loads vr_converter.dll.

set(D3D11_PROXY_SOURCES
    src/proxy/d3d11_proxy.cpp
)
add_library(d3d11_proxy SHARED ${D3D11_PROXY_SOURCES} src/proxy/d3d11_proxy.def)

# The .def export list is manually maintained, NOT auto-generated.
# See notes in src/proxy/d3d11_proxy.cpp.
target_include_directories(d3d11_proxy PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(d3d11_proxy PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(d3d11_proxy PRIVATE d3d11.lib dxgi.lib)
set_target_properties(d3d11_proxy PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
set_property(TARGET d3d11_proxy PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(d3d11_proxy PRIVATE /W4 /utf-8 /GS-)
endif()

# Note: D3D12 proxy DLL is NOT built here.  D3D12 hooking is done via
# MinHook inside vr_converter.dll (injection-based), NOT via a proxy DLL.
# The proxy DLL approach for D3D12 fails because loading a second copy of
# d3d12.dll creates CRT/state conflicts (D3D12CreateDevice returns
# DXGI_ERROR_NOT_FOUND).  vr_converter.dll hooks the REAL system d3d12.dll
# directly via GetProcAddress + MinHook.

# ─── Tools ─────────────────────────────────────────────────────────────────────

add_executable(vrc_injector tools/injector.cpp)
target_compile_definitions(vrc_injector PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(vrc_injector PRIVATE /W4 /utf-8)
endif()

# ─── Test Harness ──────────────────────────────────────────────────────────────

option(VRC_BUILD_TESTS "Build test harness applications" OFF)
if(VRC_BUILD_TESTS)
    add_executable(vrc_test_app tests/test_app.cpp)
    target_include_directories(vrc_test_app PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(vrc_test_app PRIVATE
        d3d11.lib dxgi.lib d3dcompiler.lib)
    target_compile_definitions(vrc_test_app PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(vrc_test_app PRIVATE /W4 /utf-8)
    endif()

    add_executable(vrc_test_d3d12 tests/test_d3d12.cpp)
    target_link_libraries(vrc_test_d3d12 PRIVATE d3d12.lib dxgi.lib)
    target_compile_definitions(vrc_test_d3d12 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(vrc_test_d3d12 PRIVATE /W4 /utf-8)
    endif()
endif()

# ─── Unit Tests ────────────────────────────────────────────────────────────────
# Header-only math tests. They include headers from src/ but link nothing from
# vr_converter, so they build even when the DLL does not. Run with
#   ctest --test-dir build --output-on-failure      (or build/vrc_unit_tests.exe)

option(VRC_BUILD_UNIT_TESTS "Build the header-only unit tests (vrc_unit_tests)" ON)
if(VRC_BUILD_UNIT_TESTS)
    enable_testing()
    file(GLOB VRC_UNIT_TEST_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tests/unit/*.cpp")
    add_executable(vrc_unit_tests ${VRC_UNIT_TEST_SOURCES})
    target_include_directories(vrc_unit_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests/unit
    )
    target_compile_definitions(vrc_unit_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
    if(MSVC)
        target_compile_options(vrc_unit_tests PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME vrc_unit_tests COMMAND vrc_unit_tests)
endif()

# ─── Game-side mods ───────────────────────────────────────────────────────────

add_library(vrc_big_walk_mod SHARED src/mods/big_walk_mod.cpp)
target_include_directories(vrc_big_walk_mod PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${minhook_SOURCE_DIR}/include
)
target_link_libraries(vrc_big_walk_mod PRIVATE minhook)
target_compile_definitions(vrc_big_walk_mod PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
set_target_properties(vrc_big_walk_mod PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
)
if(MSVC)
    target_compile_options(vrc_big_walk_mod PRIVATE /W4 /utf-8 /MP)
endif()

# Outlast Trials discovery scaffold. No engine hooks are installed yet. The
# phased hook/interaction plan document is not part of this repository.
add_library(vrc_outlast_mod SHARED src/mods/outlast_trials_mod.cpp)
target_include_directories(vrc_outlast_mod PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_SOURCE_DIR}/include
)
target_link_libraries(vrc_outlast_mod PRIVATE advapi32)
target_compile_definitions(vrc_outlast_mod PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
set_target_properties(vrc_outlast_mod PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
)
add_custom_target(vrc_outlast_verb_maps ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/outlast_verb_map.json"
            "${CMAKE_BINARY_DIR}/outlast_verb_map.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/outlast_verb_map.validated.json"
            "${CMAKE_BINARY_DIR}/outlast_verb_map.validated.json"
    DEPENDS
            "${PROJECT_SOURCE_DIR}/outlast_verb_map.json"
            "${PROJECT_SOURCE_DIR}/outlast_verb_map.validated.json"
    COMMENT "Copying Outlast verb maps beside the game modules"
)
add_dependencies(vrc_outlast_mod vrc_outlast_verb_maps)
if(MSVC)
    target_compile_options(vrc_outlast_mod PRIVATE /W4 /utf-8 /MP)
endif()

# ─── PDB symbol dump tool (Windows / Visual Studio DIA SDK) ───────────────────

# The DIA SDK ships with Visual Studio Build Tools. When available, build a
# small utility that extracts Unreal symbol RVAs from the game's optional Steam
# debug-symbol PDB. This is discovery only; it does not touch the game.
set(VRC_DIA_SDK_DIR "$ENV{VSINSTALLDIR}DIA SDK")
if(WIN32 AND EXISTS "${VRC_DIA_SDK_DIR}/include/dia2.h")
    add_executable(vrc_pdb_dump tools/pdb_dump.cpp)
    target_include_directories(vrc_pdb_dump PRIVATE "${VRC_DIA_SDK_DIR}/include")
    target_link_directories(vrc_pdb_dump PRIVATE "${VRC_DIA_SDK_DIR}/lib/amd64")
    target_link_libraries(vrc_pdb_dump PRIVATE diaguids ole32 oleaut32)
    if(MSVC)
        target_compile_options(vrc_pdb_dump PRIVATE /W3 /utf-8 /MP)
    endif()
    add_custom_command(TARGET vrc_pdb_dump POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VRC_DIA_SDK_DIR}/bin/amd64/msdia140.dll"
                "$<TARGET_FILE_DIR:vrc_pdb_dump>/msdia140.dll")
endif()

# ─── Install ───────────────────────────────────────────────────────────────────

install(TARGETS vr_converter vrc_injector vrc_big_walk_mod vrc_outlast_mod d3d11_proxy
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)
install(FILES profiles/default_vr.json profiles/big_walk.json profiles/big_walk_hands.json profiles/outlast_trials.json DESTINATION bin/profiles)
install(FILES outlast_verb_map.json outlast_verb_map.validated.json DESTINATION bin)
install(DIRECTORY tools/ DESTINATION bin/tools
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    PATTERN "*.exe" EXCLUDE
    PATTERN "*.obj" EXCLUDE
    PATTERN "*.pdb" EXCLUDE
)
