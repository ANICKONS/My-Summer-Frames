// My Summer Frames - DLSS, FSR, Frame Generation & Reflex for My Summer Car
// Copyright (C) 2026 ANICKON
//
// The MSCLoader side: installs and starts the native plugin, puts the upscaler (NVIDIA DLSS or AMD FSR) on the
// player's camera, frame generation (DLSS or FSR) through the native bridge, Reflex, console command "msf". The
// settings page is SettingsPage, the console lines go through Say; the image work is in the native plugin.

using System;
using System.Collections;
using System.IO;
using MSCLoader;
using UnityEngine;

namespace MySummerFrames
{
    public class MsfMod : Mod
    {
        public const string ModId = "MySummerFrames";
        public const string ModVersion = "0.5.0";

        public override string ID => ModId;
        public override string Name => "My Summer Frames";
        public override string Author => "ANICKON";
        public override string Version => ModVersion;
        public override string Description => "NVIDIA DLSS and AMD FSR for My Summer Car: anti-aliasing, upscaling, frame generation, Reflex.";
        /// <summary>The mod list icon: Resources\icon.png, embedded as MySummerFrames.Resources.icon.png.</summary>
        public override byte[] Icon => icon ?? (icon = ReadResource("MySummerFrames.Resources.icon.png"));
        static byte[] icon;

        internal static MsfMod Instance { get; private set; }
        internal static string SettingsDir { get; private set; }
        internal static string AssetsDir { get; private set; }
        /// <summary>Upscaler wanted (DlssCamera.UpscalerDlss / UpscalerFsr; DLSS without an RTX card runs FSR).</summary>
        internal static int WantUpscaler = DlssCamera.UpscalerDlss;
        /// <summary>Mode wanted (DlssCamera.Mode*; survives the camera being made again on a load). Off = no upscaler.</summary>
        internal static int WantMode = DlssCamera.ModeDlaa;
        /// <summary>Sharpening after the upscaler in tenths: 0 off .. 10 full.</summary>
        internal static int WantSharpness;
        /// <summary>Frame generation wanted: 0 off, Native.FgKindDlss, Native.FgKindFsr.</summary>
        internal static int WantFgKind;
        /// <summary>DLSS frame generation: generated frames per real frame (1 = 2x ... 5 = 6x).</summary>
        internal static int WantFg = 1;
        /// <summary>The frame generation counter under the game's FPS counter (FgHud).</summary>
        internal static bool ShowFgFps = true;
        /// <summary>Reflex wanted: 0 off, 1 on, 2 on + boost.</summary>
        internal static int WantReflex = 1;
        internal static readonly string[] ReflexNames = { "off", "on", "on + boost" };

        /// <summary>Generated frames per real frame wanted (0 = off; FSR makes one).</summary>
        internal static int FgFramesWanted => WantFgKind == 0 ? 0 : WantFgKind == Native.FgKindFsr ? 1 : Mathf.Clamp(WantFg, 1, 5);

        static byte[] ReadResource(string name)
        {
            using (Stream s = typeof(MsfMod).Assembly.GetManifestResourceStream(name))
            {
                if (s == null) return new byte[0];
                byte[] data = new byte[s.Length];
                int offset = 0, read;
                while (offset < data.Length && (read = s.Read(data, offset, data.Length - offset)) > 0)
                    offset += read;
                return data;
            }
        }

        static bool started, commandAdded, settingsLoaded;
        internal static Runner Host { get; private set; }

        public override void ModSetup()
        {
            Instance = this;
            SetupFunction(Setup.OnMenuLoad, OnMenuLoad);
            SetupFunction(Setup.ModSettings, OnModSettings);
            SetupFunction(Setup.ModSettingsLoaded, OnModSettingsLoaded);
        }

        void OnModSettings() => SettingsPage.Build(ApplySettings);

        void OnModSettingsLoaded()
        {
            SettingsPage.ApplyLanguage();
            ApplySettings();
            settingsLoaded = true;
        }

