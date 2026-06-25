// Minimal D3D11 proxy DLL.
// NOT auto-generated -- this is manually maintained.
//
// Deployment:
//   Copy this as d3d11.dll into the game directory, alongside
//   d3d11_orig.dll (renamed copy of the real system d3d11.dll)
//   and vr_converter.dll.
//
// Responsibilities:
//   1. Load d3d11_orig.dll and forward ALL exports to it.
//   2. Load vr_converter.dll which installs hooks via MinHook.
//
// vr_converter.dll handles all hooking (D3D11CreateDevice via
// MinHook, IDXGISwapChain::Present via MinHook on vtable[8]).
// This proxy does NOT patch any vtables itself -- that would
// conflict with MinHook.
//
// Exports are handled via d3d11_proxy.def -- the functions here
// are plain definitions (no __declspec(dllexport)) to avoid
// conflicting with system header declarations.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

static HMODULE g_orig_dll = nullptr;
static HMODULE g_vrc_dll = nullptr;
static HMODULE g_this_dll = nullptr;

static void log(const char* msg) {
    OutputDebugStringA(msg);
}

static bool load_orig() {
    if (g_orig_dll) return true;

    char path[MAX_PATH];
    GetModuleFileNameA(g_this_dll, path, sizeof(path));
    char* last_slash = path;
    for (char* p = path; *p; p++) if (*p == '\\') last_slash = p + 1;
    const char* name = "d3d11_orig.dll";
    size_t i = 0;
    while (name[i]) { last_slash[i] = name[i]; i++; }
    last_slash[i] = 0;

    g_orig_dll = LoadLibraryA(path);
    if (!g_orig_dll) {
        log("VRC_PROXY: FAILED to load d3d11_orig.dll\n");
        return false;
    }
    log("VRC_PROXY: Loaded d3d11_orig.dll\n");
    return true;
}

static bool load_vrc() {
    if (g_vrc_dll) return true;

    char path[MAX_PATH];
    GetModuleFileNameA(g_this_dll, path, sizeof(path));
    char* last_slash = path;
    for (char* p = path; *p; p++) if (*p == '\\') last_slash = p + 1;
    const char* name = "vr_converter.dll";
    size_t i = 0;
    while (name[i]) { last_slash[i] = name[i]; i++; }
    last_slash[i] = 0;

    g_vrc_dll = LoadLibraryA(path);
    if (!g_vrc_dll) {
        log("VRC_PROXY: FAILED to load vr_converter.dll\n");
        return false;
    }
    log("VRC_PROXY: Loaded vr_converter.dll\n");
    return true;
}

static FARPROC get_proc(const char* name) {
    if (!load_orig()) return nullptr;
    return GetProcAddress(g_orig_dll, name);
}

// --- Intercepted exports (defined here, exported via .def) ---

extern "C" HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext)
{
    auto real = (decltype(&D3D11CreateDevice))get_proc("D3D11CreateDevice");
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
    return real(pAdapter, DriverType, Software, Flags, pFeatureLevels,
        FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
        ppDevice, pFeatureLevel, ppImmediateContext);
}

// --- Forwarded exports (delegated to d3d11_orig.dll via .def) ---

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

extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(void* a) {
    auto real = (decltype(&CreateDirect3D11DeviceFromDXGIDevice))get_proc(
        "CreateDirect3D11DeviceFromDXGIDevice");
    return real(a);
}

extern "C" HRESULT WINAPI CreateDirect3D11SurfaceFromDXGISurface(void* a) {
    auto real = (decltype(&CreateDirect3D11SurfaceFromDXGISurface))get_proc(
        "CreateDirect3D11SurfaceFromDXGISurface");
    return real(a);
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

extern "C" HRESULT WINAPI D3D11On12CreateDevice(void* a) {
    auto real = (decltype(&D3D11On12CreateDevice))get_proc(
        "D3D11On12CreateDevice");
    return real(a);
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

BOOL APIENTRY DllMain(HMODULE h_module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h_module);
        g_this_dll = h_module;
        log("VRC_PROXY: DllMain attach\n");
        load_orig();
        load_vrc();
    }
    if (reason == DLL_PROCESS_DETACH && !reserved) {
        if (g_vrc_dll) { FreeLibrary(g_vrc_dll); g_vrc_dll = nullptr; }
        if (g_orig_dll) { FreeLibrary(g_orig_dll); g_orig_dll = nullptr; }
    }
    return TRUE;
}
