// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// DLSS or FSR on the player's camera (PLAYER/.../FPSCamera: deferred, HDR, the game's image effects up to
// Tonemapping). Added last, so it runs after the game's effects on the tonemapped picture; the game's FXAA
// (Antialiasing) is switched off while an upscaler is on. Both upscalers take the same inputs. Per frame:
//   OnPreCull      upscaling modes: the camera draws into a smaller render texture (screen size / scale);
//                  sub-pixel jitter (Halton 2,3) on the projection matrix;
//   OnRenderImage  colour + depth copies -> native event: motion vectors from depth + matrices, DLSS.
//                  DLAA: the result goes to the camera's own destination (the screen).
//                  Upscaling: the result (screen size) is drawn to the screen by the Composite camera, which renders
//                  right after this one and before the hand camera (depth 2) and the HUD (depth 4); it also gives the
//                  camera its screen target back, so the game's scripts (raycasts from the mouse position, ...) see
//                  the screen-sized camera outside rendering.
// The hand camera (depth 2) and the HUD camera (depth 4) draw after this one, on top.

using System;
using UnityEngine;

namespace MySummerFrames
{
    sealed class DlssCamera : MonoBehaviour
    {
        public static DlssCamera Current { get; private set; }

        // Modes: the same scales for both upscalers; mode 1 is anti-aliasing at the game's resolution (DLAA / FSR Native AA).
        public const int ModeOff = 0, ModeDlaa = 1, ModeQuality = 2, ModeBalanced = 3, ModePerformance = 4, ModeUltra = 5;
        public static readonly string[] ModeNames = { "Off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };
        public static readonly string[] FsrModeNames = { "Off", "Native AA", "Quality", "Balanced", "Performance", "Ultra Performance" };
        static readonly string[] ModeKeys = { "off", "dlaa", "quality", "balanced", "performance", "ultra" };
        // Screen size / render size (DLSS programming guide 3.7.1.1; FSR: FfxApiUpscaleQualityMode ratios) and
        // NVSDK_NGX_PerfQuality_Value.
        static readonly float[] DlssScale = { 1f, 1f, 1.5f, 1.7241f, 2f, 3f };
        static readonly float[] FsrScale = { 1f, 1f, 1.5f, 1.7f, 2f, 3f };
        static readonly int[] NgxQuality = { 5, 5, 2, 1, 0, 3 };

        /// <summary>Upscalers (0: none).</summary>
        public const int UpscalerDlss = 1, UpscalerFsr = 2;

        public static int ModeFromName(string name)
        {
            if (name == "native") return ModeDlaa;
            for (int i = 0; i < ModeKeys.Length; i++) if (ModeKeys[i] == name) return i;
            return -1;
        }

        public static string NameOf(int upscaler, int mode)
        {
            if (mode == ModeOff || upscaler == 0) return "Off";
            return (upscaler == UpscalerFsr ? "FSR " + FsrModeNames[mode] : mode == ModeDlaa ? "DLAA" : "DLSS " + ModeNames[mode]);
        }

        /// <summary>Mode wanted (the player's choice).</summary>
        public int Mode = ModeDlaa;
        /// <summary>Upscaler wanted (DLSS or FSR). DLSS without an RTX card falls back to FSR.</summary>
        public int Upscaler = UpscalerDlss;
        /// <summary>Sharpening after the upscaler, 0 (off) .. 1 (FSR: its RCAS; DLSS: the plugin's RCAS pass).</summary>
        public float Sharpness;
        /// <summary>Direction of the jitter offset in the projection matrix (tests of the convention).</summary>
        public float JitterSignX = 1f, JitterSignY = 1f;
        public float MvSignX = 1f, MvSignY = 1f;
        /// <summary>Reprojection test of the motion vectors (logged every 60 frames).</summary>
        public bool Test;
        /// <summary>Extra mip bias on top of DLSS's recommendation while upscaling (tests).</summary>
        public float MipBiasExtra;

        /// <summary>Moving vehicles in the motion vectors (off = camera motion only, for comparison).</summary>
        public bool UseBodies = true;
        /// <summary>Frame generation wanted (generated frames per real frame, 0 = off): the depth, motion and camera
        /// of every frame go to the bridge.</summary>
        public int FgFrames;
        /// <summary>Render textures are upside down against the back buffer (Direct3D): flip for frame generation.</summary>
        public bool FgFlip = true;

        Camera cam;
        RenderTexture colorRT, outputRT, depthRT;
        IntPtr colorPtr, outputPtr, depthPtr;
        RenderTexture lowRT;          // the camera's target while upscaling
        Composite composite;
        readonly BodyTracker bodies = new BodyTracker();
        int width, height, outWidth, outHeight;

        // Unity 5.0 cannot hand a depth buffer to a plugin, has no shader compiler in the player and the scene's depth
        // buffer is not bound after rendering (deferred). A fixed-function pass (compiled by the player itself) copies
        // the global _CameraDepthTexture (the game's image effects already ask for it) into our float texture.
        const string DepthShader =
            "Shader \"Hidden/MySummerFrames/CopyDepth\" { SubShader { Pass { ZTest Always Cull Off ZWrite Off Fog { Mode Off } " +
            "SetTexture [_CameraDepthTexture] { combine texture } } } }";
        static Material depthMaterial;
        static bool depthMaterialFailed;
        RenderTextureFormat format;
        uint frame;
        int slot;
        bool jittered, havePrev, resetNext, upscaling, upscaledThisFrame;
        int activeMode = -1;
        Vector2 jitter;
        Matrix4x4 projNoJitter, projJitter, view, prevView, prevGpuProj, prevScreenProj;
        Vector3 camPos, prevCamPos;
        Behaviour fxaa;
        bool fxaaOff;
        readonly float[] data = new float[Native.FrameFloats];

        public static DlssCamera Attach(Camera c)
        {
            if (c == null) return null;
            DlssCamera d = c.GetComponent<DlssCamera>();
            if (d == null) d = c.gameObject.AddComponent<DlssCamera>();
            Current = d;
            return d;
        }

        public bool Running => jittered;

        /// <summary>Outside rendering the camera must look like the game's own (screen target, screen-sized rays):
        /// the angle between a ray through the screen's centre and the camera's forward axis, and whether a target is
        /// set (both checked every frame in Update by the Runner).</summary>
        public float CenterRayAngle => cam != null ? Vector3.Angle(cam.ScreenPointToRay(new Vector3(Screen.width * 0.5f, Screen.height * 0.5f, 0f)).direction, cam.transform.forward) : 0f;
        public bool TargetSet => cam != null && cam.targetTexture != null;
        public string RenderSize => width + "x" + height + (upscaling ? " -> " + outWidth + "x" + outHeight : "");
        public void ResetHistory() => resetNext = true;

        void Awake() => cam = GetComponent<Camera>();

        // Before this frame's rendering: the composite camera must exist when the camera first draws into the small
        // target (a camera made during rendering only renders from the next frame: the screen target would stay set
        // through the next Update, with the game's raycasts 40 degrees off).
        void Update()
        {
            if (Wanted && EffectiveMode >= ModeQuality) EnsureComposite();
        }

        void OnDisable() => Stop();

        void OnDestroy()
        {
            Stop();
            if (composite != null) Destroy(composite.gameObject);
            if (Current == this) Current = null;
        }

        public static bool DlssAvailable => Native.Get(Native.Key.SrAvailable) != 0;
        public static bool FsrAvailable => Native.Get(Native.Key.FsrUpAvailable) == 1;

        /// <summary>The upscaler that runs for the wanted one (0: none): DLSS needs an RTX card, else FSR runs.</summary>
        public static int Resolve(int upscaler, int mode)
        {
            if (mode == ModeOff) return 0;
            if (upscaler == UpscalerDlss && DlssAvailable) return UpscalerDlss;
            return FsrAvailable ? UpscalerFsr : 0;
        }

        public int EffectiveUpscaler => Resolve(Upscaler, Mode);
        bool UpscaleOn => EffectiveUpscaler != 0;
        bool FgOn => FgFrames > 0 && (Native.Get(Native.Key.FgAvailable) != 0 || Native.Get(Native.Key.FsrFgAvailable) == 1);
        bool Wanted => Native.Loaded && Native.Get(Native.Key.InitState) == 2 && (UpscaleOn || FgOn);
        int EffectiveMode => UpscaleOn ? Mode : ModeOff;
        float[] Scale => EffectiveUpscaler == UpscalerFsr ? FsrScale : DlssScale;
        public string ModeLabel => NameOf(EffectiveUpscaler, EffectiveMode);

        void Stop()
        {
            if (cam != null)
            {
                if (jittered) cam.ResetProjectionMatrix();
                if (upscaling) cam.targetTexture = null;
            }
            jittered = false;
            upscaling = false;
            Release();
            RestoreGui();
            RestoreFxaa();
            MipBias.Want(0f);
            if (composite != null) composite.enabled = false;
        }

        void OnPreCull()
        {
            jittered = false;
            upscaledThisFrame = false;
            if (!Wanted)
            {
                if (colorRT != null || upscaling) Stop();
                return;
            }
            int mode = EffectiveMode;
            int upscaler = EffectiveUpscaler;
            if (mode + 10 * upscaler != activeMode)
            {
                activeMode = mode + 10 * upscaler;
                Release(); // new sizes, new textures (FSR and DLSS keep their own history)
                Native.Log("Upscaler: " + NameOf(upscaler, mode));
            }
            float[] scale = Scale;
            if (mode != ModeOff) KeepFxaaOff();
            else RestoreFxaa();

            // Upscaling: the camera draws into a smaller texture (given back to the screen by Composite).
            int screenW = Screen.width, screenH = Screen.height;
            bool up = mode >= ModeQuality && cam.rect == new Rect(0, 0, 1, 1);
            if (up)
            {
                int rw = Mathf.Max(64, Mathf.RoundToInt(screenW / scale[mode]));
                int rh = Mathf.Max(64, Mathf.RoundToInt(screenH / scale[mode]));
                if (lowRT == null || lowRT.width != rw || lowRT.height != rh)
                {
                    ReleaseLow();
                    lowRT = new RenderTexture(rw, rh, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default) { name = "MSF render" };
                    if (!lowRT.Create()) { ReleaseLow(); up = false; }
                }
            }
            if (up)
            {
                cam.targetTexture = lowRT;
                EnsureComposite();
                composite.enabled = true;
                MoveGuiToComposite();
                MipBias.Want(Mathf.Log((float)lowRT.width / screenW, 2f) - 1f + MipBiasExtra);
            }
            else
            {
                if (upscaling) cam.targetTexture = null;
                ReleaseLow();
                RestoreGui();
                if (composite != null) composite.enabled = false;
                MipBias.Want(0f);
            }
            upscaling = up;
            int rW = up ? lowRT.width : cam.pixelWidth, rH = up ? lowRT.height : cam.pixelHeight;

            cam.ResetProjectionMatrix();
            projNoJitter = cam.projectionMatrix;
            frame++;
            // Phases: 8 * scale^2 (DLSS programming guide 3.7.1.1, FSR's table): DLAA / Native AA 8, Quality 18,
            // Balanced 24 (FSR 23), Performance 32, Ultra 72. Halton (2, 3) is FSR's own sequence too.
            int phases = Mathf.Max(8, Mathf.RoundToInt(8f * scale[mode] * scale[mode]));
            int i = (int)(frame % (uint)phases) + 1;
            // Frame generation alone: no jitter (the picture is the game's own).
            jitter = mode != ModeOff ? new Vector2(Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f) : Vector2.zero;
            // jitter = how far the picture moves in the texture, in render pixels (x right, y down in memory). Unity's
            // projection is GL-style and Direct3D render textures are stored upside down: m02 += d moves the picture
            // by -d*W/2 pixels, m12 += d by -d*H/2 rows.
            Matrix4x4 p = projNoJitter;
            p.m02 -= JitterSignX * 2f * jitter.x / rW;
            p.m12 -= JitterSignY * 2f * jitter.y / rH;
            cam.projectionMatrix = p;
            projJitter = p;
            view = cam.worldToCameraMatrix;
            camPos = cam.transform.position;
            bodies.Update(transform, camPos);
            jittered = true;
        }

        public int BodyCount => bodies.Count;
        public string Carrier => bodies.LastCarrier;

        void OnPostRender()
        {
            if (jittered) cam.ResetProjectionMatrix();
        }

        void OnRenderImage(RenderTexture src, RenderTexture dst)
        {
            int ow = upscaling ? Screen.width : (src != null ? src.width : 0);
            int oh = upscaling ? Screen.height : (src != null ? src.height : 0);
            if (!jittered || src == null || !EnsureTargets(src, ow, oh))
            {
                Graphics.Blit(src, dst);
                return;
            }
            Graphics.Blit(src, colorRT);
            Graphics.Blit(src, depthRT, depthMaterial);

            Matrix4x4 toWorld = Matrix4x4.TRS(camPos, Quaternion.identity, Vector3.one);
            Matrix4x4 gpuJitter = GL.GetGPUProjectionMatrix(projJitter, true);
            Matrix4x4 gpuProj = GL.GetGPUProjectionMatrix(projNoJitter, true);
            Matrix4x4 viewRel = view * toWorld;
            Matrix4x4 vp = gpuProj * viewRel;
            Matrix4x4 invVpJitter = (gpuJitter * viewRel).inverse;
            Matrix4x4 prevVp = havePrev ? prevGpuProj * prevView * toWorld : vp;
            bool reset = !havePrev || resetNext || (camPos - prevCamPos).sqrMagnitude > 16f;

            data[0] = jitter.x;
            data[1] = jitter.y;
            Put(invVpJitter, 2);
            Put(vp, 18);
            Put(prevVp, 34);
            data[50] = MvSignX;
            data[51] = MvSignY;
            data[52] = Time.unscaledDeltaTime * 1000f;

            // Frame generation works on the back buffer: screen-oriented clip space (no render-texture flip).
            Matrix4x4 screenProj = GL.GetGPUProjectionMatrix(projNoJitter, false);
            Matrix4x4 vpScreen = screenProj * viewRel;
            Matrix4x4 prevVpScreen = havePrev ? prevScreenProj * prevView * toWorld : vpScreen;
            Matrix4x4 clipToPrev = prevVpScreen * vpScreen.inverse;
            PutRows(screenProj, 53);
            PutRows(screenProj.inverse, 69);
            PutRows(clipToPrev, 85);
            PutRows(clipToPrev.inverse, 101);
            Put3(camPos, 117);
            Put3(transform.up, 120);
            Put3(transform.right, 123);
            Put3(transform.forward, 126);
            data[129] = cam.nearClipPlane;
            data[130] = cam.farClipPlane;
            data[131] = cam.fieldOfView * Mathf.Deg2Rad;
            data[132] = cam.aspect;
            data[133] = jitter.x;
            data[134] = FgFlip ? -jitter.y : jitter.y;
            int mode = EffectiveMode;
            data[135] = mode != ModeOff ? Mathf.Clamp01(Sharpness) : 0f;
            bool fsr = EffectiveUpscaler == UpscalerFsr;
            int flags = (mode != ModeOff ? Native.FrameUpscale : 0) | (fsr ? Native.FrameFsr : 0) | (FgOn ? Native.FrameFg : 0) |
                        (FgFlip ? Native.FrameFlip : 0);
            int quality = fsr ? Mathf.Max(0, mode - 1) : NgxQuality[mode];

            Native.SetFrame(slot, colorPtr, depthPtr, outputPtr, width, height, outWidth, outHeight, quality, data, bodies.Data,
                            UseBodies ? bodies.Count : 0, reset, Test, flags);
            Native.Issue(Native.EventEvaluate + slot);
            slot = (slot + 1) % Native.Slots;
            // DLAA: straight to the camera's destination. Upscaling: Composite draws it on the screen; the camera's
            // own small target is not shown.
            if (!upscaling) Graphics.Blit(outputRT, dst);
            else upscaledThisFrame = true;

            prevView = view;
            prevGpuProj = gpuProj;
            prevScreenProj = screenProj;
            prevCamPos = camPos;
            havePrev = true;
            resetNext = false;
        }

        /// <summary>Composite: the screen target back, and this frame's DLSS output on the screen.</summary>
        internal void AfterCamera()
        {
            if (upscaling && cam != null) cam.targetTexture = null;
        }

        internal RenderTexture Output => upscaledThisFrame ? outputRT : null;

        void EnsureComposite()
        {
            if (composite != null) return;
            GameObject go = new GameObject("MSF composite");
            go.transform.SetParent(transform, false);
            Camera c = go.AddComponent<Camera>();
            c.clearFlags = CameraClearFlags.Nothing;
            c.cullingMask = 0;
            c.depth = cam.depth + 0.5f; // after the player's camera (1), before the hand camera (2)
            c.renderingPath = RenderingPath.Forward;
            c.hdr = false;
            c.useOcclusionCulling = false;
            // A frustum of 1 cm: whatever layers it has for its GUILayer, no geometry is in it.
            c.nearClipPlane = 0.01f;
            c.farClipPlane = 0.02f;
            go.AddComponent<GUILayer>().enabled = false;
            composite = go.AddComponent<Composite>();
            composite.Owner = this;
        }

        void Put(Matrix4x4 m, int at)
        {
            // Column-major, as HLSL reads a float4x4 constant by default.
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    data[at + c * 4 + r] = m[r, c];
        }

        void PutRows(Matrix4x4 m, int at)
        {
            // Row-major for column vectors: DLSS-G's float[4][4].
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    data[at + r * 4 + c] = m[r, c];
        }

        void Put3(Vector3 v, int at)
        {
            data[at] = v.x;
            data[at + 1] = v.y;
            data[at + 2] = v.z;
        }

        bool EnsureTargets(RenderTexture src, int ow, int oh)
        {
            if (colorRT != null && width == src.width && height == src.height && format == src.format && outWidth == ow && outHeight == oh)
                return true;
            Release();
            width = src.width;
            height = src.height;
            outWidth = ow;
            outHeight = oh;
            format = src.format;
            // Depth, motion vectors and the DLSS textures themselves are the plugin's own (typed formats).
            if (depthMaterial == null && !depthMaterialFailed)
            {
                depthMaterial = new Material(DepthShader) { hideFlags = HideFlags.HideAndDontSave };
                if (depthMaterial.shader == null || !depthMaterial.shader.isSupported)
                {
                    depthMaterialFailed = true;
                    depthMaterial = null;
                    Say.Problem("the depth copy shader is not supported");
                }
            }
            if (depthMaterial == null)
            {
                Mode = ModeOff;
                return false;
            }
            colorRT = NewTarget(width, height, format, "colour");
            outputRT = NewTarget(outWidth, outHeight, format, "output");
            depthRT = NewTarget(width, height, RenderTextureFormat.RFloat, "depth");
            if (colorRT == null || outputRT == null || depthRT == null)
            {
                Release();
                Mode = ModeOff;
                Say.Problem("cannot create the render textures, the upscaler is off");
                return false;
            }
            colorPtr = colorRT.GetNativeTexturePtr();
            outputPtr = outputRT.GetNativeTexturePtr();
            depthPtr = depthRT.GetNativeTexturePtr();
            havePrev = false;
            Native.Describe(src, "camera source " + src.format);
            Native.Log("Upscaler targets " + width + "x" + height + " -> " + outWidth + "x" + outHeight + " " + format + " (" +
                       ModeLabel + ") on " + Diag.Path(transform));
            return true;
        }

        static RenderTexture NewTarget(int w, int h, RenderTextureFormat f, string name)
        {
            RenderTexture rt = new RenderTexture(w, h, 0, f, RenderTextureReadWrite.Linear);
            rt.name = "MSF " + name;
            rt.filterMode = FilterMode.Point;
            rt.wrapMode = TextureWrapMode.Clamp;
            if (!rt.Create())
            {
                Destroy(rt);
                return null;
            }
            return rt;
        }

        void Release()
        {
            foreach (RenderTexture rt in new[] { colorRT, outputRT, depthRT })
                if (rt != null) { rt.Release(); Destroy(rt); }
            colorRT = outputRT = depthRT = null;
            colorPtr = outputPtr = depthPtr = IntPtr.Zero;
            width = height = outWidth = outHeight = 0;
            havePrev = false;
            if (!upscaling) ReleaseLow();
        }

        void ReleaseLow()
        {
            if (lowRT == null) return;
            if (cam != null && cam.targetTexture == lowRT) cam.targetTexture = null;
            lowRT.Release();
            Destroy(lowRT);
            lowRT = null;
        }

        // The camera's GUILayer (the crosshair, the interaction icons and texts: GUITexture / GUIText) draws after the
        // image effects into the camera's target: while upscaling that is the small texture, which is not shown. The
        // composite camera draws them instead, on the screen, after the upscaled picture (same order as the game's).
        // The game's GUI elements (GUI/Crosshair and GUI/Icons/GUITexture) are on the Default layer, with ~2500 renderers:
        // a camera that draws that layer's GUI culls all of them. So, while upscaling, the elements this camera would
        // draw go to an unused layer that only the composite camera has (and get their own layer back afterwards).
        GUILayer guiLayer;
        bool guiMoved;
        float nextGuiScan;
        static int spareLayer = -2;
        readonly System.Collections.Generic.List<GUIElement> movedElements = new System.Collections.Generic.List<GUIElement>();
        readonly System.Collections.Generic.List<int> movedLayers = new System.Collections.Generic.List<int>();

        static int SpareLayer()
        {
            if (spareLayer != -2) return spareLayer;
            spareLayer = -1;
            for (int i = 31; i >= 8; i--)
                if (string.IsNullOrEmpty(LayerMask.LayerToName(i))) { spareLayer = i; break; }
            Native.Log("GUI elements while upscaling: layer " + spareLayer);
            return spareLayer;
        }

        void MoveGuiToComposite()
        {
            if (guiLayer == null) guiLayer = GetComponent<GUILayer>();
            int spare = SpareLayer();
            if (guiLayer == null || composite == null || spare < 0) return;
            if (!guiMoved)
            {
                if (!guiLayer.enabled) { composite.SetGui(false, 0); return; } // the game has it off
                guiLayer.enabled = false;
                guiMoved = true;
                nextGuiScan = 0f;
            }
            if (Time.realtimeSinceStartup >= nextGuiScan)
            {
                // Once a second: elements made since (by the game or other mods) join.
                nextGuiScan = Time.realtimeSinceStartup + 1f;
                foreach (GUIElement e in FindObjectsOfType<GUIElement>())
                {
                    int layer = e.gameObject.layer;
                    if (layer == spare || (cam.cullingMask & (1 << layer)) == 0) continue;
                    movedElements.Add(e);
                    movedLayers.Add(layer);
                    e.gameObject.layer = spare;
                }
            }
            composite.SetGui(true, 1 << spare);
        }

        void RestoreGui()
        {
            for (int i = 0; i < movedElements.Count; i++)
                if (movedElements[i] != null && movedElements[i].gameObject.layer == spareLayer) movedElements[i].gameObject.layer = movedLayers[i];
            movedElements.Clear();
            movedLayers.Clear();
            if (guiMoved && guiLayer != null) guiLayer.enabled = true;
            guiMoved = false;
            if (composite != null) composite.SetGui(false, 0);
        }

        void KeepFxaaOff()
        {
            if (fxaa == null)
                foreach (Behaviour b in GetComponents<Behaviour>())
                    if (b != null && b.GetType().FullName == "UnityStandardAssets.ImageEffects.Antialiasing") { fxaa = b; break; }
            if (fxaa != null && fxaa.enabled)
            {
                fxaa.enabled = false;
                fxaaOff = true;
            }
        }

        void RestoreFxaa()
        {
            if (fxaaOff && fxaa != null) fxaa.enabled = true;
            fxaaOff = false;
        }

        static float Halton(int index, int b)
        {
            float f = 1f, r = 0f;
            while (index > 0)
            {
                f /= b;
                r += f * (index % b);
                index /= b;
            }
            return r;
        }
    }

    /// <summary>The camera right after the player's: gives the player's camera its screen target back, draws the
    /// upscaled picture on the screen and then the player camera's GUI elements (its GUILayer moved here), before the
    /// hands and the HUD are drawn over them.</summary>
    sealed class Composite : MonoBehaviour
    {
        public DlssCamera Owner;
        Camera cam;
        GUILayer gui;

        void Awake()
        {
            cam = GetComponent<Camera>();
            gui = GetComponent<GUILayer>();
        }

        /// <summary>on: the GUILayer draws the GUI elements of these layers (the camera renders nothing else).</summary>
        public void SetGui(bool on, int mask)
        {
            if (cam != null) cam.cullingMask = on ? mask : 0;
            if (gui != null && gui.enabled != on) gui.enabled = on;
        }

        void OnPreCull()
        {
            if (Owner != null) Owner.AfterCamera();
        }

        // OnPostRender: this camera's viewport (the whole screen) is set (in OnPreRender the player camera's small one
        // still is). Its GUILayer draws after OnPostRender (after the image effects), on top of the picture.
        void OnPostRender()
        {
            RenderTexture output = Owner != null ? Owner.Output : null;
            if (output != null) Graphics.Blit(output, (RenderTexture)null);
        }
    }
}
