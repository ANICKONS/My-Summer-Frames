// My Summer Frames core: Unity 5.0 entry points and the functions the mod calls.
// Copyright (C) 2026 ANICKON
//
// Unity 5.0 has no IUnityInterfaces (5.2+): it calls UnitySetGraphicsDevice when it loads a plugin from
// <Data>\Plugins, and GL.IssuePluginEvent(id) calls UnityRenderEvent(id) of every such plugin on the render thread
// (the game runs with a separate render thread). Every D3D11 call of ours happens there.
#include "common.h"

namespace msf
{
    Globals& G()
    {
        static Globals g;
        return g;
    }

    void Set(Key key, int value)
    {
        if (key >= 0 && key < kKeyCount) G().values[key].store(value);
    }

    int Get(Key key)
    {
        return key >= 0 && key < kKeyCount ? G().values[key].load() : 0;
    }

    namespace
    {
        std::mutex textLock;
        std::string texts[kTextCount];
    }

    void SetText(Text which, const std::string& text)
    {
        if (which < 0 || which >= kTextCount) return;
        std::lock_guard<std::mutex> guard(textLock);
        texts[which] = text;
    }

    std::string GetText(Text which)
    {
        if (which < 0 || which >= kTextCount) return "";
        std::lock_guard<std::mutex> guard(textLock);
        return texts[which];
    }

    namespace
    {
        struct Defaults
        {
            Defaults()
            {
                for (int i = 0; i < kKeyCount; i++) G().values[i].store(0);
                Set(kKeyApiVersion, kApiVersion);
                Set(kKeyDeviceType, -1);
            }
        } defaults;

        void SetDevice(ID3D11Device* device, int source)
        {
            Globals& g = G();
            std::lock_guard<std::mutex> guard(g.lock);
            if (g.device11 == device) return;
            if (g.device11) g.device11->Release();
            g.device11 = device;
            if (device) device->AddRef();
            Set(kKeyDeviceSource, device ? source : 0);
        }
    }
}

using namespace msf;

extern "C" __declspec(dllexport) void __stdcall UnitySetGraphicsDevice(void* device, int deviceType, int eventType)
{
    Set(kKeyDeviceType, deviceType);
    Log("UnitySetGraphicsDevice(%p, type %d, event %d)", device, deviceType, eventType);
    if (deviceType != kUnityGfxRendererD3D11) return;
    if (eventType == kUnityGfxDeviceEventInitialize || eventType == kUnityGfxDeviceEventAfterReset)
        SetDevice(static_cast<ID3D11Device*>(device), 1);
    else if (eventType == kUnityGfxDeviceEventShutdown)
    {
        // The game is closing: Unity unloads d3d11.dll right after this. Every reference of ours to its device
        // must be gone by then, or the device's own threads run on in unloaded code (the exit crashed so).
        StopFgThread();
        ShutdownNgx();
        SetDevice(nullptr, 0);
        UninstallCrashLog();
        Log("D3D11 references released");
    }
}

extern "C" __declspec(dllexport) void __stdcall UnityRenderEvent(int eventId)
{
    if ((eventId & ~0xFF) != kEventBase) return;
    G().values[kKeyRenderEvents].fetch_add(1);
    switch (eventId)
    {
    case kEventInit: InitNgx(); break;
    case kEventProbeTargets: ProbeBoundTargets("OnPostRender"); break;
    case kEventProbeImage: ProbeBoundTargets("OnRenderImage"); break;
    case kEventCaptureFg: CaptureFg(); break;
    default:
        if (eventId >= kEventEvaluate && eventId < kEventEvaluate + kSlots) EvaluateSlot(eventId - kEventEvaluate);
        break;
    }
}

// The frame's DLSS inputs (see FrameParams); data = jitter x/y, three 4x4 matrices, mv sign x/y, frame ms;
// bodies = bodyCount x 40 floats (BodyData).
// data[53..] = FgCamera (kFgCameraFloats floats, then reset comes from the reset argument), then the sharpness
// (0..1); flags = FrameFlags.
MSF_EXPORT void MSF_SetFrame(int slot, void* color, void* depth, void* output, int width, int height, int outWidth, int outHeight,
                                     int quality, const float* data, const float* bodies, int bodyCount, int reset, int test, int flags)
{
    if (!data) return;
    static FrameParams p; // big (bodies); the main thread is the only caller
    p = FrameParams{};
    p.color = static_cast<ID3D11Resource*>(color);
    p.depth = static_cast<ID3D11Resource*>(depth);
    p.output = static_cast<ID3D11Resource*>(output);
    p.width = width;
    p.height = height;
    p.outWidth = outWidth;
    p.outHeight = outHeight;
    p.quality = quality;
    p.jitterX = data[0];
    p.jitterY = data[1];
    memcpy(p.invViewProjJittered, data + 2, sizeof(p.invViewProjJittered));
    memcpy(p.viewProj, data + 18, sizeof(p.viewProj));
    memcpy(p.prevViewProj, data + 34, sizeof(p.prevViewProj));
    p.mvSignX = data[50];
    p.mvSignY = data[51];
    p.frameMs = data[52];
    p.reset = reset;
    p.test = test;
    p.flags = flags;
    const float* fg = data + 53;
    memcpy(p.fg.viewToClip, fg, sizeof(float) * 16);
    memcpy(p.fg.clipToView, fg + 16, sizeof(float) * 16);
    memcpy(p.fg.clipToPrevClip, fg + 32, sizeof(float) * 16);
    memcpy(p.fg.prevClipToClip, fg + 48, sizeof(float) * 16);
    memcpy(p.fg.pos, fg + 64, sizeof(float) * 3);
    memcpy(p.fg.up, fg + 67, sizeof(float) * 3);
    memcpy(p.fg.right, fg + 70, sizeof(float) * 3);
    memcpy(p.fg.fwd, fg + 73, sizeof(float) * 3);
    p.fg.nearZ = fg[76];
    p.fg.farZ = fg[77];
    p.fg.fov = fg[78];
    p.fg.aspect = fg[79];
    p.fg.jitter[0] = fg[80];
    p.fg.jitter[1] = fg[81];
    p.fg.reset = reset;
    p.sharpness = data[53 + kFgCameraFloats]; // after the frame generation camera
    if (!(p.sharpness >= 0.0f)) p.sharpness = 0.0f;
    if (p.sharpness > 1.0f) p.sharpness = 1.0f;
    p.bodyCount = bodies ? (bodyCount < 0 ? 0 : bodyCount > kMaxBodies ? kMaxBodies : bodyCount) : 0;
    if (p.bodyCount) memcpy(p.bodies, bodies, sizeof(BodyData) * static_cast<size_t>(p.bodyCount));
    SetFrame(slot, p);
}

