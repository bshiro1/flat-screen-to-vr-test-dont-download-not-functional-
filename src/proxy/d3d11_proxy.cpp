// D3D11 proxy DLL (manually maintained).
//
// Deployment: copy this DLL, named d3d11.dll, and vr_converter.dll into the
// game folder next to the game executable. The game then loads this proxy
// instead of the system d3d11.dll.
//
// Responsibilities:
//   1. Forward every d3d11.dll export to the real d3d11.dll from the system
//      directory, loaded on the first forwarded call.
//   2. Load vr_converter.dll from this DLL's folder on a thread started in
//      DllMain, because LoadLibrary must not run under the loader lock.
//      vr_converter.dll installs all hooks itself; this proxy patches nothing.
//
// While this proxy is loaded as d3d11.dll, vr_converter.dll's own d3d11.dll
// imports resolve to it too. That works because every call is forwarded.
//
// Exports are listed in d3d11_proxy.def; the functions here are plain
// definitions (no __declspec(dllexport)) to avoid conflicting with the system
// header declarations.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cwchar>

static HMODULE g_this_dll = nullptr;
static HMODULE g_real_d3d11 = nullptr;
static INIT_ONCE g_real_d3d11_once = INIT_ONCE_STATIC_INIT;

static void debug_log(const wchar_t* msg) {
    OutputDebugStringW(msg);
}

static BOOL CALLBACK load_real_d3d11(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    const UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH || wcscat_s(path, L"\\d3d11.dll") != 0) {
        debug_log(L"VRC_PROXY: could not build the system d3d11.dll path\n");
        return TRUE;
    }
    g_real_d3d11 = LoadLibraryW(path);
    debug_log(g_real_d3d11 ? L"VRC_PROXY: loaded system d3d11.dll\n"
                           : L"VRC_PROXY: FAILED to load system d3d11.dll\n");
    return TRUE;
}

static FARPROC get_proc(const char* name) {
    InitOnceExecuteOnce(&g_real_d3d11_once, load_real_d3d11, nullptr, nullptr);
    return g_real_d3d11 ? GetProcAddress(g_real_d3d11, name) : nullptr;
}

// Runs after DllMain returns and the loader lock is released.
static DWORD WINAPI load_converter_thread(LPVOID) {
    wchar_t path[1024];
    const DWORD len = GetModuleFileNameW(g_this_dll, path, ARRAYSIZE(path));
    wchar_t* slash = (len > 0 && len < ARRAYSIZE(path)) ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) {
        debug_log(L"VRC_PROXY: could not determine the proxy folder\n");
        return 1;
    }
    slash[1] = L'\0';
    if (wcscat_s(path, L"vr_converter.dll") != 0) {
        debug_log(L"VRC_PROXY: vr_converter.dll path too long\n");
        return 1;
    }
    if (!LoadLibraryW(path)) {
        debug_log(L"VRC_PROXY: FAILED to load vr_converter.dll\n");
        return 1;
    }
    debug_log(L"VRC_PROXY: loaded vr_converter.dll\n");
    return 0;
}

// --- Intercepted exports (defined here, exported via .def) ---

extern "C" HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext)
{
    auto real = (decltype(&D3D11CreateDevice))get_proc("D3D11CreateDevice");
    if (!real) return E_FAIL;
    return real(pAdapter, DriverType, Software, Flags, pFeatureLevels,
        FeatureLevels, SDKVersion, ppDevice, pFeatureLevel,
        ppImmediateContext);
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext)
{
    auto real = (decltype(&D3D11CreateDeviceAndSwapChain))get_proc(
        "D3D11CreateDeviceAndSwapChain");
    if (!real) return E_FAIL;
    return real(pAdapter, DriverType, Software, Flags, pFeatureLevels,
        FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
        ppDevice, pFeatureLevel, ppImmediateContext);
}

// --- Forwarded exports (delegated to the system d3d11.dll) ---

extern "C" void* D3D11CreateDeviceForD3D12() {
    auto real = (decltype(&D3D11CreateDeviceForD3D12))get_proc(
        "D3D11CreateDeviceForD3D12");
    return real ? real() : nullptr;
}