        /// <summary>The settings page's choices (also on load).</summary>
        internal static void ApplySettings()
        {
            if (SettingsPage.Upscaler == null) return;
            int u = SettingsPage.Upscaler.GetSelectedItemIndex();
            SettingsDropDownList modes = u == 2 ? SettingsPage.FsrMode : SettingsPage.DlssMode;
            WantUpscaler = u == 2 ? DlssCamera.UpscalerFsr : DlssCamera.UpscalerDlss;
            WantMode = u <= 0 ? DlssCamera.ModeOff : DlssCamera.ModeDlaa + Mathf.Clamp(modes.GetSelectedItemIndex(), 0, 4);
            WantSharpness = Mathf.Clamp(SettingsPage.Sharpness.GetValue(), 0, 10);
            int f = SettingsPage.FgType.GetSelectedItemIndex();
            WantFgKind = f == 1 ? Native.FgKindDlss : f == 2 ? Native.FgKindFsr : 0;
            WantFg = 1 + Mathf.Clamp(SettingsPage.DlssFg.GetSelectedItemIndex(), 0, 4);
            ShowFgFps = SettingsPage.FgCounter.GetValue();
            WantReflex = Mathf.Clamp(SettingsPage.Reflex.GetSelectedItemIndex(), 0, 2);
            bool verbose = SettingsPage.Verbose.GetValue();
            if (verbose != Say.Verbose)
            {
                Say.Verbose = verbose;
                if (settingsLoaded) Say.Info("con.verbose", Loc.OnOff(verbose));
            }
            SettingsPage.UpdateVisibility();
            ApplyToCamera();
        }

        /// <summary>The wanted upscaler and frame generation to the camera and the bridge (settings or console).</summary>
        internal static void ApplyToCamera()
        {
            DlssCamera d = DlssCamera.Current;
            if (d != null)
            {
                d.Mode = WantMode;
                d.Upscaler = WantUpscaler;
                d.Sharpness = WantSharpness / 10f;
                d.FgFrames = FgFramesWanted;
            }
            if (Host != null)
            {
                Host.FgDirty = true;
                Host.ReflexDirty = true;
            }
        }

        void OnMenuLoad()
        {
            if (!commandAdded)
            {
                commandAdded = true;
                ConsoleCommand.Add(new MsfCommand());
            }
            if (started) return;
            started = true;
            SettingsDir = ModLoader.GetModSettingsFolder(this);
            AssetsDir = ModLoader.GetModAssetsFolder(this);
            try { Directory.CreateDirectory(SettingsDir); } catch { }

            string err = Installer.EnsurePlugin(AssetsDir);
            if (err == null && !Native.Load(SettingsDir, AssetsDir)) err = Native.LoadError;
            if (err != null) { Say.Problem(Loc.F("con.plugin", err)); return; }

            GameObject go = new GameObject("MySummerFrames");
            UnityEngine.Object.DontDestroyOnLoad(go);
            Host = go.AddComponent<Runner>();
            Host.StartCoroutine(Host.Init());
        }

        static string FgName(int kind, int frames) =>
            frames <= 0 || kind == 0 ? "off" : (kind == Native.FgKindFsr ? "FSR " : "DLSS ") + (frames + 1) + "x";

