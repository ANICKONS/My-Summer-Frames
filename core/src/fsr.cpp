// My Summer Frames core: AMD FSR upscaling for the Direct3D 11 game, through our Direct3D 12 device.
// Copyright (C) 2026 ANICKON
//
// The FSR SDK (2.4) exists for D3D12 and Vulkan only. Per frame, on Unity's render thread (inside EvaluateSlot,
// after the motion vectors, where DLSS would run):
//   1. D3D11: colour, depth and motion (render size) into textures shared with our D3D12 device; fence "in";
//   2. D3D12 (our own queue): waits for "in" on the GPU, copies them into own textures, runs FSR into an own output,
//      copies that into a shared output; fence "out";
//   3. D3D11: waits for "out" on the GPU (the CPU never waits for the GPU here) and copies the shared output into
//      the upscaler's output texture; the rest of EvaluateSlot is the same as with DLSS.
// The FSR API picks the implementation for the device: the ML upscaler (FSR 4) on Radeon RX 6000 and newer, the
// analytical one (FSR 3.1) elsewhere. Inputs as for DLSS: jitter and motion vectors in pixels (motion from the
// current pixel to the previous one), depth 0 near .. 1 far, the colour tonemapped and gamma encoded.
#include "ffx.h"

#include <vector>

namespace msf
{
    namespace
    {
        HMODULE loader = nullptr;
        FfxFns fns;
        bool loaded = false;

        // Render thread only (G().lock held).
        constexpr int kLists = 3;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocators[kLists];
        ComPtr<ID3D12GraphicsCommandList> lists[kLists];
        uint64_t listValue[kLists] = {};
        int listNext = 0;
        ComPtr<ID3D12Fence> in12, out12, idle12;
        ComPtr<ID3D11Fence> in11, out11;
        ComPtr<ID3D11DeviceContext4> ctx4;
        uint64_t fenceValue = 0, idleValue = 0;
        HANDLE fenceEvent = nullptr;
        bool objectsFailed = false;

        SharedTex sharedColor, sharedDepth, sharedMotion, sharedOutput;
        ComPtr<ID3D12Resource> ownColor, ownDepth, ownMotion, ownOutput;
        UINT texW = 0, texH = 0, texOW = 0, texOH = 0;
        DXGI_FORMAT texFormat = DXGI_FORMAT_UNKNOWN;
        UINT failedW = 0, failedH = 0, failedOW = 0, failedOH = 0; // textures failed for these sizes

        ffxContext context = nullptr;
        UINT ctxW = 0, ctxH = 0, ctxOW = 0, ctxOH = 0;
        bool ctxDebug = false;
        bool broken = false;      // context creation failed for ctxW.. (retried when the sizes change)
        bool stuck = false;       // the D3D12 queue stopped: FSR off for the session
        std::atomic<bool> debugWanted{false};
        std::atomic<int> messages{0};

        std::string Versions(uint64_t createDescType, ID3D12Device* d12)
        {
            ffxQueryDescGetVersions q = {};
            q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
            q.createDescType = createDescType;
            q.device = d12;
            uint64_t count = 0;
            q.outputCount = &count;
            ffxReturnCode_t r = fns.Query(nullptr, &q.header);
            if (r != FFX_API_RETURN_OK || count == 0)
            {
                Log("FSR: versions of 0x%llX: result %u, count %llu", static_cast<unsigned long long>(createDescType), r,
                    static_cast<unsigned long long>(count));
                return "";
            }
            std::vector<uint64_t> ids(static_cast<size_t>(count));
            std::vector<const char*> names(static_cast<size_t>(count), nullptr);
            q.versionIds = ids.data();
            q.versionNames = names.data();
            if (fns.Query(nullptr, &q.header) != FFX_API_RETURN_OK) return "";
            std::string s;
            for (size_t i = 0; i < names.size() && i < count; i++)
            {
                if (!names[i]) continue;
                if (!s.empty()) s += ", ";
                s += names[i];
            }
            return s;
        }

