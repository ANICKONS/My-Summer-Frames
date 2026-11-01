// My Summer Frames core: DLSS / DLAA on Unity's D3D11 device, on the render thread.
// Copyright (C) 2026 ANICKON
//
// Per frame (event kEventEvaluate + slot, issued from the main camera's OnRenderImage):
//   1. Unity's colour and depth copies -> our own typed textures (Unity's render textures are TYPELESS);
//   2. motion vectors from the depth and the camera matrices (motion.hlsl, compute shader);
//   3. optional reprojection test (how well the vectors predict the previous frame);
//   4. NGX DLSS evaluate, or AMD FSR through our D3D12 device (fsr.cpp) -> our output -> Unity's output render
//      texture (the mod blits it to the screen).
// The main thread writes the frame's inputs into one of kSlots slots first; the render thread may run a frame
// behind, so each event carries its slot. All our D3D11 state lives in our own context state object, swapped in and
// out around the work, so Unity's bindings are untouched.
#include "common.h"

#include "motion_cs.h"
#include "error_cs.h"
#include "flip_cs.h"
#include "hudless_cs.h"
#include "sharpen_cs.h"

namespace msf
{
    namespace
    {
        struct alignas(16) FrameConstants
        {
            float invViewProjJittered[16];
            float viewProj[16];
            float prevViewProj[16];
            float size[2];
            float mvSign[2];
            float options[4];
        };

        struct Tex
        {
            ComPtr<ID3D11Texture2D> tex;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11UnorderedAccessView> uav;
            void Reset() { tex.Reset(); srv.Reset(); uav.Reset(); }
        };

        std::mutex slotLock;
        FrameParams slots[kSlots];

        ComPtr<ID3D11Device1> dev1;
        ComPtr<ID3D11DeviceContext1> ctx1;
        ComPtr<ID3DDeviceContextState> ourState;
        ComPtr<ID3D11ComputeShader> motionCS, errorCS, flipCS, hudlessCS, sharpenCS;
        Tex fgDepth, fgMotion, fgHudless; // frame generation inputs (back buffer orientation)
        FgCamera fgCamera;
        bool fgFresh = false;
        ComPtr<ID3D11Buffer> bodyConstants;
        struct alignas(16) BodyConstants
        {
            uint32_t count;
            uint32_t pad[3];
            BodyData bodies[kMaxBodies];
        };
        static_assert(sizeof(BodyData) == 160, "BodyData must match motion.hlsl Body");
        ComPtr<ID3D11Buffer> constants;
        ComPtr<ID3D11SamplerState> linearClamp;

        Tex color, depth, motion, output, prevColor, sharp; // sharp: the output after the sharpening pass
        int texW = 0, texH = 0, texOW = 0, texOH = 0;
        DXGI_FORMAT texFormat = DXGI_FORMAT_UNKNOWN;
        bool prevValid = false;
        bool formatWarned = false;

        ComPtr<ID3D11Buffer> sums;
        ComPtr<ID3D11UnorderedAccessView> sumsUav;
        ComPtr<ID3D11Buffer> staging[3];
        bool stagingBusy[3] = {};
        int stagingNext = 0;
        unsigned long long testTotals[9] = {}; // warped, zero, pixels, mvx, mvy, outside, depth 0, depth 1, frames

        NVSDK_NGX_Handle* feature = nullptr;
        NVSDK_NGX_Parameter* params = nullptr;
        int featureW = 0, featureH = 0, featureOW = 0, featureOH = 0, featureQuality = -1;
        bool setupFailed = false;

        DXGI_FORMAT Typed(DXGI_FORMAT f)
        {
            switch (f)
            {
            case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
            case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            default: return f;
            }
        }

        // CopyResource needs both textures in the same format family.
        bool SameFamily(DXGI_FORMAT a, DXGI_FORMAT b)
        {
            auto family = [](DXGI_FORMAT f) -> int {
                switch (f)
                {
                case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 1;
                case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 2;
                case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: return 3;
                case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 4;
                case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return 5;
                default: return 100 + static_cast<int>(f);
                }
            };
            return family(a) == family(b);
        }

        bool TexDesc(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& d)
        {
            ComPtr<ID3D11Texture2D> t;
            if (!r || FAILED(r->QueryInterface(IID_PPV_ARGS(&t)))) return false;
            t->GetDesc(&d);
            return true;
        }

