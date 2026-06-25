cmake_minimum_required(VERSION 3.20)
project(vr-game-converter VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS is set per-target below

# ─── Dependencies ──────────────────────────────────────────────────────────────

# MinHook - lightweight hooking library
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG master
)
set(MINHOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MINHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(minhook)

# Dear ImGui - runtime overlay UI
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# ImGui has no CMakeLists.txt; build sources into our target
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
set(IMGUI_BACKEND_SOURCES
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

# OpenXR SDK - VR runtime interface
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

# Windows SDK / DirectX
set(CMAKE_REQUIRED_LIBRARIES d3d11.lib d3d12.lib dxgi.lib dxguid.lib
    d3dcompiler.lib windowsapp.lib)

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
    src/render/camera_rig.cpp
    src/render/latency_compensator.cpp
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
set_target_properties(vr_converter PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(vr_converter PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${minhook_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${openxr_loader_SOURCE_DIR}/include
    ${nlohmann_json_SOURCE_DIR}/include
)

# DirectX 12 headers needed for ImGui D3D12 backend
target_include_directories(vr_converter PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vr_converter PRIVATE
    minhook
    openxr_loader
    nlohmann_json::nlohmann_json
    d3d11.lib
    d3d12.lib
    dxgi.lib
    dxguid.lib
    d3dcompiler.lib
    windowsapp.lib
)

target_compile_definitions(vr_converter PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
    XR_USE_GRAPHICS_API_D3D11
    XR_USE_GRAPHICS_API_D3D12
)

if(MSVC)
    target_compile_options(vr_converter PRIVATE /W4 /utf-8 /MP)
endif()

# ─── D3D11 Proxy DLL ────────────────────────────────────────────────────────────
# Replace d3d11.dll in game directory — works with all games (no remote thread,
# no code modification, uses vtable patching for Present hook).

set(D3D11_PROXY_SOURCES
    src/proxy/d3d11_proxy.cpp
)
add_library(d3d11_proxy SHARED ${D3D11_PROXY_SOURCES} src/proxy/d3d11_proxy.def)

# Sources are manually maintained — NOT auto-generated.
# See notes in src/proxy/d3d11_proxy.cpp.
target_include_directories(d3d11_proxy PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(d3d11_proxy PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(d3d11_proxy PRIVATE d3d11.lib dxgi.lib)
set_target_properties(d3d11_proxy PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
set_property(TARGET d3d11_proxy PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(d3d11_proxy PRIVATE /W4 /utf-8 /GS-)
endif()

install(TARGETS d3d11_proxy RUNTIME DESTINATION bin)

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

# ─── Install ───────────────────────────────────────────────────────────────────

install(TARGETS vr_converter
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)
install(FILES profiles/default_vr.json DESTINATION bin/profiles)
install(DIRECTORY tools/ DESTINATION bin/tools)
