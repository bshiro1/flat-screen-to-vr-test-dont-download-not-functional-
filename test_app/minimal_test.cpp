#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <MinHook.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

const char* MARKER_FILE = "C:\\Users\\shiro\\Desktop\\minimal_present_test.txt";

void mark(const char* msg) {
    FILE* f = fopen(MARKER_FILE, "a");
    if (f) { fprintf(f, "%llu: %s\n", GetTickCount64(), msg); fclose(f); }
}

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
PresentFn original_present = nullptr;

HRESULT __stdcall present_detour(IDXGISwapChain* sc, UINT si, UINT f) {
    mark("PRESENT_DETOUR_FIRED");
    return original_present(sc, si, f);
}

IDXGISwapChain* get_temp_swap_chain() {
    HINSTANCE inst = GetModuleHandleA(nullptr);
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = inst;
    wc.lpszClassName = "MinimalTest_Window";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, "MinimalTest_Window", "", WS_DISABLED,
                                0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hwnd) { mark("CreateWindow failed"); return nullptr; }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 1;
    scd.BufferDesc.Height = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd,
        &sc, &dev, nullptr, &ctx);

    if (FAILED(hr)) {
        char buf[128];
        sprintf_s(buf, "D3D11CreateDeviceAndSwapChain failed: 0x%08X", hr);
        mark(buf);
    }
    if (dev) dev->Release();
    if (ctx) ctx->Release();
    return sc;
}

extern "C" __declspec(dllexport) void StartMinimal() {
    mark("StartMinimal called");

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        mark("MH_Initialize failed");
        return;
    }

    IDXGISwapChain* temp_sc = get_temp_swap_chain();
    if (!temp_sc) { mark("FAILED: no temp swap chain"); return; }

    void* present_addr = (*reinterpret_cast<void***>(temp_sc))[8];
    char buf[256];
    sprintf_s(buf, "Present address = 0x%p", present_addr);
    mark(buf);

    if (MH_CreateHook(present_addr, &present_detour, (void**)&original_present) == MH_OK &&
        MH_EnableHook(present_addr) == MH_OK) {
        mark("Present HOOKED successfully");
    } else {
        mark("FAILED to hook Present");
    }

    temp_sc->Release();
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, [](LPVOID)->DWORD{ Sleep(100); StartMinimal(); return 0; }, nullptr, 0, nullptr);
    }
    return TRUE;
}
