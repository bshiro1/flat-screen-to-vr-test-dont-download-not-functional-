#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static constexpr UINT WIDTH = 640;
static constexpr UINT HEIGHT = 480;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_DESTROY || msg == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, w, l);
}

int main() {
    printf("===== D3D12 Test App =====\n");
    printf("PID: %d\n", GetCurrentProcessId());
    printf("Inject vr_converter.dll into PID %d to test hooks\n", GetCurrentProcessId());
    printf("Close the window to exit.\n");
    fflush(stdout);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"D3D12_TEST";
    if (!RegisterClassW(&wc)) { printf("RegisterClass failed\n"); return 1; }
    HWND hwnd = CreateWindowExW(0, L"D3D12_TEST", L"D3D12 Test",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        WIDTH, HEIGHT, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { printf("CreateWindow failed\n"); return 1; }

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { printf("CreateDXGIFactory4 failed: %08X\n", hr); return 1; }

    ID3D12Device* device = nullptr;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) { printf("D3D12CreateDevice failed: %08X\n", hr); factory->Release(); return 1; }
    printf("D3D12 device created OK\n");
    fflush(stdout);

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) { printf("CreateCommandQueue failed\n"); device->Release(); factory->Release(); return 1; }

    IDXGISwapChain1* sc1 = nullptr;
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = WIDTH;
    scd.Height = HEIGHT;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { printf("CreateSwapChainForHwnd failed: %08X\n", hr); queue->Release(); device->Release(); factory->Release(); return 1; }
    printf("Swap chain created OK\n");
    fflush(stdout);

    IDXGISwapChain* sc = sc1;
    ShowWindow(hwnd, SW_SHOW);

    MSG msg = {};
    UINT frames = 0;
    while (true) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        sc->Present(1, 0);
        frames++;
        if (frames % 120 == 0) {
            printf("Frame %u\n", frames);
            fflush(stdout);
        }
    }

cleanup:
    printf("D3D12 Test completed %u frames\n", frames);
    sc->Release();
    queue->Release();
    device->Release();
    factory->Release();
    DestroyWindow(hwnd);
    return 0;
}
