// My Summer Frames core: NGX start on Unity's D3D11 device (DLSS / DLAA) and on our own D3D12 device on the same
// adapter (DLSS Frame Generation exists for D3D12 and Vulkan only).
// Copyright (C) 2026 ANICKON
#include "common.h"

namespace msf
{
    namespace
    {
        // NGX needs a GUID-like project id for NVSDK_NGX_ENGINE_TYPE_CUSTOM.
        constexpr const char* kProjectId = "8e3c1f5a-6b2d-4f0e-9a71-3c5d2b8e4f17";
        constexpr const char* kEngineVersion = "Unity 5.0.0f4";

        bool ngx11 = false, ngx12 = false;
        ComPtr<ID3D12Device> device12;
        NVSDK_NGX_Parameter* caps11 = nullptr;
        NVSDK_NGX_Parameter* caps12 = nullptr;

        // Called from any NGX thread; Log is thread-safe.
        void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
        {
            std::string text(message ? message : "");
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            Log("NGX: %s", text.c_str());
        }

        int GetI(NVSDK_NGX_Parameter* p, const char* name, int fallback)
        {
            int v = fallback;
            if (!p || NVSDK_NGX_FAILED(p->Get(name, &v))) return fallback;
            return v;
        }

        unsigned GetU(NVSDK_NGX_Parameter* p, const char* name, unsigned fallback)
        {
            unsigned v = fallback;
            if (!p || NVSDK_NGX_FAILED(p->Get(name, &v))) return fallback;
            return v;
        }

        NVSDK_NGX_FeatureCommonInfo CommonInfo(const wchar_t* const* paths)
        {
            NVSDK_NGX_FeatureCommonInfo info = {};
            info.PathListInfo.Path = paths;
            info.PathListInfo.Length = paths[0] && paths[0][0] ? 1u : 0u;
            info.LoggingInfo.LoggingCallback = NgxLog;
            info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
            info.LoggingInfo.DisableOtherLoggingSinks = true;
            return info;
        }

