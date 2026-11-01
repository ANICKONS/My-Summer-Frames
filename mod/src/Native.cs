// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// The native plugin (msf_core.dll in mysummercar_Data\Plugins: only plugins Unity loads from there get
// UnitySetGraphicsDevice and the GL.IssuePluginEvent calls on the render thread).

using System;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;

namespace MySummerFrames
{
    static class Native
    {
        public const string Dll = "msf_core";
        public const string DllFile = Dll + ".dll";
        /// <summary>Must match kApiVersion of the native plugin.</summary>
        public const int ApiVersion = 7;

        // FrameFlags of the native plugin: run the upscaler, keep frame generation inputs, flip, the upscaler is FSR.
        public const int FrameUpscale = 1, FrameFg = 2, FrameFlip = 4, FrameFsr = 8;
        // FgKind of the native plugin.
        public const int FgKindDlss = 1, FgKindFsr = 2;
        /// <summary>53 floats of DLSS data + 82 of frame generation camera data + the sharpness (0..1).</summary>
        public const int FrameFloats = 53 + 82 + 1;

        // Must match Event in core/src/common.h.
        const int EventBase = 0x4D534400;
        public const int EventInit = EventBase + 1;
        public const int EventProbeTargets = EventBase + 2;
        public const int EventProbeImage = EventBase + 3;
        public const int EventCaptureFg = EventBase + 5;
        public const int EventEvaluate = EventBase + 0x10; // + slot
        public const int Slots = 4;

        // Must match Key in core/src/common.h.
        public enum Key
        {
            ApiVersion = 0, DeviceType = 1, RenderEvents = 2, InitState = 3, Ngx11Result = 4, SrAvailable = 5,
            SrNeedsDriver = 6, SrInitResult = 7, D3d12Result = 8, Ngx12Result = 9, FgAvailable = 10, FgMultiFrameMax = 11,
            FgNeedsDriver = 12, FgInitResult = 13, SrMinDriverMajor = 14, SrMinDriverMinor = 15, FgMinDriverMajor = 16,
            FgMinDriverMinor = 17, HasD3d11_4 = 18, FeatureLevel = 19, DeviceSource = 20, RenderThreadId = 21,
            DlssCreateResult = 22, DlssEvalResult = 23, DlssEvals = 24, DlssFails = 25,
            FgStage = 26, FgReal = 27, FgGenerated = 28, FgFails = 29, FgLastResult = 30, FgBackbuffer = 31, FgIntervalUs = 32,
            ReflexAvailable = 33, ReflexMode = 34,
            FsrLoaded = 35, FsrUpAvailable = 36, FsrFgAvailable = 37, FsrCreateResult = 38, FsrEvals = 39, FsrFails = 40,
            FsrLastResult = 41, FgKind = 42, FsrFgLastResult = 43,
        }

        // Must match Text in core/src/common.h.
        public enum Text { FsrUpVersions = 0, FsrFgVersions = 1, FsrScVersions = 2, FsrUpActive = 3, FsrFgActive = 4, FsrScActive = 5 }

        [DllImport(Dll)] static extern int MSF_GetText(int which, byte[] buffer, int length);
        [DllImport(Dll)] static extern void MSF_FsrSetDebug(int on);
        static readonly byte[] textBuffer = new byte[512];

        /// <summary>A text value of the native plugin ("" when not loaded).</summary>
        public static string GetText(Text which)
        {
            if (!loaded) return "";
            int n = MSF_GetText((int)which, textBuffer, textBuffer.Length);
            return n <= 0 ? "" : Encoding.UTF8.GetString(textBuffer, 0, Math.Min(n, textBuffer.Length));
        }

        /// <summary>Test switch: the FSR runtime checks its inputs (messages in msf.log).</summary>
        public static void FsrSetDebug(bool on)
        {
            if (loaded) MSF_FsrSetDebug(on ? 1 : 0);
        }

        [DllImport(Dll)] static extern void MSF_FgSetMode(int kind, int generatedFrames);
        [DllImport(Dll)] static extern int MSF_FgWindow();
        [DllImport(Dll, CharSet = CharSet.Unicode)] static extern void MSF_FgDump(string folder);

        static bool fgUsed;

        /// <summary>Frame generation: kind FgKindDlss / FgKindFsr, generated frames per real frame (0 = off).</summary>
        public static void FgSetMode(int kind, int generatedFrames)
        {
            if (!loaded) return;
            if (generatedFrames > 0) fgUsed = true;
            MSF_FgSetMode(kind, generatedFrames);
        }

        /// <summary>Every frame on the main thread once frame generation was used: the bridge's window.</summary>
        public static void FgTick()
        {
            if (loaded && fgUsed) MSF_FgWindow();
        }

        public static void FgDump(string folder)
        {
            if (loaded) MSF_FgDump(folder);
        }

        [DllImport(Dll)] static extern void MSF_ReflexSetMode(int mode);
        [DllImport(Dll)] static extern void MSF_ReflexMarker(int type);
        [DllImport(Dll)] static extern int MSF_ReflexReport(float[] values, int count);

        // NV_LATENCY_MARKER_TYPE
        public const int MarkerSimulationEnd = 1, MarkerRenderSubmitEnd = 3;

        /// <summary>Reflex: 0 off, 1 on, 2 on + boost.</summary>
        public static void ReflexSetMode(int mode)
        {
            if (loaded) MSF_ReflexSetMode(mode);
        }

