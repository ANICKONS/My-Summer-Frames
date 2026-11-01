// D3D11 writes a shared texture (created by D3D12), signals a shared fence; D3D12 waits, copies, reads back.
// Prints the first pixel each way: is the bridge's data path sound outside the game?
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
    ComPtr<ID3D11DeviceContext> c11;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED, &fl, 1, D3D11_SDK_VERSION, &d11,
                      nullptr, &c11);
    ComPtr<IDXGIDevice> dx;
    ComPtr<IDXGIAdapter> ad;
    d11.As(&dx);
    dx->GetAdapter(&ad);
    ComPtr<ID3D12Device> d12;
    D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12));
    ComPtr<ID3D11Device1> d111;
    ComPtr<ID3D11Device5> d115;
    ComPtr<ID3D11DeviceContext4> c4;
    d11.As(&d111);
    d11.As(&d115);
    c11.As(&c4);

    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = 64;
    rd.Height = 64;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> shared12;
    HRESULT hr = d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&shared12));
    HANDLE h = nullptr;
    d12->CreateSharedHandle(shared12.Get(), nullptr, GENERIC_ALL, nullptr, &h);
    ComPtr<ID3D11Texture2D> shared11;
    hr = d111->OpenSharedResource1(h, IID_PPV_ARGS(&shared11));
    printf("open texture 0x%08X\n", (unsigned)hr);
    ComPtr<ID3D12Fence> f12;
    d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f12));
    HANDLE fh = nullptr;
    d12->CreateSharedHandle(f12.Get(), nullptr, GENERIC_ALL, nullptr, &fh);
    ComPtr<ID3D11Fence> f11;
    hr = d115->OpenSharedFence(fh, IID_PPV_ARGS(&f11));
    printf("open fence 0x%08X\n", (unsigned)hr);

    // D3D11: a source texture with a known colour, copied into the shared one (as the bridge does).
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 64;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> src;
    d11->CreateTexture2D(&td, nullptr, &src);
    ComPtr<ID3D11RenderTargetView> rtv;
    d11->CreateRenderTargetView(src.Get(), nullptr, &rtv);
    float color[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    c11->ClearRenderTargetView(rtv.Get(), color);
    c11->CopyResource(shared11.Get(), src.Get());
    c4->Signal(f11.Get(), 1);
    c11->Flush();

    // D3D12: wait, copy to a readback buffer.
    D3D12_COMMAND_QUEUE_DESC qd = {};
    ComPtr<ID3D12CommandQueue> q;
    d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> l;
    d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&l));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total;
    d12->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rb = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buf;
    d12->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf));
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = buf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    srcl.pResource = shared12.Get();
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    l->Close();
    q->Wait(f12.Get(), 1);
    ID3D12CommandList* cl[] = {l.Get()};
    q->ExecuteCommandLists(1, cl);
    ComPtr<ID3D12Fence> done;
    d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
    q->Signal(done.Get(), 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    done->SetEventOnCompletion(1, ev);
    DWORD w = WaitForSingleObject(ev, 3000);
    unsigned char* p = nullptr;
    buf->Map(0, nullptr, reinterpret_cast<void**>(&p));
    printf("wait %s, pixel %u %u %u %u (expected 255 128 64 255)\n", w == WAIT_OBJECT_0 ? "ok" : "TIMEOUT", p[0], p[1], p[2], p[3]);
    return 0;
}
