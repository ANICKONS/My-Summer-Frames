// My Summer Frames core: NVIDIA Reflex (low latency mode) for the game's D3D11 device, through NVAPI.
// Copyright (C) 2026 ANICKON
//
// Reflex wants NvAPI_D3D_Sleep at the very start of each frame, before input is sampled, and latency markers around
// the frame's simulation and render submission. Unity 5.0 gives scripts no callback that early: a frame starts with
// the game's message pump (raw mouse input arrives there as WM_INPUT), then the player loop. The pump is found by
// hooking PeekMessageA/W in the game's import table: the first call on the main thread after the previous frame's
// end (RENDERSUBMIT_END, sent by the mod at WaitForEndOfFrame) is the start of the next frame. All markers are sent
// from the main thread (Unity's render thread submits the D3D calls later; Reflex paces the simulation thread).
#include "common.h"

#include "nvapi.h"

namespace msf
{
    namespace
    {
        using PeekFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
        PeekFn realPeekW = nullptr, realPeekA = nullptr;
        bool hooked = false;
        DWORD mainThread = 0;

        bool nvapiOk = false;
        std::atomic<int> mode{0};            // 0 off, 1 on, 2 on + boost
        std::atomic<bool> frameEnded{false}; // set at RENDERSUBMIT_END, cleared at the next frame's start
        std::atomic<uint64_t> frameId{0};
        std::atomic<int> sleeps{0}, starts{0};
        bool setModeLogged = false;

        IUnknown* Device() { return G().device11; }

        void Marker(NV_LATENCY_MARKER_TYPE type, uint64_t id)
        {
            IUnknown* d = Device();
            if (!nvapiOk || !d) return;
            NV_LATENCY_MARKER_PARAMS p = {};
            p.version = NV_LATENCY_MARKER_PARAMS_VER;
            p.frameID = id;
            p.markerType = type;
            NvAPI_D3D_SetLatencyMarker(d, &p);
        }

        void FrameStart()
        {
            IUnknown* d = Device();
            if (!nvapiOk || !d) return;
            if (mode.load() > 0)
            {
                NvAPI_Status s = NvAPI_D3D_Sleep(d);
                if (s == NVAPI_OK) sleeps.fetch_add(1);
            }
            uint64_t id = frameId.fetch_add(1) + 1;
            Marker(SIMULATION_START, id);
            Marker(INPUT_SAMPLE, id);
            starts.fetch_add(1);
        }

        BOOL WINAPI HookPeekW(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove)
        {
            if (GetCurrentThreadId() == mainThread && frameEnded.exchange(false)) FrameStart();
            return realPeekW(msg, hwnd, min, max, remove);
        }

        BOOL WINAPI HookPeekA(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove)
        {
            if (GetCurrentThreadId() == mainThread && frameEnded.exchange(false)) FrameStart();
            return realPeekA(msg, hwnd, min, max, remove);
        }