        public static void ReflexMarker(int type)
        {
            if (loaded) MSF_ReflexMarker(type);
        }

        /// <summary>The driver's latency report of the last 64 frames: frames, latency (simulation start -> GPU done)
        /// ms, GPU busy ms, GPU frame ms, simulation ms, submit ms, frame interval ms, sleeps. 0 = none yet.</summary>
        public static int ReflexReport(float[] values)
        {
            return loaded && values != null && values.Length >= 8 ? MSF_ReflexReport(values, values.Length) : 0;
        }

        [DllImport(Dll)] static extern void MSF_FgSetGenerate(int on);
        [DllImport(Dll)] static extern void MSF_FgSetHudless(int on);

        /// <summary>Test switch: DLSS-G without the HUD-less picture when off.</summary>
        public static void FgSetHudless(bool on)
        {
            if (loaded) MSF_FgSetHudless(on ? 1 : 0);
        }

        /// <summary>Test switch: the bridge presents the real frames only (no DLSS-G) when off.</summary>
        public static void FgSetGenerate(bool on)
        {
            if (loaded) MSF_FgSetGenerate(on ? 1 : 0);
        }

        /// <summary>Must match kMaxBodies / BodyData (40 floats each) of the native plugin.</summary>
        public const int MaxBodies = 16;
        public const int BodyFloats = 40;

        [DllImport(Dll)]
        static extern void MSF_SetFrame(int slot, IntPtr color, IntPtr depth, IntPtr output, int width, int height, int outWidth,
                                            int outHeight, int quality, float[] data, float[] bodies, int bodyCount, int reset, int test,
                                            int flags);

        /// <summary>width/height: the render size (colour, depth); outWidth/outHeight: the upscaler's output (the
        /// screen); quality: NVSDK_NGX_PerfQuality_Value (DLSS) or FfxApiUpscaleQualityMode (FSR, logs only). data: jitter x/y, three column-major 4x4 matrices, motion sign x/y, frame
        /// ms (53 floats), then the frame generation camera (82 floats); bodies: bodyCount x 40 floats; flags: Frame*.</summary>
        public static void SetFrame(int slot, IntPtr color, IntPtr depth, IntPtr output, int width, int height, int outWidth, int outHeight,
                                    int quality, float[] data, float[] bodies, int bodyCount, bool reset, bool test, int flags)
        {
            if (loaded)
                MSF_SetFrame(slot, color, depth, output, width, height, outWidth, outHeight, quality, data, bodies, bodyCount,
                                 reset ? 1 : 0, test ? 1 : 0, flags);
        }

        [DllImport(Dll)] static extern int MSF_GetInt(int key);
        [DllImport(Dll, CharSet = CharSet.Unicode)] static extern void MSF_Configure(string logDir, string featureDir);
        [DllImport(Dll)] static extern int MSF_SetDeviceFromTexture(IntPtr texture);
        [DllImport(Dll, CharSet = CharSet.Ansi)] static extern void MSF_DescribeTexture(IntPtr texture, string tag);
        [DllImport(Dll)] static extern int MSF_DrainLog(byte[] buffer, int length);
        [DllImport(Dll)] static extern void MSF_Log(byte[] utf8);

        static bool loaded, failed;
        static string loadError;
        static readonly byte[] drainBuffer = new byte[16384];

        public static bool Loaded => loaded;
        public static string LoadError => loadError;

        /// <summary>First call into the plugin: Unity loads it here. False when it cannot be loaded.</summary>
        public static bool Load(string logDir, string featureDir)
        {
            if (loaded) return true;
            if (failed) return false;
            try
            {
                MSF_Configure(logDir, featureDir);
                int version = MSF_GetInt((int)Key.ApiVersion);
                if (version != ApiVersion)
                {
                    failed = true;
                    loadError = "native plugin version " + version + ", the mod needs " + ApiVersion;
                    return false;
                }
                loaded = true;
                return true;
            }
            catch (Exception e)
            {
                failed = true;
                loadError = e.GetType().Name + ": " + e.Message;
                return false;
            }
        }

        public static int Get(Key key) => loaded ? MSF_GetInt((int)key) : 0;

        public static void Issue(int ev)
        {
            if (loaded) GL.IssuePluginEvent(ev);
        }

        public static bool DeviceFromTexture(Texture texture)
        {
            if (!loaded || texture == null) return false;
            return MSF_SetDeviceFromTexture(texture.GetNativeTexturePtr()) != 0;
        }

        public static void Describe(Texture texture, string tag)
        {
            if (!loaded) return;
            MSF_DescribeTexture(texture == null ? IntPtr.Zero : texture.GetNativeTexturePtr(), tag);
        }

        /// <summary>A line in msf.log (UTF-8, like the file: Russian texts stay readable).</summary>
        public static void Log(string text)
        {
            if (loaded) MSF_Log(Encoding.UTF8.GetBytes((text ?? "") + "\0"));
        }

        /// <summary>Console lines written by the plugin since the last call (whole lines), or null.</summary>
        public static string Drain()
        {
            if (!loaded) return null;
            int n = MSF_DrainLog(drainBuffer, drainBuffer.Length);
            return n <= 0 ? null : Encoding.UTF8.GetString(drainBuffer, 0, n);
        }
    }
}
