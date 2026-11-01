// Does a D3D12 flip-model swap chain present from a second thread next to a D3D11 blt-model swap chain (the game)?
// Variants: bridge_test.exe <child-main|child-thread|same-window> <both|ours> [seconds]
//   child-main    child window created by the main thread, presented from the presenter thread
//   child-thread  child window created (and pumped) by the presenter thread
//   same-window   the D3D12 swap chain on the game window itself
//   both / ours   the D3D11 swap chain keeps presenting / stops once ours runs
// The main thread imitates Unity: renders + presents D3D11 at ~100 fps and pumps messages; every 2 s it also
// blocks for 300 ms (a long frame), and it resizes nothing. Prints per second how many frames each side presented.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
using Microsoft::WRL::ComPtr;

static std::atomic<int> ours{0}, game{0};
static std::atomic<bool> quit{false};
static std::atomic<HWND> child{nullptr};
static HWND mainWnd;
static int variant = 0; // 0 child-main, 1 child-thread, 2 same-window

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCHITTEST && h != mainWnd) return HTTRANSPARENT;
    if (m == WM_DESTROY && h == mainWnd) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

static HWND MakeChild(HWND parent)
{
    RECT rc;
    GetClientRect(parent, &rc);
    HWND c = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"BridgeTestChild", L"", WS_CHILD | WS_DISABLED | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                             rc.right, rc.bottom, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    printf("child %p on thread %lu\n", (void*)c, GetCurrentThreadId());
    return c;
}

static void Presenter(IDXGIAdapter* adapter)
{
    ComPtr<ID3D12Device> d;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d)))) { printf("no d3d12\n"); return; }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    ComPtr<ID3D12CommandQueue> q;
    d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    HWND target = nullptr;
    if (variant == 1) target = MakeChild(mainWnd);
    else if (variant == 2) target = mainWnd;
    else { while (!(target = child.load())) Sleep(1); }
    ComPtr<IDXGIFactory2> f;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 640;
    sd.Height = 360;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc;
    HRESULT hr = f->CreateSwapChainForHwnd(q.Get(), target, &sd, nullptr, nullptr, &sc);
    printf("d3d12 swap chain on %p: 0x%08X\n", (void*)target, (unsigned)hr);
    if (FAILED(hr)) { quit = true; return; }
    while (!quit)
    {
        if (variant == 1)
        {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        }
        hr = sc->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        if (FAILED(hr)) { printf("present 0x%08X\n", (unsigned)hr); break; }
        ours++;
        Sleep(4);
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "child-thread")) variant = 1;
    if (argc > 1 && !strcmp(argv[1], "same-window")) variant = 2;
    bool both = !(argc > 2 && !strcmp(argv[2], "ours"));
    int seconds = argc > 3 ? atoi(argv[3]) : 8;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BridgeTestMain";
    RegisterClassW(&wc);
    wc.lpszClassName = L"BridgeTestChild";
    RegisterClassW(&wc);
    mainWnd = CreateWindowW(L"BridgeTestMain", L"bridge test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 656, 399, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 360;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = mainWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED, nullptr, 0,
                                  D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    ComPtr<IDXGIDevice> dx;
    ComPtr<IDXGIAdapter> ad;
    dev.As(&dx);
    dx->GetAdapter(&ad);
    if (variant == 0) child = MakeChild(mainWnd);
    std::thread t(Presenter, ad.Get());
    DWORD start = GetTickCount(), lastPrint = start, lastBlock = start;
    while (!quit && GetTickCount() - start < (DWORD)seconds * 1000)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        if (both || ours.load() < 20)
        {
            sc->Present(0, 0);
            game++;
        }
        Sleep(10);
        if (GetTickCount() - lastBlock > 2000) { Sleep(300); lastBlock = GetTickCount(); }
        if (GetTickCount() - lastPrint >= 1000)
        {
            printf("t=%lus ours %d game %d\n", (GetTickCount() - start) / 1000, ours.exchange(0), game.exchange(0));
            lastPrint = GetTickCount();
        }
    }
    quit = true;
    DWORD w = WaitForSingleObject(t.native_handle(), 3000);
    printf("presenter thread %s\n", w == WAIT_OBJECT_0 ? "ended" : "STUCK");
    if (w == WAIT_OBJECT_0) t.join(); else t.detach();
    return 0;
}