extern "C" long WINAPI D3DKMTCloseAdapter(void* a) {
    auto real = (decltype(&D3DKMTCloseAdapter))get_proc("D3DKMTCloseAdapter");
    return real(a);
}

extern "C" long WINAPI D3DKMTDestroyAllocation(void* a) {
    auto real = (decltype(&D3DKMTDestroyAllocation))get_proc(
        "D3DKMTDestroyAllocation");
    return real(a);
}

extern "C" long WINAPI D3DKMTDestroyContext(void* a) {
    auto real = (decltype(&D3DKMTDestroyContext))get_proc(
        "D3DKMTDestroyContext");
    return real(a);
}

extern "C" long WINAPI D3DKMTDestroyDevice(void* a) {
    auto real = (decltype(&D3DKMTDestroyDevice))get_proc(
        "D3DKMTDestroyDevice");
    return real(a);
}

extern "C" long WINAPI D3DKMTDestroySynchronizationObject(void* a) {
    auto real = (decltype(&D3DKMTDestroySynchronizationObject))get_proc(
        "D3DKMTDestroySynchronizationObject");
    return real(a);
}

extern "C" long WINAPI D3DKMTPresent(void* a) {
    auto real = (decltype(&D3DKMTPresent))get_proc("D3DKMTPresent");
    return real(a);
}

extern "C" long WINAPI D3DKMTQueryAdapterInfo(void* a) {
    auto real = (decltype(&D3DKMTQueryAdapterInfo))get_proc(
        "D3DKMTQueryAdapterInfo");
    return real(a);
}

extern "C" long WINAPI D3DKMTSetDisplayPrivateDriverFormat(void* a) {
    auto real = (decltype(&D3DKMTSetDisplayPrivateDriverFormat))get_proc(
        "D3DKMTSetDisplayPrivateDriverFormat");
    return real(a);
}

extern "C" long WINAPI D3DKMTSignalSynchronizationObject(void* a) {
    auto real = (decltype(&D3DKMTSignalSynchronizationObject))get_proc(
        "D3DKMTSignalSynchronizationObject");
    return real(a);
}

extern "C" long WINAPI D3DKMTUnlock(void* a) {
    auto real = (decltype(&D3DKMTUnlock))get_proc("D3DKMTUnlock");
    return real(a);
}

extern "C" long WINAPI D3DKMTWaitForSynchronizationObject(void* a) {
    auto real = (decltype(&D3DKMTWaitForSynchronizationObject))get_proc(
        "D3DKMTWaitForSynchronizationObject");
    return real(a);
}

extern "C" HRESULT WINAPI EnableFeatureLevelUpgrade(void* a) {
    auto real = (decltype(&EnableFeatureLevelUpgrade))get_proc(
        "EnableFeatureLevelUpgrade");
    return real(a);
}

extern "C" HRESULT WINAPI OpenAdapter10(void* a) {
    auto real = (decltype(&OpenAdapter10))get_proc("OpenAdapter10");
    return real(a);
}

extern "C" HRESULT WINAPI OpenAdapter10_2(void* a) {
    auto real = (decltype(&OpenAdapter10_2))get_proc("OpenAdapter10_2");
    return real(a);
}