        /// <summary>The technical state (English): "dlss status", the detailed log at the start.</summary>
        internal static string Status()
        {
            if (!Native.Loaded) return "native plugin not loaded" + (Native.LoadError != null ? " (" + Native.LoadError + ")" : "");
            int state = Native.Get(Native.Key.InitState);
            string s = "My Summer Frames " + ModVersion + ": device type " + Native.Get(Native.Key.DeviceType) +
                       " (source " + Native.Get(Native.Key.DeviceSource) + "), render events " + Native.Get(Native.Key.RenderEvents) +
                       ", feature level 0x" + Native.Get(Native.Key.FeatureLevel).ToString("X") +
                       ", D3D11.4 " + (Native.Get(Native.Key.HasD3d11_4) != 0) + "\n";
            if (state != 2) return s + "  NGX: " + (state == 0 ? "not started" : "starting");
            s += "  DLSS/DLAA (D3D11): NGX 0x" + Native.Get(Native.Key.Ngx11Result).ToString("X8") +
                 ", available " + (Native.Get(Native.Key.SrAvailable) != 0) +
                 ", needs driver " + (Native.Get(Native.Key.SrNeedsDriver) != 0) + " (min " + Native.Get(Native.Key.SrMinDriverMajor) + "." + Native.Get(Native.Key.SrMinDriverMinor) + ")" +
                 ", init 0x" + Native.Get(Native.Key.SrInitResult).ToString("X8") + "\n";
            s += "  DLSS Frame Generation (D3D12 bridge): device 0x" + Native.Get(Native.Key.D3d12Result).ToString("X8") +
                 ", NGX 0x" + Native.Get(Native.Key.Ngx12Result).ToString("X8") +
                 ", available " + (Native.Get(Native.Key.FgAvailable) != 0) +
                 ", max generated frames " + Native.Get(Native.Key.FgMultiFrameMax) +
                 ", needs driver " + (Native.Get(Native.Key.FgNeedsDriver) != 0) + " (min " + Native.Get(Native.Key.FgMinDriverMajor) + "." + Native.Get(Native.Key.FgMinDriverMinor) + ")" +
                 ", init 0x" + Native.Get(Native.Key.FgInitResult).ToString("X8") + "\n";
            s += "  FSR (D3D12): loader " + Native.Get(Native.Key.FsrLoaded) +
                 ", upscaler [" + Native.GetText(Native.Text.FsrUpVersions) + "] in use " + Or(Native.GetText(Native.Text.FsrUpActive)) +
                 ", frame generation [" + Native.GetText(Native.Text.FsrFgVersions) + "] in use " + Or(Native.GetText(Native.Text.FsrFgActive)) +
                 ", swap chain [" + Native.GetText(Native.Text.FsrScVersions) + "] in use " + Or(Native.GetText(Native.Text.FsrScActive)) + "\n";
            DlssCamera d = DlssCamera.Current;
            s += "  Upscaler: wanted " + DlssCamera.NameOf(WantUpscaler, WantMode) + ", sharpness " + WantSharpness * 10 + "%" +
                 ", runs " + (d != null ? d.ModeLabel : "-") +
                 ", camera " + (d != null ? Diag.Path(d.transform) : "none") +
                 ", running " + (d != null && d.Running) + (d != null && d.Running ? " at " + d.RenderSize : "") +
                 "; DLSS create 0x" + Native.Get(Native.Key.DlssCreateResult).ToString("X8") +
                 ", evaluations " + Native.Get(Native.Key.DlssEvals) + ", failures " + Native.Get(Native.Key.DlssFails) +
                 " (last 0x" + Native.Get(Native.Key.DlssEvalResult).ToString("X8") + ")" +
                 "; FSR create " + Native.Get(Native.Key.FsrCreateResult) + ", evaluations " + Native.Get(Native.Key.FsrEvals) +
                 ", failures " + Native.Get(Native.Key.FsrFails) + " (last " + Native.Get(Native.Key.FsrLastResult) + ")" +
                 ", moving bodies " + (d != null ? d.BodyCount + (d.UseBodies ? "" : " (ignored)") + ", in " + (d.Carrier ?? "-") : "-") + "\n";
            int fgKind = Native.Get(Native.Key.FgKind);
            s += "  Frame generation: wanted " + FgName(WantFgKind, FgFramesWanted) + ", runs " +
                 (Host != null ? FgName(Host.FgKindApplied, Host.FgApplied) : "-") +
                 ", path " + (fgKind == Native.FgKindFsr ? "FSR" : fgKind == Native.FgKindDlss ? "DLSS" : "none") +
                 ", stage " + Native.Get(Native.Key.FgStage) +
                 ", real " + Native.Get(Native.Key.FgReal) + ", generated " + Native.Get(Native.Key.FgGenerated) +
                 ", failures " + Native.Get(Native.Key.FgFails) + " (DLSS 0x" + Native.Get(Native.Key.FgLastResult).ToString("X8") +
                 ", FSR " + Native.Get(Native.Key.FsrFgLastResult) + ")" +
                 ", real interval " + (Native.Get(Native.Key.FgIntervalUs) / 1000f).ToString("0.00") + " ms\n";
            s += "  Reflex: wanted " + ReflexNames[WantReflex] + ", available " + Native.Get(Native.Key.ReflexAvailable) +
                 ", mode set " + Native.Get(Native.Key.ReflexMode) + (Host != null ? ", " + Host.ReflexLine : "") + "\n";
            if (Host != null)
                s += "  Camera between frames: " + Host.CheckedFrames + " frames checked, target left set " + Host.TargetLeftSet +
                     ", centre ray off the camera axis by up to " + Host.MaxRayAngle.ToString("0.000") + " deg, mip bias " +
                     MipBias.Current.ToString("0.00") + " on " + MipBias.Textures + " textures";
            return s;
        }

        static string Or(string s) => string.IsNullOrEmpty(s) ? "-" : s;

        internal static void ProbeMainCamera()
        {
            Camera c = Camera.main;
            if (c == null) { Say.Answer("no main camera"); return; }
            if (c.GetComponent<TargetProbe>() == null) c.gameObject.AddComponent<TargetProbe>();
            Say.Answer("probing " + Diag.Path(c.transform) + " (see msf.log)");
        }
    }