// Frame generation: kind 1 DLSS, 2 FSR (FgKind); generated frames per real frame (0 = off, 1 = 2x ... 5 = 6x; FSR: 1).
// Main thread.
MSF_EXPORT void MSF_FgSetMode(int kind, int generatedFrames)
{
    SetFgMode(kind, generatedFrames);
}

// Test switch: the FSR runtime's input checks (its messages go to the log).
MSF_EXPORT void MSF_FsrSetDebug(int on)
{
    SetFsrDebug(on != 0);
}

// A text value (Text); UTF-8, not terminated. Returns its length (truncated to length).
MSF_EXPORT int MSF_GetText(int which, char* buffer, int length)
{
    std::string t = GetText(static_cast<Text>(which));
    int n = static_cast<int>(t.size());
    if (!buffer || length <= 0) return n;
    if (n > length) n = length;
    memcpy(buffer, t.data(), static_cast<size_t>(n));
    return n;
}

// Main thread, every frame: creates / sizes / shows the bridge's child window. Returns the stage (kKeyFgStage).
MSF_EXPORT int MSF_FgWindow()
{
    int stage = FgWindow();
    Set(kKeyFgStage, stage);
    return stage;
}

MSF_EXPORT void MSF_FgDump(const wchar_t* folder)
{
    RequestFgDump(folder ? folder : L".");
}

MSF_EXPORT void MSF_FgSetGenerate(int on)
{
    SetFgGenerate(on != 0);
}

MSF_EXPORT void MSF_FgSetHudless(int on)
{
    SetFgHudless(on != 0);
}

// Reflex (main thread): mode 0 off, 1 on, 2 on + boost; markers SIMULATION_END (1) and RENDERSUBMIT_END (3);
// report: see ReflexReport.
MSF_EXPORT void MSF_ReflexSetMode(int mode)
{
    SetReflexMode(mode);
}

MSF_EXPORT void MSF_ReflexMarker(int type)
{
    ReflexMarker(type);
}

MSF_EXPORT int MSF_ReflexReport(float* out, int n)
{
    return ReflexReport(out, n);
}

MSF_EXPORT int MSF_GetInt(int key)
{
    return Get(static_cast<Key>(key));
}

// logDir: log file + NGX data; featureDir: where nvngx_dlss.dll / nvngx_dlssg.dll are.
MSF_EXPORT void MSF_Configure(const wchar_t* logDir, const wchar_t* featureDir)
{
    Globals& g = G();
    {
        std::lock_guard<std::mutex> guard(g.lock);
        g.logDir = logDir ? logDir : L".";
        g.featureDir = featureDir ? featureDir : L"";
    }
    LogOpen(g.logDir + L"\\msf.log");
    InstallCrashLog(g.logDir);
    InstallFrameHook();
    Log("My Summer Frames core %d, log %s, features %s", kApiVersion, Narrow(g.logDir).c_str(), Narrow(g.featureDir).c_str());
}

// When Unity did not call UnitySetGraphicsDevice: the device from any texture (Texture.GetNativeTexturePtr).
MSF_EXPORT int MSF_SetDeviceFromTexture(void* texture)
{
    if (!texture) return 0;
    ComPtr<ID3D11Device> device;
    static_cast<ID3D11Resource*>(texture)->GetDevice(&device);
    if (!device) return 0;
    SetDevice(device.Get(), 2);
    Log("device from a texture: %p", static_cast<void*>(device.Get()));
    return 1;
}

MSF_EXPORT void MSF_DescribeTexture(void* texture, const char* tag)
{
    DescribeResource(tag ? tag : "texture", static_cast<IUnknown*>(texture));
}

MSF_EXPORT int MSF_DrainLog(char* buffer, int length)
{
    return LogDrain(buffer, length);
}

MSF_EXPORT void MSF_Log(const char* text)
{
    Log("mod: %s", text ? text : "");
}