// Real signature: (IDXGIDevice*, IInspectable**). void** is ABI-identical and
// avoids pulling the WinRT interop headers into this lightweight proxy.
extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(
    void* dxgi_device, void** graphics_device)
{
    using Fn = HRESULT(WINAPI*)(void*, void**);
    auto real = (Fn)get_proc("CreateDirect3D11DeviceFromDXGIDevice");
    return real ? real(dxgi_device, graphics_device) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDirect3D11SurfaceFromDXGISurface(
    void* dxgi_surface, void** graphics_surface)
{
    using Fn = HRESULT(WINAPI*)(void*, void**);
    auto real = (Fn)get_proc("CreateDirect3D11SurfaceFromDXGISurface");
    return real ? real(dxgi_surface, graphics_surface) : E_FAIL;
}

extern "C" HRESULT WINAPI D3D11CoreCreateDevice(void* a) {
    auto real = (decltype(&D3D11CoreCreateDevice))get_proc(
        "D3D11CoreCreateDevice");
    return real(a);
}

extern "C" HRESULT WINAPI D3D11CoreCreateLayeredDevice(void* a) {
    auto real = (decltype(&D3D11CoreCreateLayeredDevice))get_proc(
        "D3D11CoreCreateLayeredDevice");
    return real(a);
}

extern "C" HRESULT WINAPI D3D11CoreGetLayeredDeviceSize(void* a) {
    auto real = (decltype(&D3D11CoreGetLayeredDeviceSize))get_proc(
        "D3D11CoreGetLayeredDeviceSize");
    return real(a);
}

extern "C" HRESULT WINAPI D3D11CoreRegisterLayers(void* a) {
    auto real = (decltype(&D3D11CoreRegisterLayers))get_proc(
        "D3D11CoreRegisterLayers");
    return real(a);
}

// Full 10-argument export (d3d11on12.h). The old one-argument stub silently
// dropped arguments 2-10 and broke the D3D12 -> D3D11On12 stereo path whenever
// this proxy DLL was deployed.
extern "C" HRESULT WINAPI D3D11On12CreateDevice(
    void* device, UINT flags, const D3D_FEATURE_LEVEL* feature_levels,
    UINT num_feature_levels, void* const* command_queues, UINT num_queues,
    UINT node_mask, ID3D11Device** out_device,
    ID3D11DeviceContext** out_context, D3D_FEATURE_LEVEL* chosen_level)
{
    using Fn = HRESULT(WINAPI*)(void*, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                void* const*, UINT, UINT, ID3D11Device**,
                                ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);
    auto real = (Fn)get_proc("D3D11On12CreateDevice");
    if (!real) return E_FAIL;
    return real(device, flags, feature_levels, num_feature_levels,
                command_queues, num_queues, node_mask, out_device,
                out_context, chosen_level);
}

extern "C" long WINAPI D3DKMTCreateAllocation(void* a) {
    auto real = (decltype(&D3DKMTCreateAllocation))get_proc(
        "D3DKMTCreateAllocation");
    return real(a);
}

extern "C" long WINAPI D3DKMTCreateContext(void* a) {
    auto real = (decltype(&D3DKMTCreateContext))get_proc(
        "D3DKMTCreateContext");
    return real(a);
}

extern "C" long WINAPI D3DKMTCreateDevice(void* a) {
    auto real = (decltype(&D3DKMTCreateDevice))get_proc(
        "D3DKMTCreateDevice");
    return real(a);
}

extern "C" long WINAPI D3DKMTCreateSynchronizationObject(void* a) {
    auto real = (decltype(&D3DKMTCreateSynchronizationObject))get_proc(
        "D3DKMTCreateSynchronizationObject");
    return real(a);
}

extern "C" long WINAPI D3DKMTEscape(void* a) {
    auto real = (decltype(&D3DKMTEscape))get_proc("D3DKMTEscape");
    return real(a);
}

extern "C" long WINAPI D3DKMTGetContextSchedulingPriority(void* a) {
    auto real = (decltype(&D3DKMTGetContextSchedulingPriority))get_proc(
        "D3DKMTGetContextSchedulingPriority");
    return real(a);
}

extern "C" long WINAPI D3DKMTGetDeviceState(void* a) {
    auto real = (decltype(&D3DKMTGetDeviceState))get_proc(
        "D3DKMTGetDeviceState");
    return real(a);
}

extern "C" long WINAPI D3DKMTGetDisplayModeList(void* a) {
    auto real = (decltype(&D3DKMTGetDisplayModeList))get_proc(
        "D3DKMTGetDisplayModeList");
    return real(a);
}

extern "C" long WINAPI D3DKMTGetMultisampleMethodList(void* a) {
    auto real = (decltype(&D3DKMTGetMultisampleMethodList))get_proc(
        "D3DKMTGetMultisampleMethodList");
    return real(a);
}

extern "C" long WINAPI D3DKMTGetRuntimeData(void* a) {
    auto real = (decltype(&D3DKMTGetRuntimeData))get_proc(
        "D3DKMTGetRuntimeData");
    return real(a);
}

extern "C" long WINAPI D3DKMTGetSharedPrimaryHandle(void* a) {
    auto real = (decltype(&D3DKMTGetSharedPrimaryHandle))get_proc(
        "D3DKMTGetSharedPrimaryHandle");
    return real(a);
}

extern "C" long WINAPI D3DKMTLock(void* a) {
    auto real = (decltype(&D3DKMTLock))get_proc("D3DKMTLock");
    return real(a);
}

extern "C" long WINAPI D3DKMTOpenAdapterFromHdc(void* a) {
    auto real = (decltype(&D3DKMTOpenAdapterFromHdc))get_proc(
        "D3DKMTOpenAdapterFromHdc");
    return real(a);
}

extern "C" long WINAPI D3DKMTOpenResource(void* a) {
    auto real = (decltype(&D3DKMTOpenResource))get_proc(
        "D3DKMTOpenResource");
    return real(a);
}

extern "C" long WINAPI D3DKMTQueryAllocationResidency(void* a) {
    auto real = (decltype(&D3DKMTQueryAllocationResidency))get_proc(
        "D3DKMTQueryAllocationResidency");
    return real(a);
}

extern "C" long WINAPI D3DKMTQueryResourceInfo(void* a) {
    auto real = (decltype(&D3DKMTQueryResourceInfo))get_proc(
        "D3DKMTQueryResourceInfo");
    return real(a);
}

extern "C" long WINAPI D3DKMTRender(void* a) {
    auto real = (decltype(&D3DKMTRender))get_proc("D3DKMTRender");
    return real(a);
}

extern "C" long WINAPI D3DKMTSetAllocationPriority(void* a) {
    auto real = (decltype(&D3DKMTSetAllocationPriority))get_proc(
        "D3DKMTSetAllocationPriority");
    return real(a);
}

extern "C" long WINAPI D3DKMTSetContextSchedulingPriority(void* a) {
    auto real = (decltype(&D3DKMTSetContextSchedulingPriority))get_proc(
        "D3DKMTSetContextSchedulingPriority");
    return real(a);
}

extern "C" long WINAPI D3DKMTSetDisplayMode(void* a) {
    auto real = (decltype(&D3DKMTSetDisplayMode))get_proc(
        "D3DKMTSetDisplayMode");
    return real(a);
}

extern "C" long WINAPI D3DKMTSetGammaRamp(void* a) {
    auto real = (decltype(&D3DKMTSetGammaRamp))get_proc(
        "D3DKMTSetGammaRamp");
    return real(a);
}

extern "C" long WINAPI D3DKMTSetVidPnSourceOwner(void* a) {
    auto real = (decltype(&D3DKMTSetVidPnSourceOwner))get_proc(
        "D3DKMTSetVidPnSourceOwner");
    return real(a);
}

extern "C" long WINAPI D3DKMTWaitForVerticalBlankEvent(void* a) {
    auto real = (decltype(&D3DKMTWaitForVerticalBlankEvent))get_proc(
        "D3DKMTWaitForVerticalBlankEvent");
    return real(a);
}

extern "C" int WINAPI D3DPerformance_BeginEvent(void* a) {
    auto real = (decltype(&D3DPerformance_BeginEvent))get_proc(
        "D3DPerformance_BeginEvent");
    return real(a);
}

extern "C" int WINAPI D3DPerformance_EndEvent(void* a) {
    auto real = (decltype(&D3DPerformance_EndEvent))get_proc(
        "D3DPerformance_EndEvent");
    return real(a);
}

extern "C" int WINAPI D3DPerformance_GetStatus() {
    auto real = (decltype(&D3DPerformance_GetStatus))get_proc(
        "D3DPerformance_GetStatus");
    return real();
}

extern "C" int WINAPI D3DPerformance_SetMarker(void* a) {
    auto real = (decltype(&D3DPerformance_SetMarker))get_proc(
        "D3DPerformance_SetMarker");
    return real(a);
}

BOOL APIENTRY DllMain(HMODULE h_module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h_module);
        g_this_dll = h_module;
        // Pin the proxy: the loader thread below runs its code, and
        // vr_converter.dll's d3d11 imports point into it.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&load_converter_thread), &pinned);
        HANDLE thread = CreateThread(nullptr, 0, load_converter_thread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            debug_log(L"VRC_PROXY: could not start the vr_converter.dll loader thread\n");
        }
    }
    // DLL_PROCESS_DETACH does nothing: both DLLs are pinned, and FreeLibrary
    // must not be called under the loader lock.
    return TRUE;
}