    sealed class Runner : MonoBehaviour
    {
        public Spinner Spin;
        /// <summary>Frame generation wanted changed: applied on the next Update.</summary>
        public bool FgDirty = true;
        /// <summary>What runs: generated frames (0 = off) and the path (Native.FgKind*).</summary>
        public int FgApplied, FgKindApplied;
        float nextFgCheck;
        /// <summary>The native start is reported (the "ready" line): the state lines and notes may follow.</summary>
        bool ready;
        // The notes shown (keys of Strings): what runs differs from what is chosen.
        string upNote, fgNote;
        object fgNoteArg;

        // What runs for what is wanted; a note on the settings page and in the console when they differ (once per
        // change). The bridge shows its frames in a child window: exclusive fullscreen would hide them.
        void ApplyFg()
        {
            if (!ready || Native.Get(Native.Key.InitState) != 2) return;
            if (!FgDirty && Time.realtimeSinceStartup < nextFgCheck) return;
            nextFgCheck = Time.realtimeSinceStartup + 1f;
            FgDirty = false;

            int mode = MsfMod.WantMode, up = MsfMod.WantUpscaler, runs = DlssCamera.Resolve(up, mode);
            string un = null;
            if (mode != DlssCamera.ModeOff && up == DlssCamera.UpscalerDlss && runs == DlssCamera.UpscalerFsr) un = "note.dlss_fsr";
            else if (mode != DlssCamera.ModeOff && runs == 0) un = "note.no_upscaler";

            int kind = MsfMod.WantFgKind, want = MsfMod.FgFramesWanted;
            bool dlssOk = Native.Get(Native.Key.FgAvailable) != 0, fsrOk = Native.Get(Native.Key.FsrFgAvailable) == 1;
            string fn = null;
            object arg = null;
            if (want > 0 && kind == Native.FgKindDlss && !dlssOk)
            {
                if (fsrOk)
                {
                    kind = Native.FgKindFsr;
                    want = 1;
                    fn = "note.fg_dlss_fsr";
                }
                else
                {
                    want = 0;
                    fn = "note.fg_none";
                }
            }
            else if (want > 0 && kind == Native.FgKindFsr && !fsrOk)
            {
                want = 0;
                fn = "note.fg_none";
            }
            if (want > 0 && kind == Native.FgKindDlss)
            {
                int max = Mathf.Max(1, Native.Get(Native.Key.FgMultiFrameMax));
                if (want > max)
                {
                    fn = "note.fg_max";
                    arg = max + 1;
                    want = max;
                }
            }
            if (want > 0 && Screen.fullScreen)
            {
                fn = "note.fg_fullscreen";
                arg = null;
                want = 0;
            }
            if (un != null && un != upNote) Say.Info(un);
            if (fn != null && (fn != fgNote || !Equals(arg, fgNoteArg))) Say.Info(fn, arg);
            upNote = un;
            fgNote = fn;
            fgNoteArg = arg;
            SettingsPage.SetNotes(un, fn, arg);

            if (want == FgApplied && (want == 0 || kind == FgKindApplied)) return;
            FgApplied = want;
            FgKindApplied = want > 0 ? kind : 0;
            Native.FgSetMode(want > 0 ? kind : Native.FgKindDlss, want);
        }

        // What runs, as codes (a language switch changes no code, so it prints nothing). -1: nothing shown yet.
        int upShown = -1, sharpShown = -1, fgShown = -1, reflexShown = -1;
        int sharpPending = -1;
        float sharpSince;

        /// <summary>The console's state lines: all in one line once the plugin is ready, then a line per change. The
        /// sharpness waits until the slider rests (half a second), else a drag prints every step.</summary>
        void Report()
        {
            if (!ready || Native.Get(Native.Key.InitState) != 2) return;
            int runs = DlssCamera.Resolve(MsfMod.WantUpscaler, MsfMod.WantMode);
            int mode = runs != 0 ? MsfMod.WantMode : DlssCamera.ModeOff;
            int up = runs * 10 + mode;
            int sharp = runs != 0 ? MsfMod.WantSharpness : 0;
            int fg = FgKindApplied * 10 + FgApplied;
            bool reflexOk = Native.Get(Native.Key.ReflexAvailable) == 1;
            int reflex = reflexOk ? MsfMod.WantReflex : -2;
            if (upShown < 0)
            {
                string s = UpLine(runs, mode, sharp) + " · " + Loc.F("con.fg", Loc.FrameGeneration(FgKindApplied, FgApplied));
                if (reflexOk) s += " · " + Loc.F("con.reflex", Loc.Reflex(reflex));
                Say.Line(s);
            }
            else
            {
                if (up != upShown) Say.Line(UpLine(runs, mode, sharp));
                else if (sharp != sharpShown)
                {
                    float now = Time.realtimeSinceStartup;
                    if (sharp != sharpPending)
                    {
                        sharpPending = sharp;
                        sharpSince = now;
                    }
                    if (now - sharpSince < 0.5f) sharp = sharpShown;
                    else Say.Info("con.sharpness", Loc.Sharpness(sharp));
                }
                if (fg != fgShown) Say.Info("con.fg", Loc.FrameGeneration(FgKindApplied, FgApplied));
                if (reflex != reflexShown && reflexOk) Say.Info("con.reflex", Loc.Reflex(reflex));
            }
            upShown = up;
            sharpShown = sharp;
            fgShown = fg;
            reflexShown = reflex;
        }