        void WaitIdle()
        {
            if (!queue || !idle12) return;
            queue->Signal(idle12.Get(), ++idleValue);
            if (idle12->GetCompletedValue() >= idleValue) return;
            idle12->SetEventOnCompletion(idleValue, fenceEvent);
            if (WaitForSingleObject(fenceEvent, 2000) == WAIT_TIMEOUT) LogE("FSR: the D3D12 queue does not finish (idle wait)");
        }

        bool EnsureObjects(ID3D12Device* d12)
        {
            if (queue) return true;
            if (objectsFailed) return false;
            objectsFailed = true;
            ComPtr<ID3D11Device5> d11;
            ComPtr<ID3D11DeviceContext> ctx;
            G().device11->GetImmediateContext(&ctx);
            if (FAILED(G().device11->QueryInterface(IID_PPV_ARGS(&d11))) || FAILED(ctx.As(&ctx4)))
            {
                LogE("FSR: Direct3D 11.4 missing (shared fences)");
                return false;
            }
            D3D12_COMMAND_QUEUE_DESC qd = {};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
            {
                LogE("FSR: D3D12 queue failed");
                return false;
            }
            queue->SetName(L"My Summer Frames FSR upscaler");
            for (int i = 0; i < kLists; i++)
            {
                if (FAILED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i]))) ||
                    FAILED(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i]))))
                {
                    LogE("FSR: D3D12 command lists failed");
                    queue.Reset();
                    return false;
                }
                lists[i]->Close();
                listValue[i] = 0;
            }
            HANDLE h1 = nullptr, h2 = nullptr;
            HRESULT hr = d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&in12));
            if (SUCCEEDED(hr)) hr = d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&out12));
            if (SUCCEEDED(hr)) hr = d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle12));
            if (SUCCEEDED(hr)) hr = d12->CreateSharedHandle(in12.Get(), nullptr, GENERIC_ALL, nullptr, &h1);
            if (SUCCEEDED(hr)) hr = d12->CreateSharedHandle(out12.Get(), nullptr, GENERIC_ALL, nullptr, &h2);
            if (SUCCEEDED(hr)) hr = d11->OpenSharedFence(h1, IID_PPV_ARGS(&in11));
            if (SUCCEEDED(hr)) hr = d11->OpenSharedFence(h2, IID_PPV_ARGS(&out11));
            if (h1) CloseHandle(h1);
            if (h2) CloseHandle(h2);
            if (FAILED(hr))
            {
                LogE("FSR: shared fences failed 0x%08X", static_cast<unsigned>(hr));
                queue.Reset();
                return false;
            }
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            objectsFailed = false;
            Log("FSR: D3D12 queue and shared fences ready");
            return true;
        }

        bool MakeOwn(ComPtr<ID3D12Resource>& r, UINT w, UINT h, DXGI_FORMAT f, bool uav, D3D12_RESOURCE_STATES state)
        {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = w;
            rd.Height = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = f;
            rd.SampleDesc.Count = 1;
            rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            HRESULT hr = Device12()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r));
            if (FAILED(hr)) LogE("FSR: texture %ux%u %s failed 0x%08X", w, h, FormatName(f).c_str(), static_cast<unsigned>(hr));
            return SUCCEEDED(hr);
        }

        void ReleaseTextures()
        {
            for (SharedTex* t : {&sharedColor, &sharedDepth, &sharedMotion, &sharedOutput}) *t = SharedTex{};
            ownColor.Reset();
            ownDepth.Reset();
            ownMotion.Reset();
            ownOutput.Reset();
            texW = texH = texOW = texOH = 0;
            texFormat = DXGI_FORMAT_UNKNOWN;
        }

        bool EnsureTextures(UINT w, UINT h, UINT ow, UINT oh, DXGI_FORMAT f)
        {
            if (ownColor && texW == w && texH == h && texOW == ow && texOH == oh && texFormat == f) return true;
            if (failedW == w && failedH == h && failedOW == ow && failedOH == oh) return false;
            WaitIdle();
            ReleaseTextures();
            ID3D12Device* d12 = Device12();
            ComPtr<ID3D11Device1> d11;
            const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            bool ok = SUCCEEDED(G().device11->QueryInterface(IID_PPV_ARGS(&d11))) &&
                      MakeSharedTexture(d12, d11.Get(), w, h, f, sharedColor, "FSR colour") &&
                      MakeSharedTexture(d12, d11.Get(), w, h, DXGI_FORMAT_R32_FLOAT, sharedDepth, "FSR depth") &&
                      MakeSharedTexture(d12, d11.Get(), w, h, DXGI_FORMAT_R16G16_FLOAT, sharedMotion, "FSR motion") &&
                      MakeSharedTexture(d12, d11.Get(), ow, oh, f, sharedOutput, "FSR output") &&
                      MakeOwn(ownColor, w, h, f, false, read) && MakeOwn(ownDepth, w, h, DXGI_FORMAT_R32_FLOAT, false, read) &&
                      MakeOwn(ownMotion, w, h, DXGI_FORMAT_R16G16_FLOAT, false, read) &&
                      MakeOwn(ownOutput, ow, oh, f, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!ok)
            {
                ReleaseTextures();
                failedW = w;
                failedH = h;
                failedOW = ow;
                failedOH = oh;
                return false;
            }
            texW = w;
            texH = h;
            texOW = ow;
            texOH = oh;
            texFormat = f;
            Log("FSR: textures %ux%u -> %ux%u %s", w, h, ow, oh, FormatName(f).c_str());
            return true;
        }

        void DestroyUpscaler()
        {
            if (!context) return;
            WaitIdle();
            fns.DestroyContext(&context, nullptr);
            context = nullptr;
        }

        bool EnsureContext(UINT w, UINT h, UINT ow, UINT oh)
        {
            bool debug = debugWanted.load();
            bool same = ctxW == w && ctxH == h && ctxOW == ow && ctxOH == oh && ctxDebug == debug;
            if (context && same) return true;
            if (broken && same) return false;
            DestroyUpscaler();
            ctxW = w;
            ctxH = h;
            ctxOW = ow;
            ctxOH = oh;
            ctxDebug = debug;
            broken = true;
            ffxCreateBackendDX12Desc backend = {};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            backend.device = Device12();
            ffxCreateContextDescUpscaleVersion version = {};
            version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
            version.header.pNext = &backend.header;
            version.version = FFX_UPSCALER_VERSION;
            ffxCreateContextDescUpscale cd = {};
            cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            cd.header.pNext = &version.header;
            // The game's picture is tonemapped and gamma encoded (Unity's gamma colour space), depth is 0 near .. 1 far.
            cd.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE |
                       (debug ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0u);
            cd.maxRenderSize = {w, h};
            cd.maxUpscaleSize = {ow, oh};
            cd.fpMessage = FfxMessage;
            ffxReturnCode_t r = fns.CreateContext(&context, &cd.header, nullptr);
            Set(kKeyFsrCreateResult, static_cast<int>(r));
            if (r != FFX_API_RETURN_OK)
            {
                context = nullptr;
                LogE("FSR: upscaler %ux%u -> %ux%u failed (%u)", w, h, ow, oh, r);
                return false;
            }
            broken = false;
            std::string v = FfxProviderVersion(&context);
            SetText(kTextFsrUpActive, v);
            LogC("FSR: upscaler %s created %ux%u -> %ux%u%s", v.c_str(), w, h, ow, oh, debug ? " (debug checks)" : "");
            return true;
        }

        void Transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
        {
            if (from == to) return;
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = r;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            l->ResourceBarrier(1, &b);
        }

        // The command list ring: an allocator is reused once its frame is done on the GPU (normally long ago). A
        // queue that stops is let go: "out" is set from the CPU so the D3D11 side never waits for it again.
        ID3D12GraphicsCommandList* BeginList()
        {
            int i = listNext;
            if (out12->GetCompletedValue() < listValue[i])
            {
                out12->SetEventOnCompletion(listValue[i], fenceEvent);
                if (WaitForSingleObject(fenceEvent, 2000) == WAIT_TIMEOUT)
                {
                    LogE("FSR: the D3D12 queue stopped (frame %llu of %llu, device removed 0x%08X) - FSR off",
                         static_cast<unsigned long long>(out12->GetCompletedValue()), static_cast<unsigned long long>(fenceValue),
                         static_cast<unsigned>(Device12()->GetDeviceRemovedReason()));
                    out12->Signal(fenceValue + (1ull << 40));
                    stuck = true;
                    Set(kKeyFsrUpAvailable, 0);
                    return nullptr;
                }
            }
            listNext = (i + 1) % kLists;
            allocators[i]->Reset();
            lists[i]->Reset(allocators[i].Get(), nullptr);
            return lists[i].Get();
        }

        int ListIndex(ID3D12GraphicsCommandList* l)
        {
            for (int i = 0; i < kLists; i++)
                if (lists[i].Get() == l) return i;
            return 0;
        }
    }

    std::string FfxProviderVersion(ffxContext* c)
    {
        if (!loaded || !c || !*c) return "";
        ffxQueryGetProviderVersion q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (fns.Query(c, &q.header) != FFX_API_RETURN_OK || !q.versionName) return "";
        return q.versionName;
    }

    void FfxMessage(uint32_t type, const wchar_t* message)
    {
        if (messages.fetch_add(1) >= 200) return;
        Log("FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", Narrow(std::wstring(message ? message : L"")).c_str());
    }

    const FfxFns& Ffx()
    {
        return fns;
    }

    bool FsrLoaded()
    {
        return loaded;
    }

    bool FsrUpscaleReady()
    {
        return loaded && !stuck && Get(kKeyFsrUpAvailable) == 1;
    }

    bool FsrFgReady()
    {
        return loaded && Get(kKeyFsrFgAvailable) == 1;
    }

    void SetFsrDebug(bool on)
    {
        debugWanted = on;
        messages = 0;
        LogC("FSR: debug checks %s (the contexts are made again)", on ? "on" : "off");
    }

    bool FsrDebug()
    {
        return debugWanted.load();
    }

    void InitFsr(ID3D12Device* d12)
    {
        Set(kKeyFsrCreateResult, -1);
        if (!d12)
        {
            Set(kKeyFsrLoaded, -2);
            LogE("FSR: no D3D12 device on this video card - FSR off");
            return;
        }
        std::wstring path = G().featureDir + L"\\amd_fidelityfx_loader_dx12.dll";
        loader = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!loader)
        {
            Set(kKeyFsrLoaded, -1);
            LogE("FSR: %s not loadable (error %lu) - FSR off", Narrow(path).c_str(), GetLastError());
            return;
        }
        fns.CreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(loader, "ffxCreateContext"));
        fns.DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(loader, "ffxDestroyContext"));
        fns.Configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(loader, "ffxConfigure"));
        fns.Query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(loader, "ffxQuery"));
        fns.Dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(loader, "ffxDispatch"));
        if (!fns.CreateContext || !fns.DestroyContext || !fns.Configure || !fns.Query || !fns.Dispatch)
        {
            Set(kKeyFsrLoaded, -1);
            LogE("FSR: the loader lacks the FSR API functions - FSR off");
            return;
        }
        loaded = true;
        Set(kKeyFsrLoaded, 1);
        std::string up = Versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, d12);
        std::string fg = Versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, d12);
        std::string sc = Versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12, d12);
        SetText(kTextFsrUpVersions, up);
        SetText(kTextFsrFgVersions, fg);
        SetText(kTextFsrScVersions, sc);
        Set(kKeyFsrUpAvailable, up.empty() ? 0 : 1);
        Set(kKeyFsrFgAvailable, fg.empty() || sc.empty() ? 0 : 1);
        Log("FSR: upscaler [%s], frame generation [%s], frame generation swap chain [%s]", up.c_str(), fg.c_str(), sc.c_str());
    }

    bool FsrUpscale(ID3D11DeviceContext* ctx, const FrameParams& p, ID3D11Texture2D* color, ID3D11Texture2D* depth,
                    ID3D11Texture2D* motion, ID3D11Texture2D* output, DXGI_FORMAT format)
    {
        ID3D12Device* d12 = Device12();
        if (!FsrUpscaleReady() || !d12 || !EnsureObjects(d12)) return false;
        UINT w = static_cast<UINT>(p.width), h = static_cast<UINT>(p.height);
        UINT ow = static_cast<UINT>(p.outWidth > 0 ? p.outWidth : p.width), oh = static_cast<UINT>(p.outHeight > 0 ? p.outHeight : p.height);
        if (!EnsureTextures(w, h, ow, oh, format) || !EnsureContext(w, h, ow, oh)) return false;
        ID3D12GraphicsCommandList* l = BeginList();
        if (!l) return false;

        // 1. D3D11: the inputs to the shared textures.
        uint64_t v = ++fenceValue;
        ctx->CopyResource(sharedColor.r11.Get(), color);
        ctx->CopyResource(sharedDepth.r11.Get(), depth);
        ctx->CopyResource(sharedMotion.r11.Get(), motion);
        ctx4->Signal(in11.Get(), v);
        ctx4->Flush();

        // 2. D3D12: own copies, FSR, the result to the shared output.
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        for (auto pair : {std::make_pair(ownColor.Get(), sharedColor.r12.Get()), std::make_pair(ownDepth.Get(), sharedDepth.r12.Get()),
                          std::make_pair(ownMotion.Get(), sharedMotion.r12.Get())})
        {
            Transition(l, pair.first, read, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyResource(pair.first, pair.second);
            Transition(l, pair.first, D3D12_RESOURCE_STATE_COPY_DEST, read);
        }
        ffxDispatchDescUpscale d = {};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = l;
        d.color = ffxApiGetResourceDX12(ownColor.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(ownDepth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = ffxApiGetResourceDX12(ownMotion.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(ownOutput.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {p.jitterX, p.jitterY};
        d.motionVectorScale = {1.0f, 1.0f};
        d.renderSize = {w, h};
        d.upscaleSize = {ow, oh};
        d.enableSharpening = p.sharpness > 0.0f; // FSR's RCAS
        d.sharpness = p.sharpness;
        d.frameTimeDelta = p.frameMs > 0.1f ? p.frameMs : 16.6f;
        d.preExposure = 1.0f;
        d.reset = p.reset != 0;
        d.cameraNear = p.fg.nearZ;
        d.cameraFar = p.fg.farZ;
        d.cameraFovAngleVertical = p.fg.fov;
        d.viewSpaceToMetersFactor = 1.0f; // Unity units are metres
        d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        ffxReturnCode_t r = fns.Dispatch(&context, &d.header);
        Set(kKeyFsrLastResult, static_cast<int>(r));
        bool ok = r == FFX_API_RETURN_OK;
        if (ok)
        {
            Transition(l, ownOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(l, sharedOutput.r12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyResource(sharedOutput.r12.Get(), ownOutput.Get());
            Transition(l, sharedOutput.r12.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Transition(l, ownOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else if (G().values[kKeyFsrFails].fetch_add(1) < 5) LogE("FSR: upscaling failed (%u)", r);
        l->Close();
        queue->Wait(in12.Get(), v);
        ID3D12CommandList* cl[] = {l};
        queue->ExecuteCommandLists(1, cl);
        queue->Signal(out12.Get(), v);
        listValue[ListIndex(l)] = v;
        if (!ok) return false;

        // 3. D3D11: the result once D3D12 is done (a GPU wait in the D3D11 queue).
        ctx4->Wait(out11.Get(), v);
        ctx->CopyResource(output, sharedOutput.r11.Get());
        G().values[kKeyFsrEvals].fetch_add(1);
        return true;
    }

    void ShutdownFsr()
    {
        if (queue) WaitIdle();
        DestroyUpscaler();
        ReleaseTextures();
        for (auto& a : allocators) a.Reset();
        for (auto& l : lists) l.Reset();
        in11.Reset();
        out11.Reset();
        in12.Reset();
        out12.Reset();
        idle12.Reset();
        ctx4.Reset();
        queue.Reset();
        if (fenceEvent) CloseHandle(fenceEvent);
        fenceEvent = nullptr;
        objectsFailed = false;
        ctxW = ctxH = ctxOW = ctxOH = 0;
        broken = false;
        // The loader stays loaded: the game is closing (the effect DLLs' threads are gone with their contexts).
    }
}
