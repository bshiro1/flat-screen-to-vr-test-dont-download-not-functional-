#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// Simple D3D12 test app that renders frames in a loop
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Create window
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_OWNDC, DefWindowProcA, 0, 0,
                       GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
                       "VRC_TestD3D12", nullptr };
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, "VRC_TestD3D12", "Test Window", WS_OVERLAPPEDWINDOW,
                                100, 100, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    FILE* f = fopen("C:\\Users\\shiro\\Desktop\\test_app_log.txt", "w");
    fprintf(f, "Starting D3D12 test app\n");
    fflush(f);

    // Enable debug layer
    ID3D12Debug* debug = nullptr;
    D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (debug) { debug->EnableDebugLayer(); debug->Release(); }

    // Create D3D12 device
    ID3D12Device* device = nullptr;
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (!device) {
        fprintf(f, "Failed to create device\n");
        fclose(f);
        return 1;
    }
    fprintf(f, "Device created\n");
    fflush(f);

    // Create command queue
    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue));
    fprintf(f, "Queue created\n");
    fflush(f);

    // Create swap chain
    IDXGIFactory4* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 800;
    scd.BufferDesc.Height = 600;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* swap_chain = nullptr;
    factory->CreateSwapChain(queue, &scd, &swap_chain);
    fprintf(f, "Swap chain created\n");
    fflush(f);

    // Main loop - render frames
    MSG msg = {};
    int frames = 0;
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        // Just call Present (we don't need to submit real work for testing)
        swap_chain->Present(1, 0);
        frames++;
        if (frames % 60 == 0) {
            fprintf(f, "Frame %d\n", frames);
            fflush(f);
        }

        Sleep(1); // limit to ~1000 FPS
    }

cleanup:
    swap_chain->Release();
    queue->Release();
    device->Release();
    factory->Release();
    fclose(f);
    return 0;
}