        static string UpLine(int runs, int mode, int sharp) =>
            Loc.F("con.upscaler", Loc.Upscaler(runs, mode)) + (sharp > 0 ? " · " + Loc.F("con.sharpness", Loc.Sharpness(sharp)) : "");

        /// <summary>Reflex wanted changed: applied on the next Update.</summary>
        public bool ReflexDirty = true;
        /// <summary>The last latency report, for the status line.</summary>
        public string ReflexLine = "no report yet";
        readonly float[] reflexReport = new float[8];
        float nextReflexReport;
        int frameMarked = -1;
        int reflexApplied = -1;

        void ApplyReflex()
        {
            if (!ready || Native.Get(Native.Key.InitState) != 2) return;
            if (ReflexDirty && Native.Get(Native.Key.ReflexAvailable) == 1)
            {
                ReflexDirty = false;
                if (MsfMod.WantReflex != reflexApplied)
                {
                    reflexApplied = MsfMod.WantReflex;
                    Native.ReflexSetMode(reflexApplied);
                }
            }
            if (Time.realtimeSinceStartup >= nextReflexReport)
            {
                nextReflexReport = Time.realtimeSinceStartup + 5f;
                ReflexReport(false);
            }
        }

        /// <summary>The driver's latency report (also into the log); print = to the console as well.</summary>
        public void ReflexReport(bool print)
        {
            if (Native.ReflexReport(reflexReport) <= 0) { if (print) Say.Answer("Reflex: no report yet"); return; }
            float[] r = reflexReport;
            ReflexLine = "latency " + r[1].ToString("0.0") + " ms (simulation start -> picture done), GPU busy " +
                         r[2].ToString("0.0") + " of " + r[6].ToString("0.0") + " ms per frame, simulation " + r[4].ToString("0.0") +
                         " ms, submit " + r[5].ToString("0.0") + " ms";
            if (print) Say.Answer("Reflex (" + MsfMod.ReflexNames[MsfMod.WantReflex] + "): " + ReflexLine);
        }

        // Reflex markers: the first camera of the frame ends the simulation and starts the render submission.
        void OnAnyPreCull(Camera c)
        {
            if (frameMarked == Time.frameCount) return;
            frameMarked = Time.frameCount;
            Native.ReflexMarker(Native.MarkerSimulationEnd);
        }

        void OnDestroy()
        {
            Camera.onPreCull -= OnAnyPreCull;
        }

        public IEnumerator Init()
        {
            yield return new WaitForEndOfFrame();
            // Unity calls UnitySetGraphicsDevice when it loads the plugin from Plugins; else take the device from a texture.
            if (Native.Get(Native.Key.DeviceType) < 0 && !Native.DeviceFromTexture(Texture2D.whiteTexture))
            {
                Say.Problem(Loc.T("con.no_device"));
                yield break;
            }
            Native.Issue(Native.EventInit);
            float until = Time.realtimeSinceStartup + 10f;
            while (Native.Get(Native.Key.InitState) != 2 && Time.realtimeSinceStartup < until) yield return null;
            if (Native.Get(Native.Key.RenderEvents) == 0)
                Say.Problem("the render thread did not call the plugin (GL.IssuePluginEvent) - plugin not loaded from Plugins?");
            else if (Native.Get(Native.Key.InitState) != 2)
                Say.Problem("the native plugin did not finish starting in 10 s");
            Say.NativeLines(Native.Drain()); // the start's own lines before the summary
            Say.Detail(MsfMod.Status());
            Say.Info("con.ready", Palette.Brand + " " + Palette.Colorize(Palette.Accent, "v" + MsfMod.ModVersion),
                     SystemInfo.graphicsDeviceName);
            Say.Line(SettingsPage.Capabilities(true));
            SettingsPage.OnNativeReady();
            ready = true;
            Camera.onPreCull += OnAnyPreCull;
            StartCoroutine(EndOfFrame());
        }