        bool AdapterOf(ID3D11Device* device, ComPtr<IDXGIAdapter1>& out, DXGI_ADAPTER_DESC1& desc)
        {
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter))) return false;
            if (FAILED(adapter.As(&out))) return false;
            return SUCCEEDED(out->GetDesc1(&desc));
        }

        void InitD3D11(ID3D11Device* device, const NVSDK_NGX_FeatureCommonInfo& info)
        {
            NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                                      G().logDir.c_str(), device, &info);
            Set(kKeyNgx11Result, static_cast<int>(r));
            if (NVSDK_NGX_FAILED(r))
            {
                LogE("DLSS: NGX D3D11 init failed: 0x%08X", static_cast<unsigned>(r));
                return;
            }
            ngx11 = true;
            r = NVSDK_NGX_D3D11_GetCapabilityParameters(&caps11);
            if (NVSDK_NGX_FAILED(r))
            {
                LogE("DLSS: NGX D3D11 capabilities failed: 0x%08X", static_cast<unsigned>(r));
                return;
            }
            Set(kKeySrAvailable, GetI(caps11, NVSDK_NGX_Parameter_SuperSampling_Available, 0));
            Set(kKeySrNeedsDriver, GetI(caps11, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, 0));
            Set(kKeySrMinDriverMajor, static_cast<int>(GetU(caps11, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, 0)));
            Set(kKeySrMinDriverMinor, static_cast<int>(GetU(caps11, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, 0)));
            Set(kKeySrInitResult, GetI(caps11, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, 0));
        }

        void InitD3D12(IDXGIAdapter1* adapter, const NVSDK_NGX_FeatureCommonInfo& info, bool nvidia)
        {
            HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12));
            Set(kKeyD3d12Result, static_cast<int>(hr));
            if (FAILED(hr))
            {
                LogE("DLSS: D3D12 device on the game's adapter failed: 0x%08X", static_cast<unsigned>(hr));
                return;
            }
            if (!nvidia) return; // the device serves FSR only
            NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                                      G().logDir.c_str(), device12.Get(), &info);
            Set(kKeyNgx12Result, static_cast<int>(r));
            if (NVSDK_NGX_FAILED(r))
            {
                LogE("DLSS: NGX D3D12 init failed: 0x%08X", static_cast<unsigned>(r));
                return;
            }
            ngx12 = true;
            r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps12);
            if (NVSDK_NGX_FAILED(r))
            {
                LogE("DLSS: NGX D3D12 capabilities failed: 0x%08X", static_cast<unsigned>(r));
                return;
            }
            Set(kKeyFgAvailable, GetI(caps12, NVSDK_NGX_Parameter_FrameGeneration_Available, 0));
            Set(kKeyFgNeedsDriver, GetI(caps12, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, 0));
            Set(kKeyFgMinDriverMajor, static_cast<int>(GetU(caps12, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, 0)));
            Set(kKeyFgMinDriverMinor, static_cast<int>(GetU(caps12, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, 0)));
            Set(kKeyFgInitResult, GetI(caps12, NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, 0));
            Set(kKeyFgMultiFrameMax, static_cast<int>(GetU(caps12, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, 0)));
        }
    }

    void InitNgx()
    {
        Globals& g = G();
        std::lock_guard<std::mutex> guard(g.lock);
        if (Get(kKeyInitState) != 0) return;
        Set(kKeyInitState, 1);
        Set(kKeyRenderThreadId, static_cast<int>(GetCurrentThreadId()));
        if (!g.device11)
        {
            LogE("DLSS: no D3D11 device from Unity");
            Set(kKeyInitState, 2);
            return;
        }
        Set(kKeyFeatureLevel, static_cast<int>(g.device11->GetFeatureLevel()));
        ComPtr<ID3D11Device5> d5;
        Set(kKeyHasD3d11_4, SUCCEEDED(g.device11->QueryInterface(IID_PPV_ARGS(&d5))) ? 1 : 0);

        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc = {};
        if (AdapterOf(g.device11, adapter, desc))
            Log("adapter: %s, VRAM %llu MB, vendor 0x%04X device 0x%04X", Narrow(desc.Description).c_str(),
                static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20), desc.VendorId, desc.DeviceId);

        const wchar_t* paths[] = {g.featureDir.c_str(), nullptr};
        NVSDK_NGX_FeatureCommonInfo info = CommonInfo(paths);
        // NGX (DLSS) exists on NVIDIA video cards only; elsewhere only our D3D12 device is made (for FSR).
        bool nvidia = adapter && desc.VendorId == 0x10DE;
        if (nvidia) InitD3D11(g.device11, info);
        else Log("DLSS: not an NVIDIA video card (vendor 0x%04X): DLSS and DLSS Frame Generation off", desc.VendorId);
        if (adapter) InitD3D12(adapter.Get(), info, nvidia);
        InitFsr(device12.Get()); // any video card with a D3D12 device
        InitReflex();

        Log("init: ngx11=0x%08X sr=%d (driver %d.%d needed=%d, init 0x%08X) | d3d12=0x%08X ngx12=0x%08X fg=%d mfgMax=%d (driver %d.%d needed=%d, init 0x%08X)",
            static_cast<unsigned>(Get(kKeyNgx11Result)), Get(kKeySrAvailable), Get(kKeySrMinDriverMajor),
            Get(kKeySrMinDriverMinor), Get(kKeySrNeedsDriver), static_cast<unsigned>(Get(kKeySrInitResult)),
            static_cast<unsigned>(Get(kKeyD3d12Result)), static_cast<unsigned>(Get(kKeyNgx12Result)), Get(kKeyFgAvailable),
            Get(kKeyFgMultiFrameMax), Get(kKeyFgMinDriverMajor), Get(kKeyFgMinDriverMinor), Get(kKeyFgNeedsDriver),
            static_cast<unsigned>(Get(kKeyFgInitResult)));
        Set(kKeyInitState, 2);
    }

    bool Ngx11Ready()
    {
        return ngx11 && Get(kKeySrAvailable) != 0;
    }

    bool Ngx12Ready()
    {
        return ngx12 && Get(kKeyFgAvailable) != 0;
    }

    ID3D12Device* Device12()
    {
        return device12.Get();
    }

    void ShutdownNgx()
    {
        ShutdownFg(); // stops the presenter thread first: it takes the lock below for its NGX calls
        Globals& g = G();
        std::lock_guard<std::mutex> guard(g.lock);
        ShutdownDlss();
        ShutdownFsr(); // before the D3D12 device goes
        if (caps11) NVSDK_NGX_D3D11_DestroyParameters(caps11);
        if (caps12) NVSDK_NGX_D3D12_DestroyParameters(caps12);
        caps11 = caps12 = nullptr;
        if (ngx11 && g.device11) NVSDK_NGX_D3D11_Shutdown1(g.device11);
        if (ngx12 && device12) NVSDK_NGX_D3D12_Shutdown1(device12.Get());
        ngx11 = ngx12 = false;
        device12.Reset();
    }
}