        bool MakeTex(Tex& t, int w, int h, DXGI_FORMAT f, bool uav, const char* tag)
        {
            t.Reset();
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = static_cast<UINT>(w);
            d.Height = static_cast<UINT>(h);
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = f;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
            ID3D11Device* device = G().device11;
            HRESULT hr = device->CreateTexture2D(&d, nullptr, &t.tex);
            if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
            if (SUCCEEDED(hr) && uav) hr = device->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav);
            if (FAILED(hr))
            {
                LogE("DLSS: texture %s %dx%d %s failed 0x%08X", tag, w, h, FormatName(f).c_str(), static_cast<unsigned>(hr));
                t.Reset();
                return false;
            }
            return true;
        }

        // w x h: render size (inputs, motion); ow x oh: DLSS output (the screen).
        bool EnsureTextures(int w, int h, int ow, int oh, DXGI_FORMAT colorFormat)
        {
            if (color.tex && texW == w && texH == h && texOW == ow && texOH == oh && texFormat == colorFormat) return true;
            texW = texH = texOW = texOH = 0;
            bool ok = MakeTex(color, w, h, colorFormat, false, "colour") &&
                      MakeTex(output, ow, oh, colorFormat, true, "output") &&
                      MakeTex(prevColor, w, h, colorFormat, false, "previous colour") &&
                      MakeTex(depth, w, h, DXGI_FORMAT_R32_FLOAT, false, "depth") &&
                      MakeTex(motion, w, h, DXGI_FORMAT_R16G16_FLOAT, true, "motion") &&
                      MakeTex(sharp, ow, oh, colorFormat, true, "sharpened") &&
                      MakeTex(fgDepth, w, h, DXGI_FORMAT_R32_FLOAT, true, "fg depth") &&
                      MakeTex(fgMotion, w, h, DXGI_FORMAT_R16G16_FLOAT, true, "fg motion") &&
                      MakeTex(fgHudless, ow, oh, colorFormat, true, "fg hudless");
            if (!ok) return false;
            texW = w;
            texH = h;
            texOW = ow;
            texOH = oh;
            texFormat = colorFormat;
            prevValid = false;
            Log("DLSS: textures %dx%d -> %dx%d colour %s", w, h, ow, oh, FormatName(colorFormat).c_str());
            return true;
        }