        // Frame generation takes the finished picture here: every camera and the UI are drawn, Present is next.
        // Reflex: the frame's render submission ends here; the next frame starts at the game's message pump.
        IEnumerator EndOfFrame()
        {
            WaitForEndOfFrame wait = new WaitForEndOfFrame();
            for (;;)
            {
                yield return wait;
                if (FgApplied > 0) Native.Issue(Native.EventCaptureFg);
                Native.ReflexMarker(Native.MarkerRenderSubmitEnd);
                // Every camera has drawn: the player's camera has its screen target again whatever happened.
                if (DlssCamera.Current != null) DlssCamera.Current.AfterCamera();
            }
        }

        void Awake()
        {
            Spin = gameObject.AddComponent<Spinner>();
            gameObject.AddComponent<FgHud>();
        }

        void Update()
        {
            Say.NativeLines(Native.Drain());

            // The player's camera exists in the game scene only (and is made again on every load).
            if (DlssCamera.Current == null && Application.loadedLevelName == "GAME")
            {
                Camera c = Camera.main;
                if (c != null && c.name == "FPSCamera")
                {
                    DlssCamera.Attach(c);
                    MsfMod.ApplyToCamera();
                }
            }
            ApplyFg();
            ApplyReflex();
            Report();
            Native.FgTick();

            // What the game's scripts see of the camera between frames (upscaling sets a target during rendering only).
            DlssCamera cur = DlssCamera.Current;
            if (cur != null && cur.Running)
            {
                CheckedFrames++;
                if (cur.TargetSet) TargetLeftSet++;
                float a = cur.CenterRayAngle;
                if (a > MaxRayAngle) MaxRayAngle = a;
            }
        }

        public int CheckedFrames, TargetLeftSet;
        public float MaxRayAngle;

        static IEnumerator Frames(int n)
        {
            for (int i = 0; i < n; i++) yield return null;
        }

        static IEnumerator Seconds(float s)
        {
            float until = Time.realtimeSinceStartup + s;
            while (Time.realtimeSinceStartup < until) yield return null;
        }

        /// <summary>The world frozen (time scale 0): screenshots of the same picture with the upscaler off and with the
        /// four jitter directions, two of each a few frames apart (a wrong direction shakes and blurs).</summary>
        public IEnumerator Shots(string label, float yaw)
        {
            DlssCamera d = DlssCamera.Current;
            if (d == null) { Say.Answer("shots: no camera"); yield break; }
            float scale = Time.timeScale;
            int mode = d.Mode;
            int shotMode = mode != DlssCamera.ModeOff ? mode : DlssCamera.ModeDlaa;
            float sx = d.JitterSignX, sy = d.JitterSignY;
            Time.timeScale = 0f;
            Spin.YawPerSecond = yaw;
            Spin.UpPerSecond = 0f;
            string[] names = { "off", "pp", "np", "pn", "nn" };
            float[] xs = { 1f, 1f, -1f, 1f, -1f };
            float[] ys = { 1f, 1f, 1f, -1f, -1f };
            for (int i = 0; i < names.Length; i++)
            {
                d.Mode = i > 0 ? shotMode : DlssCamera.ModeOff;
                d.JitterSignX = xs[i];
                d.JitterSignY = ys[i];
                d.ResetHistory();
                yield return StartCoroutine(Seconds(1.5f));
                Application.CaptureScreenshot(Path.Combine(MsfMod.SettingsDir, "shot_" + label + "_" + names[i] + "_a.png"));
                yield return StartCoroutine(Frames(3));
                Application.CaptureScreenshot(Path.Combine(MsfMod.SettingsDir, "shot_" + label + "_" + names[i] + "_b.png"));
                yield return StartCoroutine(Frames(2));
            }
            Spin.YawPerSecond = 0f;
            d.Mode = mode;
            d.JitterSignX = sx;
            d.JitterSignY = sy;
            Time.timeScale = scale;
            Say.Answer("shots " + label + ": done (" + names.Length + " x 2, " + d.ModeLabel + ")");
        }