        // The game's import table entry of user32!name -> ours. Returns the original.
        void* PatchImport(HMODULE module, const char* dll, const char* name, void* hook)
        {
            auto* base = reinterpret_cast<BYTE*>(module);
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress) return nullptr;
            for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; imp++)
            {
                if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
                auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
                auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
                for (; names->u1.AddressOfData; names++, slots++)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                    auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                    if (strcmp(reinterpret_cast<const char*>(byName->Name), name) != 0) continue;
                    void* original = reinterpret_cast<void*>(slots->u1.Function);
                    DWORD old;
                    if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
                    slots->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
                    VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
                    return original;
                }
            }
            return nullptr;
        }
    }

    // Main thread (MSF_Configure): the frame start hook.
    void InstallFrameHook()
    {
        if (hooked) return;
        hooked = true;
        mainThread = GetCurrentThreadId();
        HMODULE game = GetModuleHandleW(nullptr);
        realPeekW = reinterpret_cast<PeekFn>(PatchImport(game, "user32.dll", "PeekMessageW", reinterpret_cast<void*>(&HookPeekW)));
        realPeekA = reinterpret_cast<PeekFn>(PatchImport(game, "user32.dll", "PeekMessageA", reinterpret_cast<void*>(&HookPeekA)));
        Log("Reflex: frame start hook PeekMessageW %s, PeekMessageA %s, main thread %lu", realPeekW ? "on" : "not found",
            realPeekA ? "on" : "not found", mainThread);
    }

    // Render thread, after the device is known (InitNgx).
    void InitReflex()
    {
        NvAPI_Status s = NvAPI_Initialize();
        if (s != NVAPI_OK)
        {
            Set(kKeyReflexAvailable, -1);
            Log("Reflex: NvAPI_Initialize %d", static_cast<int>(s));
            return;
        }
        NV_GET_SLEEP_STATUS_PARAMS st = {};
        st.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
        s = NvAPI_D3D_GetSleepStatus(Device(), &st);
        nvapiOk = s == NVAPI_OK;
        Set(kKeyReflexAvailable, nvapiOk ? 1 : -2);
        Log("Reflex: sleep status %d (low latency %d, fullscreen VRR %d, CPL vsync %d)", static_cast<int>(s), st.bLowLatencyMode,
            st.bFsVrr, st.bCplVsyncOn);
    }

    void SetReflexMode(int m)
    {
        if (m < 0) m = 0;
        if (m > 2) m = 2;
        mode = m;
        IUnknown* d = Device();
        if (!nvapiOk || !d) return;
        NV_SET_SLEEP_MODE_PARAMS p = {};
        p.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        p.bLowLatencyMode = m > 0;
        p.bLowLatencyBoost = m == 2;
        p.minimumIntervalUs = 0;
        p.bUseMarkersToOptimize = false;
        NvAPI_Status s = NvAPI_D3D_SetSleepMode(d, &p);
        Set(kKeyReflexMode, s == NVAPI_OK ? m : -1);
        if (s == NVAPI_OK) LogC("DLSS: Reflex %s", m == 0 ? "off" : m == 1 ? "on" : "on + boost");
        else LogE("DLSS: Reflex %s refused by NVAPI (%d)", m == 0 ? "off" : m == 1 ? "on" : "on + boost", static_cast<int>(s));
        setModeLogged = true;
    }

    // Main thread: the mod's markers (the frame start ones come from the hook).
    void ReflexMarker(int type)
    {
        uint64_t id = frameId.load();
        if (type == RENDERSUBMIT_END)
        {
            Marker(RENDERSUBMIT_END, id);
            frameEnded = true;
        }
        else if (type == SIMULATION_END)
        {
            Marker(SIMULATION_END, id);
            Marker(RENDERSUBMIT_START, id);
        }
    }

    // Main thread: averages over the last 64 frames the driver reports. out: frames, PC latency (simulation start
    // -> GPU done) ms, GPU busy ms, GPU frame ms, simulation ms, submit ms, frame interval ms, sleeps/s-ish counter.
    int ReflexReport(float* out, int n)
    {
        IUnknown* d = Device();
        if (!nvapiOk || !d || !out || n < 8) return 0;
        static NV_LATENCY_RESULT_PARAMS r;
        memset(&r, 0, sizeof(r));
        r.version = NV_LATENCY_RESULT_PARAMS_VER;
        NvAPI_Status s = NvAPI_D3D_GetLatency(d, &r);
        if (s != NVAPI_OK) return 0;
        double lat = 0, busy = 0, gframe = 0, sim = 0, submit = 0, interval = 0;
        int count = 0, intervals = 0;
        uint64_t prevStart = 0;
        for (const auto& f : r.frameReport)
        {
            if (!f.simStartTime || !f.gpuRenderEndTime || f.gpuRenderEndTime < f.simStartTime) { prevStart = 0; continue; }
            lat += static_cast<double>(f.gpuRenderEndTime - f.simStartTime);
            busy += f.gpuActiveRenderTimeUs;
            gframe += f.gpuFrameTimeUs;
            if (f.simEndTime > f.simStartTime) sim += static_cast<double>(f.simEndTime - f.simStartTime);
            if (f.renderSubmitEndTime > f.renderSubmitStartTime) submit += static_cast<double>(f.renderSubmitEndTime - f.renderSubmitStartTime);
            if (prevStart && f.simStartTime > prevStart) { interval += static_cast<double>(f.simStartTime - prevStart); intervals++; }
            prevStart = f.simStartTime;
            count++;
        }
        if (!count) return 0;
        out[0] = static_cast<float>(count);
        out[1] = static_cast<float>(lat / count / 1000.0);
        out[2] = static_cast<float>(busy / count / 1000.0);
        out[3] = static_cast<float>(gframe / count / 1000.0);
        out[4] = static_cast<float>(sim / count / 1000.0);
        out[5] = static_cast<float>(submit / count / 1000.0);
        out[6] = intervals ? static_cast<float>(interval / intervals / 1000.0) : 0.0f;
        out[7] = static_cast<float>(sleeps.load());
        Log("Reflex report (mode %d): %d frames, latency sim start -> GPU done %.2f ms, GPU busy %.2f ms of %.2f ms GPU frame, "
            "simulation %.2f ms, submit %.2f ms, frame interval %.2f ms, sleeps %d, frame starts %d",
            mode.load(), count, out[1], out[2], out[3], out[4], out[5], out[6], sleeps.load(), starts.load());
        return count;
    }
}
