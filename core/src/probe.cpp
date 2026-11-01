// My Summer Frames core: what Unity has bound when our events run (for the camera integration).
// Copyright (C) 2026 ANICKON
#include "common.h"

namespace msf
{
    std::string FormatName(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
        case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
        default: return "fmt" + std::to_string(static_cast<int>(f));
        }
    }

    void DescribeResource(const char* tag, IUnknown* resource)
    {
        if (!resource)
        {
            Log("%s: null", tag);
            return;
        }
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&tex))))
        {
            Log("%s: %p is not a Texture2D", tag, static_cast<void*>(resource));
            return;
        }
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        Log("%s: %p %ux%u %s msaa=%u mips=%u array=%u bind=0x%X misc=0x%X usage=%u", tag, static_cast<void*>(tex.Get()),
            d.Width, d.Height, FormatName(d.Format).c_str(), d.SampleDesc.Count, d.MipLevels, d.ArraySize, d.BindFlags,
            d.MiscFlags, static_cast<unsigned>(d.Usage));
    }

    void ProbeBoundTargets(const char* tag)
    {
        ID3D11Device* device = G().device11;
        if (!device) return;
        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        char name[64];
        int bound = 0;
        for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
        {
            if (!rtvs[i]) continue;
            bound++;
            ComPtr<ID3D11Resource> res;
            rtvs[i]->GetResource(&res);
            D3D11_RENDER_TARGET_VIEW_DESC vd;
            rtvs[i]->GetDesc(&vd);
            snprintf(name, sizeof(name), "%s rt%d (view %s)", tag, i, FormatName(vd.Format).c_str());
            DescribeResource(name, res.Get());
            rtvs[i]->Release();
        }
        if (dsv)
        {
            ComPtr<ID3D11Resource> res;
            dsv->GetResource(&res);
            D3D11_DEPTH_STENCIL_VIEW_DESC vd;
            dsv->GetDesc(&vd);
            snprintf(name, sizeof(name), "%s depth (view %s, dim %d)", tag, FormatName(vd.Format).c_str(),
                     static_cast<int>(vd.ViewDimension));
            DescribeResource(name, res.Get());
            dsv->Release();
        }
        else Log("%s depth: none", tag);
        if (bound == 0) Log("%s: no render target bound", tag);
        D3D11_VIEWPORT vp[1];
        UINT n = 1;
        ctx->RSGetViewports(&n, vp);
        if (n) Log("%s viewport: %.0f,%.0f %.0fx%.0f depth %.2f..%.2f", tag, vp[0].TopLeftX, vp[0].TopLeftY, vp[0].Width,
                   vp[0].Height, vp[0].MinDepth, vp[0].MaxDepth);
    }
}