        /// <summary>The world frozen, the player turning and moving up and down: the reprojection test for every sign
        /// combination of the motion vectors. The right one has the smallest error with motion.</summary>
        public IEnumerator MvSweep()
        {
            DlssCamera d = DlssCamera.Current;
            if (d == null) { Say.Answer("mvsweep: no camera"); yield break; }
            float scale = Time.timeScale;
            int mode = d.Mode;
            float mx = d.MvSignX, my = d.MvSignY;
            Time.timeScale = 0f;
            if (mode == DlssCamera.ModeOff) d.Mode = DlssCamera.ModeDlaa;
            d.Test = true;
            float[] xs = { 1f, 1f, -1f, -1f };
            float[] ys = { 1f, -1f, 1f, -1f };
            for (int i = 0; i < xs.Length; i++)
            {
                d.MvSignX = xs[i];
                d.MvSignY = ys[i];
                Say.Answer("mvsweep: motion sign " + xs[i] + " " + ys[i]);
                Spin.YawPerSecond = 30f;
                Spin.UpPerSecond = 0.6f;
                yield return StartCoroutine(Seconds(2.5f));
            }
            Spin.YawPerSecond = 0f;
            Spin.UpPerSecond = 0f;
            Say.Answer("mvsweep: still camera");
            d.MvSignX = mx;
            d.MvSignY = my;
            yield return StartCoroutine(Seconds(1.5f));
            d.Test = false;
            d.Mode = mode;
            Time.timeScale = scale;
            Say.Answer("mvsweep: done");
        }
    }

    public sealed class MsfCommand : ConsoleCommand
    {
        public override string Name => "msf";
        public override string Help => "My Summer Frames: msf status | info | on | off | mode off|dlaa|quality|balanced|performance|ultra | fsr off|native|quality|balanced|performance|ultra|debug on/off | sharpness 0..10 | reflex off|on|boost|stats | fg off|2..6|fsr|dump|flip on/off | fgfps on|off | verbose on|off | cameras [label] | probe | jitter sx sy | mvsign sx sy | mvtest on|off | bodies on|off | spin yaw up | freeze on|off | shots label [yaw] | mvsweep";
        public override bool ShowInHelp => true;

        static float F(string[] a, int i, float d)
        {
            float v;
            return a.Length > i && float.TryParse(a[i], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out v) ? v : d;
        }

        static bool On(string[] a, int i) => a.Length <= i || a[i].ToLowerInvariant() != "off";

