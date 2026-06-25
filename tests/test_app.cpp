// Minimal D3D11 test harness for VR Game Converter injection testing
// Renders a colorful rotating triangle + gradient background, presents each frame.
// Inject vr_converter.dll to see the scene in VR via blit_to_eye().

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static constexpr UINT WIDTH = 1280;
static constexpr UINT HEIGHT = 720;
static constexpr float CLEAR_COLOR[4] = { 0.05f, 0.1f, 0.15f, 1.0f };

struct TestApp {
    HWND hwnd = nullptr;
    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* cb = nullptr;

    float angle = 0.0f;
    bool running = true;
};

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_DESTROY || msg == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, w, l);
}

static bool create_window(TestApp& app) {
    WNDCLASS wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"VRC_TEST";
    if (!RegisterClass(&wc)) return false;

    RECT r = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    app.hwnd = CreateWindowEx(0, L"VRC_TEST", L"VR Converter Test",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    return app.hwnd != nullptr;
}

static bool init_d3d11(TestApp& app) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = WIDTH;
    scd.BufferDesc.Height = HEIGHT;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    scd.OutputWindow = app.hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_DRIVER_TYPE driver_types[] = {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP
    };
    HRESULT hr = E_FAIL;
    for (auto driver : driver_types) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            &scd, &app.swapchain,
            &app.device, nullptr, &app.ctx);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) return false;

    // Create RTV
    ID3D11Texture2D* backbuffer = nullptr;
    hr = app.swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (FAILED(hr)) return false;
    hr = app.device->CreateRenderTargetView(backbuffer, nullptr, &app.rtv);
    backbuffer->Release();
    return SUCCEEDED(hr);
}

static bool init_shaders(TestApp& app) {
    // Fullscreen triangle vertex shader (same structure as depth_reprojection HLSL)
    const char* vs_src = R"(
        struct VSOutput {
            float4 position : SV_POSITION;
            float2 uv : TEXCOORD0;
            float3 color : COLOR0;
        };
        VSOutput vs_main(uint vertex_id : SV_VertexID) {
            VSOutput o;
            float x = float(vertex_id & 1) * 2.0f - 1.0f;
            float y = float((vertex_id >> 1) & 1) * 2.0f - 1.0f;
            o.position = float4(x, -y, 0.0f, 1.0f);
            o.uv = float2((x + 1.0f) * 0.5f, (-y + 1.0f) * 0.5f);
            // Per-vertex color for a colorful triangle:
            // vertex 0 (top-left) = red-ish, vertex 1 (top-right) = green-ish, vertex 2 (bottom-left) = blue-ish
            float3 colors[3] = { float3(1,0.2,0.2), float3(0.2,1,0.2), float3(0.2,0.2,1) };
            o.color = colors[vertex_id];
            return o;
        }
    )";

    const char* ps_src = R"(
        struct VSOutput {
            float4 position : SV_POSITION;
            float2 uv : TEXCOORD0;
            float3 color : COLOR0;
        };
        float4 ps_main(VSOutput input) : SV_TARGET {
            // Pulsing colored triangle with time-based animation
            return float4(input.color, 1.0f);
        }
    )";

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(vs_src, strlen(vs_src), nullptr, nullptr, nullptr,
        "vs_main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &err);
    if (FAILED(hr)) {
        if (err) { printf("VS error: %s\n", (const char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    hr = app.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &app.vs);
    if (FAILED(hr)) { vs_blob->Release(); return false; }

    // Input layout: no vertex buffer needed (SV_VertexID)
    vs_blob->Release();

    ID3DBlob* ps_blob = nullptr;
    hr = D3DCompile(ps_src, strlen(ps_src), nullptr, nullptr, nullptr,
        "ps_main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, &err);
    if (FAILED(hr)) {
        if (err) { printf("PS error: %s\n", (const char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    hr = app.device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &app.ps);
    ps_blob->Release();
    return SUCCEEDED(hr);
}

static void render_frame(TestApp& app) {
    app.angle += 0.01f;
    if (app.angle > 6.283185f) app.angle -= 6.283185f;

    app.ctx->OMSetRenderTargets(1, &app.rtv, nullptr);

    // Animated clear color (slowly shifting)
    float color[4] = {
        CLEAR_COLOR[0] + sinf(app.angle * 0.3f) * 0.05f,
        CLEAR_COLOR[1] + cosf(app.angle * 0.2f) * 0.05f,
        CLEAR_COLOR[2] + sinf(app.angle * 0.5f) * 0.05f,
        1.0f
    };
    app.ctx->ClearRenderTargetView(app.rtv, color);

    D3D11_VIEWPORT vp = { 0, 0, (float)WIDTH, (float)HEIGHT, 0, 1 };
    app.ctx->RSSetViewports(1, &vp);

    // Draw fullscreen triangle with colorful shader
    app.ctx->VSSetShader(app.vs, nullptr, 0);
    app.ctx->PSSetShader(app.ps, nullptr, 0);
    app.ctx->IASetInputLayout(nullptr);
    app.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    app.ctx->Draw(3, 0);
}

static void cleanup(TestApp& app) {
    if (app.rtv) app.rtv->Release();
    if (app.vs) app.vs->Release();
    if (app.ps) app.ps->Release();
    if (app.layout) app.layout->Release();
    if (app.vb) app.vb->Release();
    if (app.cb) app.cb->Release();
    if (app.ctx) app.ctx->Release();
    if (app.swapchain) app.swapchain->Release();
    if (app.device) app.device->Release();
    if (app.hwnd) DestroyWindow(app.hwnd);
}

int main() {
    // Step 1: Load vr_converter.dll in-process to test DLL init
    HMODULE dll = LoadLibraryA("C:\\Users\\shiro\\vr-game-converter\\build\\vr_converter.dll");
    if (dll) {
        printf("DLL loaded successfully via LoadLibrary\n");
        // Call the exported entry point
        auto start = (void(*)())GetProcAddress(dll, "StartVRConverter");
        if (start) {
            printf("Calling StartVRConverter...\n");
            start();
            printf("StartVRConverter returned\n");
        } else {
            printf("StartVRConverter not found\n");
        }
    } else {
        DWORD err = GetLastError();
        printf("LoadLibrary failed with error %lu\n", err);
        // Continue running the app regardless
    }

    TestApp app;

    if (!create_window(app)) {
        printf("Failed to create window\n"); return 1;
    }
    if (!init_d3d11(app)) {
        printf("Failed to init D3D11\n"); return 1;
    }
    if (!init_shaders(app)) {
        printf("Failed to init shaders\n"); return 1;
    }

    ShowWindow(app.hwnd, SW_SHOW);

    printf("Test app running. PID: %d\n", GetCurrentProcessId());

    MSG msg = {};
    while (app.running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) app.running = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        render_frame(app);
        app.swapchain->Present(1, 0);
    }

    if (dll) FreeLibrary(dll);
    cleanup(app);
    return 0;
}
