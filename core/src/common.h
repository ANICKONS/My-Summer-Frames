// My Summer Frames core (msf_core.dll): DLSS, FSR, frame generation and Reflex for My Summer Car (Unity 5.0.0f4, Direct3D 11).
// Copyright (C) 2026 ANICKON
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_defs_dlssg.h"

using Microsoft::WRL::ComPtr;

#define MSF_EXPORT extern "C" __declspec(dllexport)

namespace msf
{
    // Bumped when the C# side must change with the DLL (checked by the mod).
    constexpr int kApiVersion = 7;

    // Unity 5.0 native plugin interface (UnityPluginInterface.h of that version).
    enum UnityGfxRenderer { kUnityGfxRendererD3D11 = 2 };
    enum UnityGfxDeviceEventType
    {
        kUnityGfxDeviceEventInitialize = 0,
        kUnityGfxDeviceEventShutdown = 1,
        kUnityGfxDeviceEventBeforeReset = 2,
        kUnityGfxDeviceEventAfterReset = 3,
    };

    // GL.IssuePluginEvent(id) reaches every plugin that exports UnityRenderEvent: ours carry this base.
    constexpr int kEventBase = 0x4D534400; // "MSD\0"
    constexpr int kSlots = 4;
    enum Event
    {
        kEventInit = kEventBase + 1,         // NGX D3D11 + D3D12 init and capability query
        kEventProbeTargets = kEventBase + 2, // log the bound render targets (camera OnPostRender)
        kEventProbeImage = kEventBase + 3,   // the same, inside OnRenderImage
        kEventCaptureFg = kEventBase + 5,    // WaitForEndOfFrame: the finished back buffer to the bridge
        kEventEvaluate = kEventBase + 0x10,  // + slot: motion vectors + DLSS for the frame in that slot
    };

    // Values read by the mod through MSF_GetInt.
    enum Key
    {
        kKeyApiVersion = 0,
        kKeyDeviceType = 1,     // from UnitySetGraphicsDevice; -1 = never called
        kKeyRenderEvents = 2,   // UnityRenderEvent calls with our ids
        kKeyInitState = 3,      // 0 not started, 1 running, 2 done
        kKeyNgx11Result = 4,
        kKeySrAvailable = 5,
        kKeySrNeedsDriver = 6,
        kKeySrInitResult = 7,
        kKeyD3d12Result = 8,
        kKeyNgx12Result = 9,
        kKeyFgAvailable = 10,
        kKeyFgMultiFrameMax = 11,
        kKeyFgNeedsDriver = 12,
        kKeyFgInitResult = 13,
        kKeySrMinDriverMajor = 14,
        kKeySrMinDriverMinor = 15,
        kKeyFgMinDriverMajor = 16,
        kKeyFgMinDriverMinor = 17,
        kKeyHasD3d11_4 = 18,
        kKeyFeatureLevel = 19,
        kKeyDeviceSource = 20,  // 0 none, 1 UnitySetGraphicsDevice, 2 texture pointer
        kKeyRenderThreadId = 21,
        kKeyDlssCreateResult = 22,
        kKeyDlssEvalResult = 23,
        kKeyDlssEvals = 24,
        kKeyDlssFails = 25,
        kKeyFgStage = 26,      // -1 failed, 0 off, 1 warm-up (both swapchains present), 2 on (ours only)
        kKeyFgReal = 27,       // real frames presented by the bridge
        kKeyFgGenerated = 28,  // generated frames presented
        kKeyFgFails = 29,
        kKeyFgLastResult = 30, // last NGX DLSSG result
        kKeyFgBackbuffer = 31, // DXGI format of the game's back buffer
        kKeyFgIntervalUs = 32, // smoothed interval between real frames
        kKeyReflexAvailable = 33, // 1 NVAPI Reflex usable, <0 not (NVIDIA driver / GPU missing)
        kKeyReflexMode = 34,      // mode set (0 off, 1 on, 2 on + boost), -1 refused
        kKeyFsrLoaded = 35,       // AMD FSR loader: 1 loaded, 0 not tried, -1 DLL missing / not loadable, -2 no D3D12 device
        kKeyFsrUpAvailable = 36,  // 1: an FSR upscaler runs on this device
        kKeyFsrFgAvailable = 37,  // 1: FSR frame generation and its swap chain run on this device
        kKeyFsrCreateResult = 38, // last upscaler context creation (ffxReturnCode_t), -1 none yet
        kKeyFsrEvals = 39,
        kKeyFsrFails = 40,
        kKeyFsrLastResult = 41,   // last upscaler dispatch result
        kKeyFgKind = 42,          // the bridge's active path: 0 none, 1 DLSS-G, 2 FSR
        kKeyFsrFgLastResult = 43, // last FSR frame generation call result
        kKeyCount
    };