        // A change of the upscaler, frame generation, Reflex or sharpness answers with the console's state line
        // (Runner.Report), not here.
        public override void Run(string[] args)
        {
            string sub = args.Length > 0 ? args[0].ToLowerInvariant() : "status";
            DlssCamera d = DlssCamera.Current;
            Runner host = MsfMod.Host;
            try
            {
                switch (sub)
                {
                    case "status": Say.Answer(MsfMod.Status()); break;
                    case "info":
                        Say.Answer(SystemInfo.graphicsDeviceName + ": " + (Native.Loaded ? SettingsPage.Capabilities(true) : "native plugin not loaded"));
                        break;
                    case "on":
                    case "off":
                    case "mode":
                    case "fsr":
                        {
                            // dlss on (DLAA) | off | mode off|dlaa|quality|... (DLSS) | fsr off|native|quality|... (FSR) | fsr debug on|off
                            string v = sub == "mode" || sub == "fsr" ? (args.Length > 1 ? args[1].ToLowerInvariant() : "") : sub == "on" ? "dlaa" : "off";
                            if (sub == "fsr" && v == "debug")
                            {
                                Native.FsrSetDebug(On(args, 2));
                                break;
                            }
                            int m = DlssCamera.ModeFromName(v);
                            if (m < 0) { Say.Answer("modes: off, dlaa / native, quality, balanced, performance, ultra"); break; }
                            MsfMod.WantMode = m;
                            if (m != DlssCamera.ModeOff) MsfMod.WantUpscaler = sub == "fsr" ? DlssCamera.UpscalerFsr : DlssCamera.UpscalerDlss;
                            MsfMod.ApplyToCamera();
                            break;
                        }
                    case "sharpness":
                        {
                            int x;
                            if (args.Length > 1 && int.TryParse(args[1], out x) && x >= 0 && x <= 10)
                            {
                                MsfMod.WantSharpness = x;
                                MsfMod.ApplyToCamera();
                                // With an upscaler the state line answers; without one the value waits for it.
                                if (DlssCamera.Resolve(MsfMod.WantUpscaler, MsfMod.WantMode) == 0) Say.Info("con.sharpness", Loc.Sharpness(x));
                            }
                            else Say.Answer("sharpness 0 (off) .. 10 (full), now " + MsfMod.WantSharpness);
                            break;
                        }
                    case "verbose":
                        Say.Verbose = On(args, 1);
                        Say.Info("con.verbose", Loc.OnOff(Say.Verbose));
                        break;
                    case "reflex":
                        {
                            // dlss reflex off | on | boost | stats
                            string v = args.Length > 1 ? args[1].ToLowerInvariant() : "stats";
                            int m = v == "off" ? 0 : v == "on" ? 1 : v == "boost" ? 2 : -1;
                            if (m >= 0)
                            {
                                MsfMod.WantReflex = m;
                                if (host != null) host.ReflexDirty = true;
                            }
                            else if (host != null) host.ReflexReport(true);
                            break;
                        }
                    case "cameras": Say.Answer(Diag.DumpCameras(MsfMod.SettingsDir, args.Length > 1 ? args[1] : "now")); break;
                    case "probe": MsfMod.ProbeMainCamera(); break;
                    case "jitter":
                        if (d == null) break;
                        d.JitterSignX = F(args, 1, 1f);
                        d.JitterSignY = F(args, 2, 1f);
                        d.ResetHistory();
                        Say.Answer("jitter sign " + d.JitterSignX + " " + d.JitterSignY);
                        break;
                    case "mvsign":
                        if (d == null) break;
                        d.MvSignX = F(args, 1, 1f);
                        d.MvSignY = F(args, 2, 1f);
                        Say.Answer("motion sign " + d.MvSignX + " " + d.MvSignY);
                        break;
                    case "mvtest":
                        if (d != null) d.Test = On(args, 1);
                        break;
                    case "fg":
                        {
                            // dlss fg off | 2 | 3 | 4 | 5 | 6 (DLSS, the multiplier) | fsr (FSR 2x) | dump | flip on|off | gen | hudless
                            string v = args.Length > 1 ? args[1].ToLowerInvariant() : "status";
                            if (v == "dump") { Native.FgDump(MsfMod.SettingsDir); Say.Answer("FG: dump requested"); break; }
                            if (v == "flip") { if (d != null) d.FgFlip = On(args, 2); Say.Answer("FG: flip " + (d != null && d.FgFlip)); break; }
                            if (v == "gen") { Native.FgSetGenerate(On(args, 2)); break; }
                            if (v == "hudless") { Native.FgSetHudless(On(args, 2)); break; }
                            int x;
                            if (v == "off") MsfMod.WantFgKind = 0;
                            else if (v == "fsr") MsfMod.WantFgKind = Native.FgKindFsr;
                            else if (int.TryParse(v, out x) && x >= 2 && x <= 6)
                            {
                                MsfMod.WantFgKind = Native.FgKindDlss;
                                MsfMod.WantFg = x - 1;
                            }
                            else { Say.Answer(MsfMod.Status()); break; }
                            MsfMod.ApplyToCamera();
                            break;
                        }
                    case "mipbias":
                        if (d != null) d.MipBiasExtra = F(args, 1, 0f);
                        Say.Answer("extra mip bias " + (d != null ? d.MipBiasExtra : 0f) + " (now " + MipBias.Current.ToString("0.00") +
                                   " on " + MipBias.Textures + " textures)");
                        break;
                    case "fgfps":
                        MsfMod.ShowFgFps = On(args, 1);
                        Say.Answer("FG counter " + (MsfMod.ShowFgFps ? "on" : "off"));
                        break;
                    case "bodies":
                        if (d != null) d.UseBodies = On(args, 1);
                        Say.Answer("moving bodies in the motion vectors " + (d != null && d.UseBodies));
                        break;
                    case "spin":
                        if (host == null) break;
                        host.Spin.YawPerSecond = F(args, 1, 0f);
                        host.Spin.UpPerSecond = F(args, 2, 0f);
                        break;
                    case "freeze": Time.timeScale = args.Length > 1 && args[1] == "off" ? 1f : 0f; break;
                    case "shots": if (host != null) host.StartCoroutine(host.Shots(args.Length > 1 ? args[1] : "now", F(args, 2, 0f))); break;
                    case "mvsweep": if (host != null) host.StartCoroutine(host.MvSweep()); break;
                    default: ModConsole.Print(Help); break;
                }
            }
            catch (Exception e)
            {
                Say.Problem("dlss " + sub + ": " + e.GetType().Name + ": " + e.Message);
            }
        }
    }
}
