#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Minimal D3D11 app that creates a swap chain and calls Present in a loop.
// Used to test our VR Converter DLL injection and hooking.

const char* LOG_PATH = "C:\\Users\\shiro\\Desktop\\test_app_output.txt";
FILE* g_log = nullptr;

static void log_msg(const char* msg) {
    if (!g_log) {
        g_log = fopen(LOG_PATH, "w");
    }
    if (g_log) {
        fprintf(g_log, "[%llu] %s\n", GetTickCount64(), msg);
        fflush(g_log);
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcA(hwnd, msg, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    log_msg("Test app starting");

    log_msg("No delay - creating device immediately");

    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "VRC_TestApp";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, "VRC_TestApp", "VRC Test", WS_OVERLAPPEDWINDOW,
                                 100, 100, 640, 480, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    log_msg("Window created");

    // Create D3D11 device + swap chain
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width = 640;
    scd.BufferDesc.Height = 480;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    D3D_FEATURE_LEVEL level;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &scd,
        &swap, &device, &level, &ctx);

    if (FAILED(hr) || !swap || !device) {
        char buf[256];
        sprintf_s(buf, "D3D11CreateDeviceAndSwapChain failed: 0x%08X", hr);
        log_msg(buf);
        return 1;
    }
    log_msg("D3D11 device + swap chain created");

    MSG msg = {};
    int frames = 0;
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        swap->Present(0, 0);
        frames++;
        if (frames % 100 == 0) {
            char buf[64];
            sprintf_s(buf, "Frame %d", frames);
            log_msg(buf);
        }

        Sleep(1);
    }

cleanup:
    log_msg("Test app exiting");
    swap->Release();
    ctx->Release();
    device->Release();
    if (g_log) fclose(g_log);
    return 0;
}