    // Texts read by the mod through MSF_GetText.
    enum Text
    {
        kTextFsrUpVersions = 0, // FSR upscaler versions this device can run, newest first ("3.1.6, 2.3.4")
        kTextFsrFgVersions = 1, // FSR frame generation versions
        kTextFsrScVersions = 2, // FSR frame generation swap chain versions
        kTextFsrUpActive = 3,   // the upscaler version in use
        kTextFsrFgActive = 4,   // the frame generation version in use
        kTextFsrScActive = 5,   // the swap chain version in use
        kTextCount
    };

    // Frame generation paths of the bridge.
    enum FgKind
    {
        kFgDlss = 1,
        kFgFsr = 2,
    };

    // Camera data DLSS Frame Generation needs, screen orientation (the back buffer's), no jitter.
    // Matrices row-major for column vectors (out = M * v), as NVSDK_NGX_DLSSG_Opt_Eval_Params wants.
    struct FgCamera
    {
        float viewToClip[16];
        float clipToView[16];
        float clipToPrevClip[16];
        float prevClipToClip[16];
        float pos[3], up[3], right[3], fwd[3];
        float nearZ, farZ, fov, aspect;
        float jitter[2];
        int reset;
    };
    constexpr int kFgCameraFloats = 64 + 12 + 4 + 2;

    enum FrameFlags
    {
        kFrameUpscale = 1, // run the upscaler (else the output is the input)
        kFrameFg = 2,      // keep depth + motion (screen orientation) and the camera for frame generation
        kFrameFlip = 4,    // the render textures are upside down against the back buffer (Direct3D + Unity)
        kFrameFsr = 8,     // the upscaler is AMD FSR (else NVIDIA DLSS)
    };

    // A moving rigid body for the motion vectors (motion.hlsl Body): 40 floats.
    constexpr int kMaxBodies = 16;
    struct BodyData
    {
        float toLocal[16];
        float prevToWorld[16];
        float center[4];  // w = box volume
        float extents[4];
    };

    // One frame's DLSS inputs, written by the main thread (MSF_SetFrame), read on the render thread.
    // Matrices are column-major (Unity's Matrix4x4 memory order), camera-relative (world minus camera position).
    struct FrameParams
    {
        ID3D11Resource* color;
        ID3D11Resource* depth; // R32: Unity's _CameraDepthTexture copied by the mod
        ID3D11Resource* output;
        int width, height;       // render size: colour, depth, motion
        int outWidth, outHeight; // DLSS output (the screen); = render size for DLAA and frame generation alone
        int quality;             // DLSS: NVSDK_NGX_PerfQuality_Value; FSR: FfxApiUpscaleQualityMode (logs only)
        float jitterX, jitterY;
        float invViewProjJittered[16];
        float viewProj[16];
        float prevViewProj[16];
        float mvSignX, mvSignY;
        float frameMs;
        float sharpness;         // 0 off .. 1 strongest (FSR: its own RCAS; DLSS: the same RCAS here, after DLSS)
        int reset;
        int test;
        int flags;
        FgCamera fg;
        int bodyCount;
        BodyData bodies[kMaxBodies];
    };

    struct Globals
    {
        std::mutex lock; // every NGX call and every change of the fields below
        ID3D11Device* device11 = nullptr;
        std::wstring logDir;
        std::wstring featureDir;
        std::atomic<int> values[kKeyCount];
    };

    Globals& G();
    void Set(Key key, int value);
    int Get(Key key);
    void SetText(Text which, const std::string& text);
    std::string GetText(Text which);

