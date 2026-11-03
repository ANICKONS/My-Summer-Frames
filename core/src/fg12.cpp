// My Summer Frames core: the Direct3D 11 -> 12 bridge for DLSS and FSR Frame Generation.
// Copyright (C) 2026 ANICKON
//
// DLSS and FSR Frame Generation exist for D3D12 (and Vulkan) only; the game is D3D11. The bridge:
//   * At the end of every frame (the mod's WaitForEndOfFrame event, render thread) the game's back buffer and this
//     frame's depth + motion vectors (dlss11.cpp, back buffer orientation) are copied into textures shared with our
//     D3D12 device on the same adapter; a shared fence is signalled and the frame is queued for the presenter thread.
//   * The presenter thread waits for the fence on its D3D12 queue, then
//       DLSS: runs NGX DLSS-G (1..5 generated frames between the last two real ones) and presents the generated
//             frames, then the real one, evenly spaced, on its own flip-model swap chain;
//       FSR:  puts the frame into the back buffer of the FSR SDK's frame generation swap chain (a proxy of a DXGI
//             swap chain, with its own pacing and present threads), prepares the frame generation inputs and
//             presents it; the proxy generates one frame in between and presents both, paced.
//     Either swap chain belongs to a child window that covers the game window's client area (a window can have
//     only one flip-model swap chain, and the game keeps its own); the child lets mouse and keyboard through.
//   * The child window has a thread of its own (it only pumps its messages): DXGI may talk to a swap chain's window
//     synchronously from any present, and that thread never waits for anything.
//   * While the bridge warms up, the child stays hidden; once ours has presented, the child is shown over the game's
//     picture (the game goes on presenting under it). Off = the child hidden.
// The game's device is single-threaded: every D3D11 call of the bridge is on the render thread.
#include "ffx.h"

#include "nvsdk_ngx_helpers_dlssg_d3d.h"

#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>

namespace msf
{
    namespace
    {
        constexpr int kSlotsFg = 3;   // frames shared between the APIs
        constexpr int kLists = 16;    // D3D12 command lists in flight
        constexpr int kMaxPending = 2;

        struct Slot
        {
            SharedTex color, depth, motion, hudless;
        };

        struct Pending
        {
            uint64_t no;
            int slot;
            bool hasCamera;
            bool hasHudless;
            FgCamera camera;
            LARGE_INTEGER time;
        };


        // Shared between the render thread (Feed) and the presenter thread.
        std::mutex fgLock;
        std::condition_variable fgCv;
        std::deque<Pending> pending;
        bool quit = false;
        std::atomic<int> mode{0};           // generated frames per real frame
        std::atomic<int> kind{kFgDlss};     // FgKind wanted
        std::atomic<int> stage{0};
        std::atomic<int> visibleAfter{10};  // the child is shown once the bridge has presented this many real frames
        std::atomic<uint64_t> submitted{0}, processed{0};
        std::atomic<bool> dumpWanted{false};
        std::wstring dumpFolder;
        HANDLE frameEvent = nullptr; // set by Feed: wakes the presenter (which also pumps its window's messages)
        std::atomic<bool> dump11Wanted{false};
        void Dump11(ID3D11Texture2D* bb);

        // Shared resources (created by the render thread while the presenter is idle).
        Slot slots[kSlotsFg];
        UINT sharedW = 0, sharedH = 0;
        DXGI_FORMAT sharedFormat = DXGI_FORMAT_UNKNOWN;
        UINT motionW = 0, motionH = 0;
        ComPtr<ID3D12Fence> ready12, free12;
        ComPtr<ID3D11Fence> ready11, free11;
        ComPtr<ID3D11DeviceContext4> ctx4;
        bool sharedFailed = false;

        // Presenter thread only.
        std::thread presenter;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocators[kLists];
        ComPtr<ID3D12GraphicsCommandList> lists[kLists];
        uint64_t listFence[kLists] = {};
        int listNext = 0;
        ComPtr<ID3D12Fence> gpuFence;
        uint64_t gpuValue = 0;
        HANDLE gpuEvent = nullptr;
        ComPtr<IDXGISwapChain3> swapchain;
        UINT scW = 0, scH = 0;
        DXGI_FORMAT scFormat = DXGI_FORMAT_UNKNOWN;
        bool tearing = false;
        ComPtr<ID3D12Resource> interp;
        NVSDK_NGX_Handle* feature = nullptr;
        NVSDK_NGX_Parameter* params = nullptr;
        UINT featureW = 0, featureH = 0, featureRW = 0, featureRH = 0;
        int featureFrames = 0;
        bool havePrevious = false;
        double intervalMs = 16.0;
        LARGE_INTEGER lastArrival = {}, freq = {};
        LARGE_INTEGER lastProgress = {}; // presenter: last frame done (read by the watchdog in Feed)
        HANDLE timer = nullptr;

        int traced = 0; // presenter: the first frames are traced step by step (a hang shows where)
#define FG_TRACE(...) do { if (traced < 4) Log(__VA_ARGS__); } while (0)

        // Presenter thread: the FSR path (FSR SDK frame generation swap chain + frame generation context).
        int activeKind = 0;
        ffxContext fsrSwapCtx = nullptr, fsrFgCtx = nullptr;
        ComPtr<IDXGISwapChain4> fsrProxy;
        UINT fsrW = 0, fsrH = 0, fsrRW = 0, fsrRH = 0;
        DXGI_FORMAT fsrFormat = DXGI_FORMAT_UNKNOWN;
        bool fsrFailed = false;         // creation failed for these sizes (retried when they change)
        uint64_t fsrFrameId = 0;
        bool fsrPrepared = false;       // the last frame ran the frame generation prepare pass
        UINT fsrRealSince = 0;          // real frames presented through the current proxy
        int fsrGenSince = 0;            // generated frames seen on the current proxy (its present count - real)

        // The child window's own thread.
        std::thread windowThread;
        std::atomic<bool> windowQuit{false};
        HWND gameHwnd = nullptr, childHwnd = nullptr;
        std::atomic<HWND> childForPresenter{nullptr};
        bool childShown = false;

        DXGI_FORMAT TypedColor(DXGI_FORMAT f)
        {
            switch (f)
            {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default: return f;
            }
        }

        double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
        {
            return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
        }

        // ---------------------------------------------------------------- render thread: Present hook