        bool Setup(ID3D11Device* device)
        {
            if (ourState) return true;
            if (setupFailed) return false;
            setupFailed = true;
            ComPtr<ID3D11DeviceContext> ctx;
            device->GetImmediateContext(&ctx);
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dev1))) || FAILED(ctx.As(&ctx1)))
            {
                LogE("DLSS: Direct3D 11.1 interfaces missing");
                return false;
            }
            D3D_FEATURE_LEVEL fl = device->GetFeatureLevel();
            // Unity creates its device single-threaded (one render thread): the state object must say so too.
            UINT creation = device->GetCreationFlags();
            UINT stateFlags = (creation & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
            HRESULT hr = dev1->CreateDeviceContextState(stateFlags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &ourState);
            Log("DLSS: device creation flags 0x%X, context state 0x%08X", creation, static_cast<unsigned>(hr));
            if (FAILED(hr)) { LogE("DLSS: context state failed 0x%08X", static_cast<unsigned>(hr)); return false; }
            if (FAILED(device->CreateComputeShader(g_MotionCS, sizeof(g_MotionCS), nullptr, &motionCS)) ||
                FAILED(device->CreateComputeShader(g_ErrorCS, sizeof(g_ErrorCS), nullptr, &errorCS)) ||
                FAILED(device->CreateComputeShader(g_FlipCS, sizeof(g_FlipCS), nullptr, &flipCS)) ||
                FAILED(device->CreateComputeShader(g_HudlessCS, sizeof(g_HudlessCS), nullptr, &hudlessCS)) ||
                FAILED(device->CreateComputeShader(g_SharpenCS, sizeof(g_SharpenCS), nullptr, &sharpenCS)))
            {
                LogE("DLSS: compute shaders failed");
                return false;
            }
            D3D11_BUFFER_DESC cbd = {};
            cbd.ByteWidth = sizeof(FrameConstants);
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&cbd, nullptr, &constants))) return false;
            cbd.ByteWidth = sizeof(BodyConstants);
            if (FAILED(device->CreateBuffer(&cbd, nullptr, &bodyConstants))) return false;
            D3D11_SAMPLER_DESC sd = {};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sd, &linearClamp))) return false;

            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = 32;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            if (FAILED(device->CreateBuffer(&bd, nullptr, &sums))) return false;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = 8;
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            if (FAILED(device->CreateUnorderedAccessView(sums.Get(), &ud, &sumsUav))) return false;
            D3D11_BUFFER_DESC st = {};
            st.ByteWidth = 32;
            st.Usage = D3D11_USAGE_STAGING;
            st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& s : staging) if (FAILED(device->CreateBuffer(&st, nullptr, &s))) return false;
            setupFailed = false;
            Log("DLSS: D3D11 setup done");
            return true;
        }

        bool EnsureFeature(ID3D11DeviceContext* ctx, int w, int h, int ow, int oh, int quality)
        {
            if (feature && featureW == w && featureH == h && featureOW == ow && featureOH == oh && featureQuality == quality) return true;
            if (feature)
            {
                NVSDK_NGX_D3D11_ReleaseFeature(feature);
                feature = nullptr;
            }
            if (!params && NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&params)))
            {
                LogE("DLSS: NGX parameters failed");
                return false;
            }
            NVSDK_NGX_DLSS_Create_Params cp = {};
            cp.Feature.InWidth = static_cast<unsigned>(w);
            cp.Feature.InHeight = static_cast<unsigned>(h);
            cp.Feature.InTargetWidth = static_cast<unsigned>(ow);
            cp.Feature.InTargetHeight = static_cast<unsigned>(oh);
            cp.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(quality);
            cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_CreationNodeMask, 1);
            NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
            NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &feature, params, &cp);
            Set(kKeyDlssCreateResult, static_cast<int>(r));
            if (NVSDK_NGX_FAILED(r))
            {
                feature = nullptr;
                LogE("DLSS: create feature %dx%d -> %dx%d (quality %d) failed: 0x%08X", w, h, ow, oh, quality, static_cast<unsigned>(r));
                return false;
            }
            featureW = w;
            featureH = h;
            featureOW = ow;
            featureOH = oh;
            featureQuality = quality;
            if (w == ow && h == oh) LogC("DLSS: DLAA feature created %dx%d", w, h);
            else LogC("DLSS: Super Resolution feature created %dx%d -> %dx%d (quality %d)", w, h, ow, oh, quality);
            return true;
        }

        void ReadTestResults(ID3D11DeviceContext* ctx)
        {
            for (int i = 0; i < 3; i++)
            {
                if (!stagingBusy[i]) continue;
                D3D11_MAPPED_SUBRESOURCE m;
                if (ctx->Map(staging[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
                const uint32_t* v = static_cast<const uint32_t*>(m.pData);
                for (int k = 0; k < 8; k++) testTotals[k] += v[k];
                testTotals[8]++;
                ctx->Unmap(staging[i].Get(), 0);
                stagingBusy[i] = false;
            }
            if (testTotals[8] >= 60)
            {
                double px = testTotals[2] ? static_cast<double>(testTotals[2]) : 1.0;
                double warped = testTotals[0] / px, zero = testTotals[1] / px;
                double all = static_cast<double>(testTotals[2] + testTotals[5]);
                LogC("DLSS mvtest: %llu frames, error with motion %.3f, without %.3f (ratio %.2f), mean |mv| x %.2f y %.2f px, outside %.1f%%, depth 0 %.1f%% 1 %.1f%%",
                     testTotals[8], warped, zero, zero > 0 ? warped / zero : 0.0, testTotals[3] / px / 4.0,
                     testTotals[4] / px / 4.0, all > 0 ? 100.0 * testTotals[5] / all : 0.0,
                     all > 0 ? 100.0 * testTotals[6] / all : 0.0, all > 0 ? 100.0 * testTotals[7] / all : 0.0);
                for (auto& t : testTotals) t = 0;
            }
        }

        void RunTest(ID3D11DeviceContext* ctx, const FrameParams& p)
        {
            if (prevValid && !p.reset && !stagingBusy[stagingNext])
            {
                const UINT zero[4] = {0, 0, 0, 0};
                ctx->ClearUnorderedAccessViewUint(sumsUav.Get(), zero);
                ID3D11ShaderResourceView* srvs[4] = {depth.srv.Get(), color.srv.Get(), prevColor.srv.Get(), motion.srv.Get()};
                ctx->CSSetShaderResources(0, 4, srvs);
                ID3D11UnorderedAccessView* uavs[2] = {nullptr, sumsUav.Get()};
                ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
                ID3D11SamplerState* s = linearClamp.Get();
                ctx->CSSetSamplers(0, 1, &s);
                ctx->CSSetShader(errorCS.Get(), nullptr, 0);
                ctx->Dispatch((p.width + 7) / 8, (p.height + 7) / 8, 1);
                ID3D11ShaderResourceView* nullSrv[4] = {};
                ID3D11UnorderedAccessView* nullUav[2] = {};
                ctx->CSSetShaderResources(0, 4, nullSrv);
                ctx->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
                ctx->CopyResource(staging[stagingNext].Get(), sums.Get());
                stagingBusy[stagingNext] = true;
                stagingNext = (stagingNext + 1) % 3;
            }
            ReadTestResults(ctx);
            ctx->CopyResource(prevColor.tex.Get(), color.tex.Get());
            prevValid = true;
        }

        // The frame constants' size and flip only (for the passes after the motion vectors).
        void SetSize(ID3D11DeviceContext* ctx, int w, int h, bool flip)
        {
            FrameConstants fc = {};
            fc.size[0] = static_cast<float>(w);
            fc.size[1] = static_cast<float>(h);
            fc.options[0] = flip ? 1.0f : 0.0f;
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                memcpy(m.pData, &fc, sizeof(fc));
                ctx->Unmap(constants.Get(), 0);
            }
        }

        // RCAS (motion.hlsl SharpenCS) over src into sharp: sharpness 0..1 as FSR's (strength exp2(2 s - 2)).
        void Sharpen(ID3D11DeviceContext* ctx, Tex& src, int w, int h, float sharpness)
        {
            FrameConstants fc = {};
            fc.size[0] = static_cast<float>(w);
            fc.size[1] = static_cast<float>(h);
            fc.options[1] = exp2f(2.0f * sharpness - 2.0f);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                memcpy(m.pData, &fc, sizeof(fc));
                ctx->Unmap(constants.Get(), 0);
            }
            ID3D11Buffer* cb = constants.Get();
            ctx->CSSetConstantBuffers(0, 1, &cb);
            ID3D11ShaderResourceView* srv = src.srv.Get();
            ctx->CSSetShaderResources(6, 1, &srv);
            ID3D11UnorderedAccessView* uav = sharp.uav.Get();
            ctx->CSSetUnorderedAccessViews(5, 1, &uav, nullptr);
            ctx->CSSetShader(sharpenCS.Get(), nullptr, 0);
            ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ID3D11UnorderedAccessView* nullUav = nullptr;
            ctx->CSSetShaderResources(6, 1, &nullSrv);
            ctx->CSSetUnorderedAccessViews(5, 1, &nullUav, nullptr);
        }

        void Motion(ID3D11DeviceContext* ctx, const FrameParams& p)
        {
            FrameConstants fc;
            memcpy(fc.invViewProjJittered, p.invViewProjJittered, sizeof(fc.invViewProjJittered));
            memcpy(fc.viewProj, p.viewProj, sizeof(fc.viewProj));
            memcpy(fc.prevViewProj, p.prevViewProj, sizeof(fc.prevViewProj));
            fc.size[0] = static_cast<float>(p.width);
            fc.size[1] = static_cast<float>(p.height);
            fc.mvSign[0] = p.mvSignX;
            fc.mvSign[1] = p.mvSignY;
            fc.options[0] = (p.flags & kFrameFlip) ? 1.0f : 0.0f;
            fc.options[1] = fc.options[2] = fc.options[3] = 0.0f;
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                memcpy(m.pData, &fc, sizeof(fc));
                ctx->Unmap(constants.Get(), 0);
            }
            static BodyConstants bc;
            bc.count = static_cast<uint32_t>(p.bodyCount);
            memcpy(bc.bodies, p.bodies, sizeof(BodyData) * static_cast<size_t>(p.bodyCount));
            if (SUCCEEDED(ctx->Map(bodyConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                memcpy(m.pData, &bc, sizeof(bc));
                ctx->Unmap(bodyConstants.Get(), 0);
            }
            ID3D11Buffer* cbs[2] = {constants.Get(), bodyConstants.Get()};
            ctx->CSSetConstantBuffers(0, 2, cbs);
            ID3D11ShaderResourceView* srv = depth.srv.Get();
            ctx->CSSetShaderResources(0, 1, &srv);
            ID3D11UnorderedAccessView* uav = motion.uav.Get();
            ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            ctx->CSSetShader(motionCS.Get(), nullptr, 0);
            ctx->Dispatch((p.width + 7) / 8, (p.height + 7) / 8, 1);
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ID3D11UnorderedAccessView* nullUav = nullptr;
            ctx->CSSetShaderResources(0, 1, &nullSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        }
    }

    void SetFrame(int slot, const FrameParams& p)
    {
        if (slot < 0 || slot >= kSlots) return;
        std::lock_guard<std::mutex> guard(slotLock);
        slots[slot] = p;
    }

    void EvaluateSlot(int slot)
    {
        if (slot < 0 || slot >= kSlots) return;
        static FrameParams p; // big (bodies); render thread only
        {
            std::lock_guard<std::mutex> guard(slotLock);
            p = slots[slot];
        }
        Globals& g = G();
        std::lock_guard<std::mutex> guard(g.lock);
        ID3D11Device* device = g.device11;
        // Without DLSS (other video cards) the motion vectors still serve FSR and frame generation.
        if (!device || !p.color || !p.depth || !p.output || p.width <= 0 || p.height <= 0) return;
        if (!Setup(device)) return;

        D3D11_TEXTURE2D_DESC cd, od, dd;
        if (!TexDesc(p.color, cd) || !TexDesc(p.output, od) || !TexDesc(p.depth, dd)) return;
        DXGI_FORMAT cf = Typed(cd.Format);
        int ow = p.outWidth > 0 ? p.outWidth : p.width, oh = p.outHeight > 0 ? p.outHeight : p.height;
        if (!SameFamily(cd.Format, od.Format) || !SameFamily(dd.Format, DXGI_FORMAT_R32_FLOAT) ||
            static_cast<int>(cd.Width) != p.width || static_cast<int>(cd.Height) != p.height || dd.Width != cd.Width ||
            dd.Height != cd.Height || static_cast<int>(od.Width) != ow || static_cast<int>(od.Height) != oh)
        {
            if (!formatWarned)
            {
                formatWarned = true;
                LogE("DLSS: unexpected inputs: colour %s %ux%u, depth %s %ux%u, output %s %ux%u (want %dx%d)", FormatName(cd.Format).c_str(),
                     cd.Width, cd.Height, FormatName(dd.Format).c_str(), dd.Width, dd.Height, FormatName(od.Format).c_str(), od.Width,
                     od.Height, ow, oh);
            }
            return;
        }
        bool upscale = ow != p.width || oh != p.height;

        ComPtr<ID3DDeviceContextState> unityState;
        ctx1->SwapDeviceContextState(ourState.Get(), &unityState);
        ID3D11DeviceContext* ctx = ctx1.Get();

        if (EnsureTextures(p.width, p.height, ow, oh, cf))
        {
            ctx->CopyResource(color.tex.Get(), p.color);
            ctx->CopyResource(depth.tex.Get(), p.depth);
            Motion(ctx, p);
            if (p.test) RunTest(ctx, p);
            else prevValid = false;
            // The camera's finished picture: the upscaler's output, or the input as it is.
            Tex* finished = &color;
            bool fsr = (p.flags & kFrameFsr) != 0;
            if ((p.flags & kFrameUpscale) && fsr)
            {
                // AMD FSR through our D3D12 device (fsr.cpp); the result lands in output.
                if (FsrUpscale(ctx, p, color.tex.Get(), depth.tex.Get(), motion.tex.Get(), output.tex.Get(), cf)) finished = &output;
            }
            else if ((p.flags & kFrameUpscale) && Ngx11Ready() && EnsureFeature(ctx, p.width, p.height, ow, oh, p.quality))
            {
                NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
                ep.Feature.pInColor = color.tex.Get();
                ep.Feature.pInOutput = output.tex.Get();
                ep.pInDepth = depth.tex.Get();
                ep.pInMotionVectors = motion.tex.Get();
                ep.InJitterOffsetX = p.jitterX;
                ep.InJitterOffsetY = p.jitterY;
                ep.InRenderSubrectDimensions.Width = static_cast<unsigned>(p.width);
                ep.InRenderSubrectDimensions.Height = static_cast<unsigned>(p.height);
                ep.InReset = p.reset;
                ep.InMVScaleX = 1.0f;
                ep.InMVScaleY = 1.0f;
                ep.InFrameTimeDeltaInMsec = p.frameMs;
                NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, feature, params, &ep);
                Set(kKeyDlssEvalResult, static_cast<int>(r));
                if (NVSDK_NGX_SUCCEED(r))
                {
                    g.values[kKeyDlssEvals].fetch_add(1);
                    finished = &output;
                }
                else if (g.values[kKeyDlssFails].fetch_add(1) < 5) LogE("DLSS: evaluate failed: 0x%08X", static_cast<unsigned>(r));
            }
            // The sharpening slider: FSR sharpens inside (its RCAS), DLSS gets the same RCAS here.
            if (!fsr && finished == &output && p.sharpness > 0.0f)
            {
                Sharpen(ctx, output, ow, oh, p.sharpness);
                finished = &sharp;
            }
            // Upscaling that failed has nothing of the screen's size: the output keeps its last picture.
            bool haveFinished = !upscale || finished == &output || finished == &sharp;
            if (haveFinished) ctx->CopyResource(p.output, finished->tex.Get());

            // Frame generation inputs in the back buffer's orientation: depth and motion (render size), and the
            // HUD-less picture (screen size).
            if ((p.flags & kFrameFg) && haveFinished)
            {
                ID3D11ShaderResourceView* srvs[6] = {depth.srv.Get(), nullptr, nullptr, motion.srv.Get(), nullptr, finished->srv.Get()};
                ctx->CSSetShaderResources(0, 6, srvs);
                ID3D11UnorderedAccessView* uavs[5] = {nullptr, nullptr, fgDepth.uav.Get(), fgMotion.uav.Get(), fgHudless.uav.Get()};
                ctx->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
                // Render size and flip again (the sharpening pass set the constants to the output size).
                SetSize(ctx, p.width, p.height, (p.flags & kFrameFlip) != 0);
                ID3D11Buffer* cb = constants.Get();
                ctx->CSSetConstantBuffers(0, 1, &cb);
                ctx->CSSetShader(flipCS.Get(), nullptr, 0);
                ctx->Dispatch((p.width + 7) / 8, (p.height + 7) / 8, 1);
                if (upscale) SetSize(ctx, ow, oh, (p.flags & kFrameFlip) != 0);
                ctx->CSSetShader(hudlessCS.Get(), nullptr, 0);
                ctx->Dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
                ID3D11ShaderResourceView* nullSrv[6] = {};
                ID3D11UnorderedAccessView* nullUav[5] = {};
                ctx->CSSetShaderResources(0, 6, nullSrv);
                ctx->CSSetUnorderedAccessViews(0, 5, nullUav, nullptr);
                fgCamera = p.fg;
                fgFresh = true;
            }
        }
        ctx1->SwapDeviceContextState(unityState.Get(), nullptr);
    }

    bool TakeFgInputs(ID3D11Texture2D** outDepth, ID3D11Texture2D** outMotion, ID3D11Texture2D** outHudless, FgCamera* camera)
    {
        std::lock_guard<std::mutex> guard(G().lock);
        if (!fgFresh || !fgDepth.tex || !fgMotion.tex || !fgHudless.tex) return false;
        fgFresh = false;
        fgDepth.tex.CopyTo(outDepth);
        fgMotion.tex.CopyTo(outMotion);
        fgHudless.tex.CopyTo(outHudless);
        *camera = fgCamera;
        return true;
    }

    void ShutdownDlss()
    {
        if (feature) NVSDK_NGX_D3D11_ReleaseFeature(feature);
        feature = nullptr;
        if (params) NVSDK_NGX_D3D11_DestroyParameters(params);
        params = nullptr;
        featureW = featureH = featureOW = featureOH = 0;
        featureQuality = -1;
        for (Tex* t : {&color, &depth, &motion, &output, &prevColor, &sharp, &fgDepth, &fgMotion, &fgHudless}) t->Reset();
        fgFresh = false;
        texW = texH = texOW = texOH = 0;
        ourState.Reset();
        motionCS.Reset();
        errorCS.Reset();
        flipCS.Reset();
        hudlessCS.Reset();
        sharpenCS.Reset();
        constants.Reset();
        bodyConstants.Reset();
        linearClamp.Reset();
        sums.Reset();
        sumsUav.Reset();
        for (auto& s : staging) s.Reset();
        ctx1.Reset();
        dev1.Reset();
        setupFailed = false;
    }
}