    // A texture of our D3D12 device opened on Unity's D3D11 device (same adapter). Created in D3D12_RESOURCE_STATE_COMMON.
    struct SharedTex
    {
        ComPtr<ID3D12Resource> r12;
        ComPtr<ID3D11Texture2D> r11;
    };
    bool MakeSharedTexture(ID3D12Device* d12, ID3D11Device1* d11, UINT w, UINT h, DXGI_FORMAT f, SharedTex& t, const char* tag);

    // log.cpp
    void LogOpen(const std::wstring& path);
    void Log(const char* fmt, ...);        // file only
    void LogC(const char* fmt, ...);       // file, and a detail for the console (shown with the mod's detailed log)
    void LogE(const char* fmt, ...);       // file, and a problem for the console (always shown)
    int LogDrain(char* buffer, int length);

    // ngx_init.cpp
    void InitNgx();
    void ShutdownNgx();
    bool Ngx11Ready();
    bool Ngx12Ready();
    ID3D12Device* Device12();

    // dlss11.cpp (EvaluateSlot and ShutdownDlss: caller holds no lock / G().lock respectively)
    void SetFrame(int slot, const FrameParams& p);
    void EvaluateSlot(int slot);
    void ShutdownDlss();
    // The last evaluated frame's depth + motion (screen orientation, R32 / RG16) and camera, once per frame (render
    // thread). False when this frame produced none.
    bool TakeFgInputs(ID3D11Texture2D** depth, ID3D11Texture2D** motion, ID3D11Texture2D** hudless, FgCamera* camera);

    // fsr.cpp: AMD FSR (signed DLLs of the FSR SDK, loaded from the mod's assets) on our D3D12 device.
    void InitFsr(ID3D12Device* device12); // render thread, from InitNgx
    bool FsrUpscaleReady();
    bool FsrFgReady();
    // Render thread, inside EvaluateSlot (our D3D11 context state, G().lock held): colour / depth / motion (render
    // size) -> FSR -> output (screen size), through our D3D12 device. False: output untouched.
    bool FsrUpscale(ID3D11DeviceContext* ctx, const FrameParams& p, ID3D11Texture2D* color, ID3D11Texture2D* depth,
                    ID3D11Texture2D* motion, ID3D11Texture2D* output, DXGI_FORMAT format);
    void ShutdownFsr();
    void SetFsrDebug(bool on); // test switch: the FSR runtime's input checks (messages in the log); contexts made again
    bool FsrDebug();
    // The loaded API (valid while FsrLoaded()): fg12.cpp uses it for FSR frame generation.
    struct FfxFns;
    const FfxFns& Ffx();
    bool FsrLoaded();

    // fg12.cpp: the Direct3D 12 bridge for DLSS / FSR Frame Generation.
    void SetFgMode(int kind, int generatedFrames); // main thread; kind = FgKind; 0 frames = off
    int FgWindow();                       // main thread, every frame: the bridge's child window
    void RequestFgDump(const std::wstring& folder);
    void SetFgGenerate(bool on);          // test switch: the bridge without DLSS-G
    void SetFgHudless(bool on);           // test switch: DLSS-G without the HUD-less picture
    void CaptureFg();                     // render thread, end of frame: the back buffer to the bridge
    void ShutdownFg();
    void StopFgThread();                  // game exit: only the presenter thread stops

    // reflex.cpp
    void InstallFrameHook();             // main thread
    void InitReflex();                   // render thread, device known
    void SetReflexMode(int mode);        // main thread: 0 off, 1 on, 2 on + boost
    void ReflexMarker(int type);         // main thread: NV_LATENCY_MARKER_TYPE (SIMULATION_END, RENDERSUBMIT_END)
    int ReflexReport(float* out, int n); // main thread

    // crash.cpp
    void InstallCrashLog(const std::wstring& folder);
    void UninstallCrashLog();

    // probe.cpp
    void ProbeBoundTargets(const char* tag);
    void DescribeResource(const char* tag, IUnknown* resource);

    std::string Narrow(const std::wstring& text);
    std::string FormatName(DXGI_FORMAT format);
}
