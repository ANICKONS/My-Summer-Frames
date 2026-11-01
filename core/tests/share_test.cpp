// Which texture formats can a D3D12 resource share with a D3D11 device (the bridge's depth / motion)?
// cl /nologo /EHsc share_test.cpp d3d11.lib d3d12.lib dxgi.lib
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;

int main()
{
    ComPtr<ID3D11Device> d11;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED, &fl, 1,
                                 D3D11_SDK_VERSION, &d11, nullptr, nullptr))) return 1;
    D3D11_FEATURE_DATA_D3D11_OPTIONS o = {};
    d11->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o));
    printf("ExtendedResourceSharing %d\n", o.ExtendedResourceSharing);
    ComPtr<IDXGIDevice> dx;
    ComPtr<IDXGIAdapter> ad;
    d11.As(&dx);
    dx->GetAdapter(&ad);
    ComPtr<ID3D12Device> d12;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12)))) return 2;
    ComPtr<ID3D11Device1> d111;
    d11.As(&d111);
    DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_TYPELESS,
                             DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM};
    const char* names[] = {"R8G8B8A8_UNORM", "R32_FLOAT", "R16G16_FLOAT", "R32_TYPELESS", "R16G16B16A16_FLOAT", "R32G32_FLOAT", "R10G10B10A2_UNORM"};
    D3D12_RESOURCE_FLAGS flags[] = {D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS};
    const char* fnames[] = {"none", "rt", "uav", "rt+simul"};
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 4; j++)
        {
            D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = 1280;
            rd.Height = 720;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = formats[i];
            rd.SampleDesc.Count = 1;
            rd.Flags = flags[j];
            ComPtr<ID3D12Resource> r;
            HRESULT h1 = d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
            HANDLE h = nullptr;
            HRESULT h2 = SUCCEEDED(h1) ? d12->CreateSharedHandle(r.Get(), nullptr, GENERIC_ALL, nullptr, &h) : E_FAIL;
            ComPtr<ID3D11Texture2D> t;
            HRESULT h3 = SUCCEEDED(h2) ? d111->OpenSharedResource1(h, IID_PPV_ARGS(&t)) : E_FAIL;
            if (h) CloseHandle(h);
            D3D11_TEXTURE2D_DESC td = {};
            if (t) t->GetDesc(&td);
            printf("%-20s %-9s create %08X handle %08X open %08X bind 0x%X\n", names[i], fnames[j], (unsigned)h1, (unsigned)h2, (unsigned)h3, td.BindFlags);
        }
    return 0;
}