        bool MakeShared(ID3D12Device* d12, ID3D11Device1* d11, UINT w, UINT h, DXGI_FORMAT f, SharedTex& t, const char* tag)
        {
            t = SharedTex{};
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
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            // D3D11 opens a shared R32_FLOAT / R16G16_FLOAT texture only with simultaneous access (core\tests\
            // share_test.cpp); the colour formats open without it. The fences keep the two APIs apart anyway.
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R16G16_FLOAT) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
            const char* step = "D3D12 texture";
            HRESULT hr = d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&t.r12));
            HANDLE handle = nullptr;
            if (SUCCEEDED(hr))
            {
                step = "shared handle";
                hr = d12->CreateSharedHandle(t.r12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
            }
            if (SUCCEEDED(hr))
            {
                step = "open on the game's D3D11 device";
                hr = d11->OpenSharedResource1(handle, IID_PPV_ARGS(&t.r11));
            }
            if (handle) CloseHandle(handle);
            if (FAILED(hr))
            {
                LogE("DLSS FG: shared %s %ux%u %s failed at %s: 0x%08X", tag, w, h, FormatName(f).c_str(), step, static_cast<unsigned>(hr));
                t = SharedTex{};
                return false;
            }
            return true;
        }

        // The presenter must be idle (nothing pending) when this runs.
        bool EnsureShared(UINT w, UINT h, DXGI_FORMAT f, UINT mw, UINT mh)
        {
            if (slots[0].color.r11 && sharedW == w && sharedH == h && sharedFormat == f && motionW == mw && motionH == mh) return true;
            if (sharedFailed) return false;
            ID3D12Device* d12 = Device12();
            ComPtr<ID3D11Device1> d11;
            ComPtr<ID3D11Device5> d11_5;
            if (!d12 || FAILED(G().device11->QueryInterface(IID_PPV_ARGS(&d11))) ||
                FAILED(G().device11->QueryInterface(IID_PPV_ARGS(&d11_5))))
            {
                sharedFailed = true;
                LogE("DLSS FG: no D3D12 device or no Direct3D 11.4");
                return false;
            }
            if (!ctx4)
            {
                ComPtr<ID3D11DeviceContext> ctx;
                G().device11->GetImmediateContext(&ctx);
                if (FAILED(ctx.As(&ctx4))) { sharedFailed = true; LogE("DLSS FG: no ID3D11DeviceContext4"); return false; }
            }
            if (!ready12)
            {
                HANDLE h1 = nullptr, h2 = nullptr;
                HRESULT hr = d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ready12));
                if (SUCCEEDED(hr)) hr = d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&free12));
                if (SUCCEEDED(hr)) hr = d12->CreateSharedHandle(ready12.Get(), nullptr, GENERIC_ALL, nullptr, &h1);
                if (SUCCEEDED(hr)) hr = d12->CreateSharedHandle(free12.Get(), nullptr, GENERIC_ALL, nullptr, &h2);
                if (SUCCEEDED(hr)) hr = d11_5->OpenSharedFence(h1, IID_PPV_ARGS(&ready11));
                if (SUCCEEDED(hr)) hr = d11_5->OpenSharedFence(h2, IID_PPV_ARGS(&free11));
                if (h1) CloseHandle(h1);
                if (h2) CloseHandle(h2);
                if (FAILED(hr))
                {
                    sharedFailed = true;
                    LogE("DLSS FG: shared fences failed 0x%08X", static_cast<unsigned>(hr));
                    return false;
                }
            }
            for (Slot& s : slots)
            {
                if (!MakeShared(d12, d11.Get(), w, h, f, s.color, "colour") ||
                    !MakeShared(d12, d11.Get(), mw, mh, DXGI_FORMAT_R32_FLOAT, s.depth, "depth") ||
                    !MakeShared(d12, d11.Get(), mw, mh, DXGI_FORMAT_R16G16_FLOAT, s.motion, "motion") ||
                    !MakeShared(d12, d11.Get(), w, h, f, s.hudless, "hud-less"))
                {
                    sharedFailed = true;
                    return false;
                }
            }
            sharedW = w;
            sharedH = h;
            sharedFormat = f;
            motionW = mw;
            motionH = mh;
            Log("DLSS FG: shared textures %ux%u %s, depth/motion %ux%u", w, h, FormatName(f).c_str(), mw, mh);
            return true;
        }

        // The game's back buffer at the end of the frame (WaitForEndOfFrame: every camera and the UI are drawn, the
        // game's Present comes next): bound as render target then. No reference is kept (the game resizes it).
        void* lastBackbuffer = nullptr;

        ComPtr<ID3D11Texture2D> FindBackbuffer()
        {
            ComPtr<ID3D11DeviceContext> ctx;
            G().device11->GetImmediateContext(&ctx);
            ComPtr<ID3D11RenderTargetView> rtv;
            ctx->OMGetRenderTargets(1, &rtv, nullptr);
            ComPtr<ID3D11Texture2D> t;
            if (!rtv) return t;
            ComPtr<ID3D11Resource> r;
            rtv->GetResource(&r);
            ComPtr<IDXGIResource> dr;
            DXGI_USAGE usage = 0;
            if (SUCCEEDED(r.As(&dr)) && SUCCEEDED(dr->GetUsage(&usage)) && (usage & DXGI_USAGE_BACK_BUFFER) && SUCCEEDED(r.As(&t)))
            {
                if (t.Get() != lastBackbuffer) Log("DLSS FG: game back buffer %p (usage 0x%X)", static_cast<void*>(t.Get()), usage);
                lastBackbuffer = t.Get();
            }
            else
            {
                static int misses = 0;
                if (misses++ < 3) Log("DLSS FG: end of frame: render target is not the back buffer (usage 0x%X)", usage);
                t.Reset();
            }
            return t;
        }

        // Copies this frame for the presenter (render thread, end of frame). The game still presents its own frame
        // to its window; once the bridge runs, our child window covers it.
        bool Feed()
        {
            ComPtr<ID3D11Texture2D> bb = FindBackbuffer();
            if (!bb) return false;
            D3D11_TEXTURE2D_DESC bd;
            bb->GetDesc(&bd);
            if (Get(kKeyFgBackbuffer) != static_cast<int>(bd.Format))
            {
                Set(kKeyFgBackbuffer, static_cast<int>(bd.Format));
                Log("DLSS FG: game back buffer %ux%u %s, %u samples", bd.Width, bd.Height, FormatName(bd.Format).c_str(),
                    bd.SampleDesc.Count);
            }
            ComPtr<ID3D11Texture2D> depth, motion, hudless;
            FgCamera camera;
            bool hasCamera = TakeFgInputs(&depth, &motion, &hudless, &camera);
            UINT mw = bd.Width, mh = bd.Height;
            if (hasCamera)
            {
                D3D11_TEXTURE2D_DESC dd;
                depth->GetDesc(&dd);
                mw = dd.Width;
                mh = dd.Height;
            }

            std::unique_lock<std::mutex> lk(fgLock);
            // Back-pressure: at most kMaxPending frames ahead of the presenter (this paces the game).
            fgCv.wait_for(lk, std::chrono::milliseconds(100), [] { return submitted.load() - processed.load() < kMaxPending || quit; });
            if (submitted.load() - processed.load() >= kMaxPending)
            {
                // Watchdog: a presenter that does not move for a second switches the bridge off (the game presents).
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                if (lastProgress.QuadPart && freq.QuadPart && Ms(lastProgress, now) > 1000.0)
                {
                    mode = 0;
                    LogE("DLSS FG: the presenter stopped (frame %llu of %llu) - frame generation off",
                         static_cast<unsigned long long>(processed.load()), static_cast<unsigned long long>(submitted.load()));
                }
                return false;
            }
            DXGI_FORMAT f = TypedColor(bd.Format);
            if (sharedW != bd.Width || sharedH != bd.Height || sharedFormat != f || (hasCamera && (motionW != mw || motionH != mh)))
            {
                // New size: wait until the presenter has finished with every slot.
                fgCv.wait_for(lk, std::chrono::milliseconds(200), [] { return submitted.load() == processed.load(); });
                if (submitted.load() != processed.load()) return false;
                if (!EnsureShared(bd.Width, bd.Height, f, mw, mh)) return false;
                havePrevious = false;
            }
            if (!slots[0].color.r11) return false;
            uint64_t no = submitted.load() + 1;
            int s = static_cast<int>(no % kSlotsFg);
            ID3D11DeviceContext4* ctx = ctx4.Get();
            // Never a GPU wait here (a stalled D3D11 queue stalls the game): a slot still in use = this frame is
            // skipped by the bridge.
            if (no > kSlotsFg && free12->GetCompletedValue() < no - kSlotsFg) return false;
            // With the game's anti-aliasing on, its back buffer is multisampled: resolved, not copied.
            if (bd.SampleDesc.Count > 1) ctx->ResolveSubresource(slots[s].color.r11.Get(), 0, bb.Get(), 0, f);
            else ctx->CopyResource(slots[s].color.r11.Get(), bb.Get());
            if (dump11Wanted.exchange(false)) Dump11(slots[s].color.r11.Get());
            bool hasHudless = false;
            if (hasCamera)
            {
                ctx->CopyResource(slots[s].depth.r11.Get(), depth.Get());
                ctx->CopyResource(slots[s].motion.r11.Get(), motion.Get());
                D3D11_TEXTURE2D_DESC hd;
                hudless->GetDesc(&hd);
                hasHudless = hd.Width == bd.Width && hd.Height == bd.Height && TypedColor(hd.Format) == f;
                if (hasHudless) ctx->CopyResource(slots[s].hudless.r11.Get(), hudless.Get());
            }
            ctx->Signal(ready11.Get(), no);
            ctx->Flush();
            if (no <= 4) Log("DLSS FG trace: fed frame %llu (camera %d)", static_cast<unsigned long long>(no), hasCamera ? 1 : 0);
            Pending p;
            p.no = no;
            p.slot = s;
            p.hasCamera = hasCamera;
            p.hasHudless = hasHudless;
            p.camera = camera;
            QueryPerformanceCounter(&p.time);
            pending.push_back(p);
            submitted.store(no);
            lk.unlock();
            SetEvent(frameEvent);
            return true;
        }

        // ---------------------------------------------------------------- presenter thread

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

        ID3D12GraphicsCommandList* BeginList()
        {
            int i = listNext;
            listNext = (listNext + 1) % kLists;
            if (gpuFence->GetCompletedValue() < listFence[i])
            {
                gpuFence->SetEventOnCompletion(listFence[i], gpuEvent);
                if (WaitForSingleObject(gpuEvent, 2000) == WAIT_TIMEOUT)
                    LogE("DLSS FG: the D3D12 queue is stuck (list %d, fence %llu of %llu, ready %llu)", i,
                         static_cast<unsigned long long>(gpuFence->GetCompletedValue()), static_cast<unsigned long long>(listFence[i]),
                         static_cast<unsigned long long>(ready12->GetCompletedValue()));
            }
            allocators[i]->Reset();
            lists[i]->Reset(allocators[i].Get(), nullptr);
            return lists[i].Get();
        }

        void Submit(ID3D12GraphicsCommandList* l)
        {
            l->Close();
            ID3D12CommandList* cl[] = {l};
            queue->ExecuteCommandLists(1, cl);
            queue->Signal(gpuFence.Get(), ++gpuValue);
            for (int i = 0; i < kLists; i++)
                if (lists[i].Get() == l) listFence[i] = gpuValue;
        }

        void WaitGpuIdle()
        {
            queue->Signal(gpuFence.Get(), ++gpuValue);
            if (gpuFence->GetCompletedValue() < gpuValue)
            {
                gpuFence->SetEventOnCompletion(gpuValue, gpuEvent);
                if (WaitForSingleObject(gpuEvent, 2000) == WAIT_TIMEOUT)
                    LogE("DLSS FG: D3D12 GPU idle wait timed out (fence %llu of %llu, ready %llu, device removed 0x%08X)",
                         static_cast<unsigned long long>(gpuFence->GetCompletedValue()), static_cast<unsigned long long>(gpuValue),
                         static_cast<unsigned long long>(ready12 ? ready12->GetCompletedValue() : 0),
                         static_cast<unsigned>(Device12()->GetDeviceRemovedReason()));
            }
        }

        bool PresenterSetup()
        {
            ID3D12Device* d = Device12();
            if (!d) return false;
            D3D12_COMMAND_QUEUE_DESC qd = {};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
            for (int i = 0; i < kLists; i++)
            {
                if (FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])))) return false;
                if (FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i]))))
                    return false;
                lists[i]->Close();
            }
            if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpuFence)))) return false;
            gpuEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&lastProgress);
            ComPtr<IDXGIFactory5> f5;
            BOOL allow = FALSE;
            if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f5))) &&
                SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                tearing = allow != FALSE;
            Log("DLSS FG: presenter ready, tearing %d", tearing ? 1 : 0);
            return true;
        }

        bool EnsureSwapchain(UINT w, UINT h, DXGI_FORMAT f)
        {
            HWND hwnd = childForPresenter.load();
            if (!hwnd) return false;
            if (swapchain && scW == w && scH == h && scFormat == f) return true;
            WaitGpuIdle();
            interp.Reset();
            if (swapchain && scFormat == f)
            {
                HRESULT hr = swapchain->ResizeBuffers(3, w, h, f, tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                if (FAILED(hr)) swapchain.Reset();
            }
            if (!swapchain)
            {
                ComPtr<IDXGIFactory2> f2;
                if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f2)))) return false;
                DXGI_SWAP_CHAIN_DESC1 sd = {};
                sd.Width = w;
                sd.Height = h;
                sd.Format = f;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 3;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
                ComPtr<IDXGISwapChain1> sc1;
                HRESULT hr = f2->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
                if (FAILED(hr) || FAILED(sc1.As(&swapchain)))
                {
                    LogE("DLSS FG: swap chain %ux%u %s failed 0x%08X", w, h, FormatName(f).c_str(), static_cast<unsigned>(hr));
                    return false;
                }
            }
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
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(Device12()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           nullptr, IID_PPV_ARGS(&interp))))
            {
                LogE("DLSS FG: interpolation target failed");
                return false;
            }
            scW = w;
            scH = h;
            scFormat = f;
            Log("DLSS FG: swap chain %ux%u %s on the child window", w, h, FormatName(f).c_str());
            return true;
        }

        bool EnsureFeature(ID3D12GraphicsCommandList* l, UINT w, UINT h, UINT rw, UINT rh)
        {
            if (feature && featureW == w && featureH == h && featureRW == rw && featureRH == rh) return true;
            std::lock_guard<std::mutex> guard(G().lock);
            if (feature)
            {
                WaitGpuIdle();
                NVSDK_NGX_D3D12_ReleaseFeature(feature);
                feature = nullptr;
            }
            if (!params && NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&params))) return false;
            NVSDK_NGX_DLSSG_Create_Params cp = {};
            cp.Width = w;
            cp.Height = h;
            cp.NativeBackbufferFormat = static_cast<unsigned>(scFormat);
            cp.RenderWidth = rw;
            cp.RenderHeight = rh;
            cp.DynamicResolutionScaling = false;
            NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSSG(l, 1, 1, &feature, params, &cp);
            Set(kKeyFgLastResult, static_cast<int>(r));
            if (NVSDK_NGX_FAILED(r))
            {
                feature = nullptr;
                LogE("DLSS FG: create feature failed 0x%08X", static_cast<unsigned>(r));
                return false;
            }
            featureW = w;
            featureH = h;
            featureRW = rw;
            featureRH = rh;
            havePrevious = false;
            LogC("DLSS FG: frame generation created %ux%u (depth/motion %ux%u)", w, h, rw, rh);
            return true;
        }

        void WaitUntil(const LARGE_INTEGER& t)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double left = Ms(now, t);
            if (left <= 0) return;
            if (left > 1.5 && timer)
            {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((left - 1.0) * 10000.0);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 100);
            }
            do { YieldProcessor(); QueryPerformanceCounter(&now); } while (Ms(now, t) > 0);
        }

        void Present(bool generated)
        {
            FG_TRACE("DLSS FG trace: present %s", generated ? "generated" : "real");
            HRESULT hr = swapchain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
            FG_TRACE("DLSS FG trace: present -> 0x%08X", static_cast<unsigned>(hr));
            if (FAILED(hr))
            {
                if (G().values[kKeyFgFails].fetch_add(1) < 5) LogE("DLSS FG: present failed 0x%08X", static_cast<unsigned>(hr));
                return;
            }
            G().values[generated ? kKeyFgGenerated : kKeyFgReal].fetch_add(1);
        }

        void CopyToBackbuffer(ID3D12GraphicsCommandList* l, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState)
        {
            ComPtr<ID3D12Resource> bb;
            swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
            Transition(l, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(l, bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyResource(bb.Get(), src);
            Transition(l, bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            Transition(l, src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
        }

        void WriteBmp(const std::wstring& path, UINT w, UINT h, const uint8_t* data, size_t pitch, bool bgra)
        {
            FILE* file = nullptr;
            if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return;
            BITMAPFILEHEADER fh = {};
            BITMAPINFOHEADER ih = {};
            ih.biSize = sizeof(ih);
            ih.biWidth = static_cast<LONG>(w);
            ih.biHeight = -static_cast<LONG>(h);
            ih.biPlanes = 1;
            ih.biBitCount = 24;
            UINT row = (w * 3 + 3) & ~3u;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + row * h;
            fwrite(&fh, sizeof(fh), 1, file);
            fwrite(&ih, sizeof(ih), 1, file);
            std::vector<uint8_t> line(row, 0);
            for (UINT y = 0; y < h; y++)
            {
                const uint8_t* s = data + static_cast<size_t>(y) * pitch;
                for (UINT x = 0; x < w; x++)
                {
                    line[x * 3 + 0] = bgra ? s[x * 4 + 0] : s[x * 4 + 2];
                    line[x * 3 + 1] = s[x * 4 + 1];
                    line[x * 3 + 2] = bgra ? s[x * 4 + 2] : s[x * 4 + 0];
                }
                fwrite(line.data(), 1, row, file);
            }
            fclose(file);
        }

        // Render thread: the game's back buffer as the bridge copies it (does the picture reach the bridge?).
        int dump11Count = 0;

        void Dump11(ID3D11Texture2D* bb)
        {
            D3D11_TEXTURE2D_DESC d;
            bb->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            d.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> st;
            HRESULT hr = G().device11->CreateTexture2D(&d, nullptr, &st);
            if (FAILED(hr)) { Log("DLSS FG: dump: staging texture failed 0x%08X", static_cast<unsigned>(hr)); return; }
            ComPtr<ID3D11DeviceContext> ctx;
            G().device11->GetImmediateContext(&ctx);
            ctx->CopyResource(st.Get(), bb);
            D3D11_MAPPED_SUBRESOURCE m;
            hr = ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m);
            if (FAILED(hr)) { Log("DLSS FG: dump: map failed 0x%08X", static_cast<unsigned>(hr)); return; }
            WriteBmp(dumpFolder + L"\\fg" + std::to_wstring(++dump11Count) + L"_game.bmp", d.Width, d.Height,
                     static_cast<const uint8_t*>(m.pData), m.RowPitch, d.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
            ctx->Unmap(st.Get(), 0);
        }

        void SaveBmp(ID3D12Resource* tex, D3D12_RESOURCE_STATES state, const std::wstring& path)
        {
            ID3D12Device* d = Device12();
            D3D12_RESOURCE_DESC rd = tex->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
            UINT64 total = 0;
            d->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> buf;
            if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf))))
                return;
            ID3D12GraphicsCommandList* l = BeginList();
            Transition(l, tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = buf.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;
            src.pResource = tex;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(l, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            Submit(l);
            WaitGpuIdle();
            UINT w = static_cast<UINT>(rd.Width), h = rd.Height;
            uint8_t* p = nullptr;
            if (FAILED(buf->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return;
            WriteBmp(path, w, h, p + fp.Offset, fp.Footprint.RowPitch, rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
            buf->Unmap(0, nullptr);
        }

        // Own, unshared copies of the frame for DLSS-G (NGX gets plain committed resources; the shared ones are only
        // copied from, through implicit state promotion) and for presenting the real frame. Colour ping-pongs so the
        // dump still has the previous frame.
        ComPtr<ID3D12Resource> colorIn[2], depthIn, motionIn, hudlessIn;
        int colorNext = 0;
        std::atomic<bool> generateOn{true};
        std::atomic<bool> hudlessOn{true};

        bool MakeOwn(ComPtr<ID3D12Resource>& r, UINT w, UINT h, DXGI_FORMAT f)
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
            return SUCCEEDED(Device12()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                 nullptr, IID_PPV_ARGS(&r)));
        }

        bool EnsureOwn()
        {
            D3D12_RESOURCE_DESC c = colorIn[0] ? colorIn[0]->GetDesc() : D3D12_RESOURCE_DESC{};
            D3D12_RESOURCE_DESC d = depthIn ? depthIn->GetDesc() : D3D12_RESOURCE_DESC{};
            if (colorIn[0] && c.Width == sharedW && c.Height == sharedH && c.Format == sharedFormat && d.Width == motionW && d.Height == motionH)
                return true;
            WaitGpuIdle();
            bool ok = MakeOwn(colorIn[0], sharedW, sharedH, sharedFormat) && MakeOwn(colorIn[1], sharedW, sharedH, sharedFormat) &&
                      MakeOwn(depthIn, motionW, motionH, DXGI_FORMAT_R32_FLOAT) && MakeOwn(motionIn, motionW, motionH, DXGI_FORMAT_R16G16_FLOAT) &&
                      MakeOwn(hudlessIn, sharedW, sharedH, sharedFormat);
            if (!ok)
            {
                LogE("DLSS FG: own textures failed");
                colorIn[0].Reset();
                return false;
            }
            havePrevious = false;
            return true;
        }

        // ---------------------------------------------------------------- presenter thread: the FSR path

        // The DLSS path's swap chain off the child window (the FSR proxy needs the window to itself, and back).
        void DropDlssSwapchain()
        {
            if (!swapchain) return;
            WaitGpuIdle();
            interp.Reset();
            swapchain.Reset();
            scW = scH = 0;
            Log("DLSS FG: DLSS swap chain released");
        }

        // FSR's frame generation context: disabled on the proxy first (that waits for the proxy's presents).
        void DropFsrFg()
        {
            if (!fsrFgCtx) return;
            ffxConfigureDescFrameGeneration cfg = {};
            cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            cfg.swapChain = fsrProxy.Get();
            cfg.frameGenerationEnabled = false;
            Ffx().Configure(&fsrFgCtx, &cfg.header);
            WaitGpuIdle();
            Ffx().DestroyContext(&fsrFgCtx, nullptr);
            fsrFgCtx = nullptr;
            fsrPrepared = false;
        }

        void DropFsr()
        {
            DropFsrFg();
            if (!fsrProxy && !fsrSwapCtx) return;
            WaitGpuIdle();
            if (fsrSwapCtx) Ffx().DestroyContext(&fsrSwapCtx, nullptr);
            fsrSwapCtx = nullptr;
            fsrGenSince = 0;
            fsrRealSince = 0;
            // The last reference: the proxy's destructor stops its threads and releases the real swap chain.
            ULONG refs = fsrProxy ? fsrProxy.Reset() : 0;
            fsrW = fsrH = fsrRW = fsrRH = 0;
            Log("DLSS FG: FSR swap chain released (references left %lu)", refs);
        }

        ffxReturnCode_t FsrGenerate(ffxDispatchDescFrameGeneration* desc, void* user)
        {
            // Called by the proxy inside its Present, on this thread: the interpolation into its own target.
            ffxReturnCode_t r = Ffx().Dispatch(static_cast<ffxContext*>(user), &desc->header);
            if (r != FFX_API_RETURN_OK)
            {
                Set(kKeyFsrFgLastResult, static_cast<int>(r));
                if (G().values[kKeyFgFails].fetch_add(1) < 5) LogE("DLSS FG: FSR frame generation failed (%u)", r);
            }
            return r;
        }

        // The proxy swap chain on the child window (back buffer size) and the frame generation context (render size of
        // depth and motion).
        bool EnsureFsr(UINT w, UINT h, DXGI_FORMAT f, UINT rw, UINT rh)
        {
            HWND hwnd = childForPresenter.load();
            if (!hwnd || !FsrFgReady()) return false;
            bool sameSc = fsrProxy && fsrW == w && fsrH == h && fsrFormat == f;
            if (sameSc && fsrFgCtx && fsrRW == rw && fsrRH == rh) return true;
            if (fsrFailed && fsrW == w && fsrH == h && fsrFormat == f && fsrRW == rw && fsrRH == rh) return false;
            if (!sameSc) DropFsr();
            else DropFsrFg();
            fsrW = w;
            fsrH = h;
            fsrFormat = f;
            fsrRW = rw;
            fsrRH = rh;
            fsrFailed = true;
            if (!fsrProxy)
            {
                DropDlssSwapchain();
                WaitGpuIdle();
                ComPtr<IDXGIFactory2> f2;
                if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f2)))) return false;
                DXGI_SWAP_CHAIN_DESC1 sd = {};
                sd.Width = w;
                sd.Height = h;
                sd.Format = f;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2; // the proxy keeps its own buffers for the real swap chain
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
                IDXGISwapChain4* sc = nullptr;
                ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version = {};
                version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
                version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
                ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 cd = {};
                cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
                cd.header.pNext = &version.header;
                cd.swapchain = &sc;
                cd.hwnd = hwnd;
                cd.desc = &sd;
                cd.fullscreenDesc = nullptr;
                cd.dxgiFactory = f2.Get();
                cd.gameQueue = queue.Get();
                ffxReturnCode_t r = Ffx().CreateContext(&fsrSwapCtx, &cd.header, nullptr);
                Set(kKeyFsrFgLastResult, static_cast<int>(r));
                if (r != FFX_API_RETURN_OK || !sc)
                {
                    fsrSwapCtx = nullptr;
                    LogE("DLSS FG: FSR swap chain %ux%u %s failed (%u)", w, h, FormatName(f).c_str(), r);
                    return false;
                }
                fsrProxy.Attach(sc);
                std::string v = FfxProviderVersion(&fsrSwapCtx);
                SetText(kTextFsrScActive, v);
                Log("DLSS FG: FSR swap chain %s %ux%u %s on the child window", v.c_str(), w, h, FormatName(f).c_str());
            }
            ffxCreateBackendDX12Desc backend = {};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            backend.device = Device12();
            ffxCreateContextDescFrameGenerationVersion version = {};
            version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
            version.header.pNext = &backend.header;
            version.version = FFX_FRAMEGENERATION_VERSION;
            ffxCreateContextDescFrameGeneration cd = {};
            cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
            cd.header.pNext = &version.header;
            cd.flags = FsrDebug() ? FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0u; // depth 0 near .. 1 far, LDR colour
            cd.displaySize = {w, h};
            cd.maxRenderSize = {rw, rh};
            cd.backBufferFormat = ffxApiGetSurfaceFormatDX12(f);
            ffxReturnCode_t r = Ffx().CreateContext(&fsrFgCtx, &cd.header, nullptr);
            Set(kKeyFsrFgLastResult, static_cast<int>(r));
            if (r != FFX_API_RETURN_OK)
            {
                fsrFgCtx = nullptr;
                LogE("DLSS FG: FSR frame generation %ux%u (depth/motion %ux%u) failed (%u)", w, h, rw, rh, r);
                return false;
            }
            fsrFailed = false;
            fsrPrepared = false;
            std::string v = FfxProviderVersion(&fsrFgCtx);
            SetText(kTextFsrFgActive, v);
            LogC("DLSS FG: FSR frame generation %s created %ux%u (depth/motion %ux%u)", v.c_str(), w, h, rw, rh);
            return true;
        }

        void ProcessFsr(const Pending& f, bool catchUp)
        {
            Slot& s = slots[f.slot];
            const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            bool generate = mode.load() > 0 && f.hasCamera && !catchUp && generateOn.load();
            ID3D12GraphicsCommandList* l = BeginList();
            // The real frame into the proxy's back buffer; depth, motion and the HUD-less picture into our textures
            // (with frame generation on the game's queue, they are read before the next frame overwrites them).
            ComPtr<ID3D12Resource> bb;
            fsrProxy->GetBuffer(fsrProxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
            if (bb)
            {
                Transition(l, bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(bb.Get(), s.color.r12.Get());
                Transition(l, bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            }
            bool hudless = generate && f.hasHudless && hudlessOn.load();
            if (generate)
            {
                Transition(l, depthIn.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
                Transition(l, motionIn.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(depthIn.Get(), s.depth.r12.Get());
                l->CopyResource(motionIn.Get(), s.motion.r12.Get());
                Transition(l, depthIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
                Transition(l, motionIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
                if (hudless)
                {
                    Transition(l, hudlessIn.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
                    l->CopyResource(hudlessIn.Get(), s.hudless.r12.Get());
                    Transition(l, hudlessIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
                }
            }

            uint64_t id = ++fsrFrameId;
            ffxConfigureDescFrameGeneration cfg = {};
            cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            cfg.swapChain = fsrProxy.Get();
            cfg.frameGenerationCallback = FsrGenerate;
            cfg.frameGenerationCallbackUserContext = &fsrFgCtx;
            cfg.frameGenerationEnabled = generate;
            cfg.allowAsyncWorkloads = false; // on our queue: the inputs above stay valid until it ran
            // The HUD-less picture marks the HUD, the hands and the UI as screen-fixed.
            if (hudless) cfg.HUDLessColor = ffxApiGetResourceDX12(hudlessIn.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            cfg.flags = 0;
            cfg.onlyPresentGenerated = false;
            cfg.generationRect = {0, 0, static_cast<int32_t>(fsrW), static_cast<int32_t>(fsrH)};
            cfg.frameID = id;
            ffxReturnCode_t r = Ffx().Configure(&fsrFgCtx, &cfg.header);
            if (r != FFX_API_RETURN_OK)
            {
                Set(kKeyFsrFgLastResult, static_cast<int>(r));
                if (G().values[kKeyFgFails].fetch_add(1) < 5) LogE("DLSS FG: FSR configure failed (%u)", r);
            }
            bool reset = !fsrPrepared || f.camera.reset;
            fsrPrepared = false;
            if (generate)
            {
                ffxDispatchDescFrameGenerationPrepareV2 pd = {};
                pd.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
                pd.frameID = id;
                pd.flags = 0;
                pd.commandList = l;
                pd.renderSize = {motionW, motionH};
                pd.jitterOffset = {f.camera.jitter[0], f.camera.jitter[1]};
                pd.motionVectorScale = {1.0f, 1.0f}; // pixels
                pd.frameTimeDelta = static_cast<float>(intervalMs);
                pd.reset = reset;
                pd.cameraNear = f.camera.nearZ;
                pd.cameraFar = f.camera.farZ;
                pd.cameraFovAngleVertical = f.camera.fov;
                pd.viewSpaceToMetersFactor = 1.0f;
                pd.depth = ffxApiGetResourceDX12(depthIn.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
                pd.motionVectors = ffxApiGetResourceDX12(motionIn.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
                memcpy(pd.cameraPosition, f.camera.pos, sizeof(pd.cameraPosition));
                memcpy(pd.cameraUp, f.camera.up, sizeof(pd.cameraUp));
                memcpy(pd.cameraRight, f.camera.right, sizeof(pd.cameraRight));
                memcpy(pd.cameraForward, f.camera.fwd, sizeof(pd.cameraForward));
                r = Ffx().Dispatch(&fsrFgCtx, &pd.header);
                if (r == FFX_API_RETURN_OK) fsrPrepared = true;
                else
                {
                    Set(kKeyFsrFgLastResult, static_cast<int>(r));
                    if (G().values[kKeyFgFails].fetch_add(1) < 5) LogE("DLSS FG: FSR prepare failed (%u)", r);
                }
            }
            Submit(l);
            queue->Signal(free12.Get(), f.no);

            FG_TRACE("DLSS FG trace: FSR present (generate %d)", generate ? 1 : 0);
            HRESULT hr = fsrProxy->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
            FG_TRACE("DLSS FG trace: FSR present -> 0x%08X", static_cast<unsigned>(hr));
            if (FAILED(hr))
            {
                if (G().values[kKeyFgFails].fetch_add(1) < 5) LogE("DLSS FG: FSR present failed 0x%08X", static_cast<unsigned>(hr));
                return;
            }
            G().values[kKeyFgReal].fetch_add(1);
            fsrRealSince++;
            // Generated frames: the real swap chain's presents (the proxy presents real and generated) minus ours.
            // The counter is shared with the DLSS path: only what this proxy added since the last frame.
            UINT presents = 0;
            if (SUCCEEDED(fsrProxy->GetLastPresentCount(&presents)))
            {
                int gen = static_cast<int>(presents) - static_cast<int>(fsrRealSince);
                if (gen > fsrGenSince)
                {
                    G().values[kKeyFgGenerated].fetch_add(gen - fsrGenSince);
                    fsrGenSince = gen;
                }
            }
            havePrevious = f.hasCamera;
            if (generate) featureFrames++;
        }

        void Process(const Pending& f, bool catchUp)
        {
            Slot& s = slots[f.slot];
            const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            FG_TRACE("DLSS FG trace: frame %llu slot %d camera %d catch-up %d", static_cast<unsigned long long>(f.no), f.slot,
                     f.hasCamera ? 1 : 0, catchUp ? 1 : 0);
            queue->Wait(ready12.Get(), f.no);
            // The path wanted: the other one's swap chain leaves the child window first.
            int k0 = kind.load();
            if (k0 != activeKind)
            {
                if (k0 == kFgFsr) DropDlssSwapchain();
                else DropFsr();
                activeKind = k0;
                havePrevious = false;
                Set(kKeyFgKind, k0);
                Log("DLSS FG: path %s", k0 == kFgFsr ? "FSR" : "DLSS");
            }
            bool fsr = k0 == kFgFsr;
            bool ready = fsr ? EnsureOwn() && EnsureFsr(sharedW, sharedH, sharedFormat, motionW, motionH)
                             : EnsureSwapchain(sharedW, sharedH, sharedFormat) && EnsureOwn();
            if (!ready)
            {
                queue->Signal(free12.Get(), f.no);
                return;
            }
            if (lastArrival.QuadPart)
            {
                double dt = Ms(lastArrival, f.time);
                if (dt > 0.5 && dt < 250) intervalMs = intervalMs * 0.9 + dt * 0.1;
            }
            lastArrival = f.time;
            Set(kKeyFgIntervalUs, static_cast<int>(intervalMs * 1000.0));
            if (fsr)
            {
                ProcessFsr(f, catchUp);
                return;
            }

            int k = mode.load();
            int maxK = Get(kKeyFgMultiFrameMax);
            if (maxK < 1) maxK = 1;
            if (k > maxK) k = maxK;
            bool generate = k > 0 && f.hasCamera && !catchUp && generateOn.load() && Ngx12Ready();

            // The frame into our own textures; the slot goes back to the game's side right away.
            int ci = colorNext;
            colorNext ^= 1;
            ID3D12Resource* color = colorIn[ci].Get();
            {
                ID3D12GraphicsCommandList* l = BeginList();
                Transition(l, color, read, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(color, s.color.r12.Get());
                Transition(l, color, D3D12_RESOURCE_STATE_COPY_DEST, read);
                if (generate)
                {
                    Transition(l, depthIn.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
                    Transition(l, motionIn.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
                    Transition(l, hudlessIn.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
                    l->CopyResource(depthIn.Get(), s.depth.r12.Get());
                    l->CopyResource(motionIn.Get(), s.motion.r12.Get());
                    if (f.hasHudless) l->CopyResource(hudlessIn.Get(), s.hudless.r12.Get());
                    Transition(l, depthIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
                    Transition(l, motionIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
                    Transition(l, hudlessIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
                    if (!EnsureFeature(l, sharedW, sharedH, motionW, motionH)) generate = false;
                }
                Submit(l);
                queue->Signal(free12.Get(), f.no);
            }

            LARGE_INTEGER first = {};
            bool reset = !havePrevious || f.camera.reset;
            if (generate)
            {
                for (int i = 1; i <= k; i++)
                {
                    ID3D12GraphicsCommandList* l = BeginList();
                    NVSDK_NGX_D3D12_DLSSG_Eval_Params ep = {};
                    ep.pBackbuffer = color;
                    ep.pDepth = depthIn.Get();
                    ep.pMVecs = motionIn.Get();
                    ep.pOutputInterpFrame = interp.Get();
                    // The HUD-less picture marks the HUD, the hands and the UI as screen-fixed (same size only).
                    if (hudlessOn.load() && f.hasHudless) ep.pHudless = hudlessIn.Get();
                    NVSDK_NGX_DLSSG_Opt_Eval_Params op{};
                    op.multiFrameCount = static_cast<unsigned>(k);
                    op.multiFrameIndex = static_cast<unsigned>(i);
                    memcpy(op.cameraViewToClip, f.camera.viewToClip, sizeof(op.cameraViewToClip));
                    memcpy(op.clipToCameraView, f.camera.clipToView, sizeof(op.clipToCameraView));
                    memcpy(op.clipToPrevClip, f.camera.clipToPrevClip, sizeof(op.clipToPrevClip));
                    memcpy(op.prevClipToClip, f.camera.prevClipToClip, sizeof(op.prevClipToClip));
                    for (int r = 0; r < 4; r++)
                        for (int c = 0; c < 4; c++) op.clipToLensClip[r][c] = r == c ? 1.0f : 0.0f;
                    op.jitterOffset[0] = f.camera.jitter[0];
                    op.jitterOffset[1] = f.camera.jitter[1];
                    op.mvecScale[0] = 1.0f;
                    op.mvecScale[1] = 1.0f;
                    memcpy(op.cameraPos, f.camera.pos, sizeof(op.cameraPos));
                    memcpy(op.cameraUp, f.camera.up, sizeof(op.cameraUp));
                    memcpy(op.cameraRight, f.camera.right, sizeof(op.cameraRight));
                    memcpy(op.cameraFwd, f.camera.fwd, sizeof(op.cameraFwd));
                    op.cameraNear = f.camera.nearZ;
                    op.cameraFar = f.camera.farZ;
                    op.cameraFOV = f.camera.fov;
                    op.cameraAspectRatio = f.camera.aspect;
                    op.colorBuffersHDR = false;
                    op.depthInverted = false;
                    op.cameraMotionIncluded = true;
                    op.reset = reset;
                    op.notRenderingGameFrames = false;
                    op.orthoProjection = false;
                    op.motionVectorsDilated = false;
                    op.menuDetectionEnabled = false;
                    NVSDK_NGX_Result r;
                    FG_TRACE("DLSS FG trace: evaluate %d/%d reset %d", i, k, reset ? 1 : 0);
                    {
                        std::lock_guard<std::mutex> guard(G().lock);
                        r = NGX_D3D12_EVALUATE_DLSSG(l, feature, params, &ep, &op);
                    }
                    FG_TRACE("DLSS FG trace: evaluate -> 0x%08X", static_cast<unsigned>(r));
                    Set(kKeyFgLastResult, static_cast<int>(r));
                    if (NVSDK_NGX_FAILED(r))
                    {
                        if (G().values[kKeyFgFails].fetch_add(1) < 5) LogE("DLSS FG: evaluate failed 0x%08X", static_cast<unsigned>(r));
                        Submit(l);
                        generate = false;
                        break;
                    }
                    CopyToBackbuffer(l, interp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    Submit(l);
                    if (i == 1)
                    {
                        // NGX's GPU work completes within ~2 ms; if it does not, it hangs the device (TDR): stop.
                        if (traced < 4)
                        {
                            WaitGpuIdle();
                            FG_TRACE("DLSS FG trace: GPU done (fence %llu)", static_cast<unsigned long long>(gpuFence->GetCompletedValue()));
                        }
                        if (dumpWanted.load() && havePrevious)
                        {
                            dumpWanted = false;
                            static int dumps = 0;
                            std::wstring base = dumpFolder + L"\\fg" + std::to_wstring(++dumps);
                            SaveBmp(colorIn[ci ^ 1].Get(), read, base + L"_0_previous.bmp");
                            SaveBmp(interp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, base + L"_1_generated.bmp");
                            SaveBmp(color, read, base + L"_2_current.bmp");
                            SaveBmp(s.color.r12.Get(), D3D12_RESOURCE_STATE_COMMON, base + L"_3_shared.bmp");
                            LogC("DLSS FG: dump %d written (%d of %d generated frames)", dumps, i, k);
                        }
                        QueryPerformanceCounter(&first);
                    }
                    else
                    {
                        LARGE_INTEGER t = first;
                        t.QuadPart += static_cast<LONGLONG>(intervalMs * (i - 1) / (k + 1) * freq.QuadPart / 1000.0);
                        WaitUntil(t);
                    }
                    Present(true);
                }
            }
            ID3D12GraphicsCommandList* l = BeginList();
            CopyToBackbuffer(l, color, read);
            Submit(l);
            if (generate)
            {
                LARGE_INTEGER t = first;
                t.QuadPart += static_cast<LONGLONG>(intervalMs * k / (k + 1) * freq.QuadPart / 1000.0);
                WaitUntil(t);
            }
            Present(false);
            havePrevious = f.hasCamera;
            if (generate) featureFrames++;
        }

        // ---------------------------------------------------------------- the child window
        // The child window belongs to a thread of its own. DXGI talks to a swap chain's window synchronously from
        // Present; had the game's main thread owned it, Present would wait for that thread, which waits for the
        // render thread, which (game swap chain, DXGI locks) can wait for us. Nor can it be the presenter: the FSR
        // proxy presents from its own threads while the presenter waits inside the proxy's Present. A thread that only
        // pumps the window's messages: no cycle. The main thread only says what it wants (visible or not).

        std::atomic<bool> wantVisible{false};
        std::atomic<HWND> gameForPresenter{nullptr};

        LRESULT CALLBACK ChildProc(HWND h, UINT msg, WPARAM w, LPARAM l)
        {
            switch (msg)
            {
            case WM_NCHITTEST: return HTTRANSPARENT;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: ValidateRect(h, nullptr); return 0;
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            default: return DefWindowProcW(h, msg, w, l);
            }
        }

        void Pump()
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        // Creates, sizes, shows and hides the child (window thread only).
        void UpdateChild()
        {
            HWND game = gameForPresenter.load();
            if (!game) return;
            if (!childHwnd)
            {
                WNDCLASSW wc = {};
                wc.lpfnWndProc = ChildProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"MySummerFramesFrameGeneration";
                RegisterClassW(&wc);
                RECT rc;
                GetClientRect(game, &rc);
                // Disabled: the system gives mouse input to the game window under it (whatever thread owns it).
                childHwnd = CreateWindowExW(WS_EX_NOPARENTNOTIFY, wc.lpszClassName, L"", WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0,
                                            rc.right - rc.left, rc.bottom - rc.top, game, nullptr, wc.hInstance, nullptr);
                if (!childHwnd)
                {
                    LogE("DLSS FG: child window failed (%lu)", GetLastError());
                    stage = -1;
                    return;
                }
                gameHwnd = game;
                childForPresenter = childHwnd;
                Log("DLSS FG: child window %p on %p (%ldx%ld)", static_cast<void*>(childHwnd), static_cast<void*>(game),
                    rc.right - rc.left, rc.bottom - rc.top);
            }
            RECT pr, cr;
            GetClientRect(gameHwnd, &pr);
            GetClientRect(childHwnd, &cr);
            if (pr.right != cr.right || pr.bottom != cr.bottom)
                SetWindowPos(childHwnd, nullptr, 0, 0, pr.right, pr.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
            bool want = wantVisible.load();
            if (want != childShown)
            {
                ShowWindow(childHwnd, want ? SW_SHOWNA : SW_HIDE);
                childShown = want;
                if (want && mode.load() > 0)
                {
                    stage = 2;
                    LogC("DLSS FG: bridge on (its window covers the game's picture)");
                }
            }
        }

        void PresenterMain()
        {
            if (!PresenterSetup())
            {
                LogE("DLSS FG: D3D12 presenter setup failed");
                stage = -1;
                return;
            }
            LARGE_INTEGER lastStats;
            QueryPerformanceCounter(&lastStats);
            int lastReal = 0, lastGen = 0;
            for (;;)
            {
                Pending f;
                bool catchUp = false, have = false;
                {
                    std::lock_guard<std::mutex> lk(fgLock);
                    if (quit && pending.empty()) break;
                    if (!pending.empty())
                    {
                        f = pending.front();
                        pending.pop_front();
                        catchUp = !pending.empty();
                        have = true;
                    }
                }
                if (!have)
                {
                    WaitForSingleObject(frameEvent, 20);
                    continue;
                }
                Process(f, catchUp);
                traced++;
                QueryPerformanceCounter(&lastProgress);
                processed.store(f.no);
                fgCv.notify_all();

                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                double ms = Ms(lastStats, now);
                if (ms > 5000)
                {
                    int real = Get(kKeyFgReal), gen = Get(kKeyFgGenerated);
                    Log("DLSS FG: %.1f real + %.1f generated frames/s, real interval %.2f ms, %s, mode %d, stage %d",
                        (real - lastReal) * 1000.0 / ms, (gen - lastGen) * 1000.0 / ms, intervalMs,
                        activeKind == kFgFsr ? "FSR" : "DLSS", mode.load(), stage.load());
                    lastReal = real;
                    lastGen = gen;
                    lastStats = now;
                }
            }
            WaitGpuIdle();
            DropFsr();
            interp.Reset();
            swapchain.Reset();
            activeKind = 0;
            Set(kKeyFgKind, 0);
        }

        // The child window's thread: creates it, keeps it on the game window's client area, shows / hides it, and
        // pumps its messages; it waits for nothing else.
        void WindowMain()
        {
            while (!windowQuit.load())
            {
                UpdateChild();
                Pump();
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            }
            childForPresenter = nullptr;
            if (childHwnd) DestroyWindow(childHwnd);
            childHwnd = nullptr;
            childShown = false;
            Pump();
        }

        void StopThreads()
        {
            if (presenter.joinable())
            {
                {
                    std::lock_guard<std::mutex> lk(fgLock);
                    quit = true;
                }
                fgCv.notify_all();
                SetEvent(frameEvent);
                presenter.join();
            }
            // The swap chains are gone with the presenter: now the window.
            if (windowThread.joinable())
            {
                windowQuit = true;
                windowThread.join();
            }
        }

        BOOL CALLBACK FindGame(HWND h, LPARAM out)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            wchar_t cls[64];
            if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetClassNameW(h, cls, 64) && wcscmp(cls, L"UnityWndClass") == 0)
            {
                *reinterpret_cast<HWND*>(out) = h;
                return FALSE;
            }
            return TRUE;
        }
    }

    void SetFgMode(int k, int generatedFrames)
    {
        if (generatedFrames < 0) generatedFrames = 0;
        if (k != kFgFsr) k = kFgDlss;
        const char* name = k == kFgFsr ? "FSR" : "DLSS";
        if (generatedFrames > 0 && !(k == kFgFsr ? FsrFgReady() : Ngx12Ready()))
        {
            LogE("DLSS FG: %s frame generation is not available on this system", name);
            return;
        }
        if (k == kFgFsr && generatedFrames > 1) generatedFrames = 1; // FSR generates one frame between two
        if (generatedFrames > 0 && !presenter.joinable())
        {
            quit = false;
            windowQuit = false;
            if (!frameEvent) frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            windowThread = std::thread(WindowMain);
            presenter = std::thread(PresenterMain);
        }
        // Off -> on, or the other path while on: the game's picture shows until the bridge presents again.
        if (generatedFrames > 0 && (mode.load() == 0 || k != kind.load()))
        {
            visibleAfter = Get(kKeyFgReal) + 10;
            if (mode.load() > 0)
            {
                wantVisible = false;
                if (stage.load() > 0) stage = 1;
            }
        }
        kind = k;
        mode = generatedFrames;
        LogC("DLSS FG: %s", generatedFrames > 0 ? (std::string(name) + " on, " + std::to_string(generatedFrames + 1) + "x").c_str() : "off");
    }

    int FgWindow()
    {
        int m = mode.load();
        if (stage.load() < 0) return -1;
        if (!gameForPresenter.load())
        {
            HWND game = nullptr;
            EnumWindows(FindGame, reinterpret_cast<LPARAM>(&game));
            gameForPresenter = game;
        }
        if (m > 0)
        {
            // Warm-up (the child hidden) until ours has presented a few frames, then the child is shown (by the
            // window thread) over the game's picture.
            if (stage.load() == 0 && childForPresenter.load()) stage = 1;
            if (Get(kKeyFgReal) > visibleAfter.load() && Get(kKeyFgKind) == kind.load()) wantVisible = true;
        }
        else if (stage.load() > 0 || wantVisible.load())
        {
            stage = 0; // the game presents again first, then the child goes away
            wantVisible = false;
        }
        return stage.load();
    }

    // Render thread, end of the frame (the mod's WaitForEndOfFrame event).
    void CaptureFg()
    {
        if (mode.load() > 0 && stage.load() >= 1) Feed();
    }

    void SetFgGenerate(bool on)
    {
        generateOn = on;
        LogC("DLSS FG: generation %s (the bridge presents real frames only when off)", on ? "on" : "off");
    }

    void SetFgHudless(bool on)
    {
        hudlessOn = on;
        LogC("DLSS FG: HUD-less input %s", on ? "on" : "off");
    }

    void RequestFgDump(const std::wstring& folder)
    {
        dumpFolder = folder;
        dump11Wanted = true;
        dumpWanted = true;
    }

    void StopFgThread()
    {
        mode = 0;
        stage = 0;
        if (!presenter.joinable() && !windowThread.joinable()) return;
        StopThreads();
        Log("DLSS FG: presenter and window threads stopped");
    }

    void ShutdownFg()
    {
        mode = 0;
        stage = 0;
        StopThreads();
        if (feature)
        {
            std::lock_guard<std::mutex> guard(G().lock);
            NVSDK_NGX_D3D12_ReleaseFeature(feature);
            feature = nullptr;
        }
        if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
        params = nullptr;
        swapchain.Reset();
        interp.Reset();
        colorIn[0].Reset();
        colorIn[1].Reset();
        depthIn.Reset();
        motionIn.Reset();
        hudlessIn.Reset();
        for (Slot& s : slots) s = Slot{};
        ready11.Reset();
        free11.Reset();
        ready12.Reset();
        free12.Reset();
        ctx4.Reset();
        queue.Reset();
        for (auto& a : allocators) a.Reset();
        for (auto& l : lists) l.Reset();
        gpuFence.Reset();
    }
}

namespace msf
{
    bool MakeSharedTexture(ID3D12Device* d12, ID3D11Device1* d11, UINT w, UINT h, DXGI_FORMAT f, SharedTex& t, const char* tag)
    {
        return MakeShared(d12, d11, w, h, f, t, tag);
    }
}
