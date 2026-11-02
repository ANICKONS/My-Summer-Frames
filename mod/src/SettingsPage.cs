// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// The settings page, in the style of MOPR 5.1: colored Information and Language headers, one section per feature,
// a short grey line under a section, a yellow note only when something needs the player's attention, settings
// that do not apply hidden (the mode list of the other upscaler, the DLSS multiplier with FSR, ...). English and
// Russian.
//
// Language: the page is built (ModSettings) before MSCLoader loads the saved values, so it is built in the
// system's language; ModSettingsLoaded then applies the saved choice. A switch relabels every setting: the texts
// MSCLoader builds the page from (ModSetting.Name, the dropdown items, the slider values: internal fields, set
// through reflection) and, when the page is open, the page is filled again (UniversalView.FillSettings). Guarded:
// if a future MSCLoader renames them, the language still applies after a restart.
//
// MSCLoader members used (1.4.2): ModSetting.Name, ModSetting.SettingsElement, SettingsDropDownList.ArrayOfItems,
// SettingsSliderInt.TextValues, SettingsElement.settingName, UniversalView.mod / FillSettings.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using MSCLoader;
using UnityEngine;
using UnityEngine.UI;

namespace MySummerFrames
{
    static class SettingsPage
    {
        public static SettingsDropDownList Language, Upscaler, DlssMode, FsrMode, FgType, DlssFg, Reflex;
        public static SettingsSliderInt Sharpness;
        public static SettingsCheckBox FgCounter, Verbose;
        static SettingsText info, upNote, fgNote, fsrMultiplier, reflexNote, reflexHint;

        // The notes under the upscaler and the frame generation (keys; null = none).
        static string upNoteKey, fgNoteKey;
        static object fgNoteArg;
        static bool nativeReady;

        /// <summary>Builds the page (inside the ModSettings setup function); onChange runs on every change.</summary>
        public static void Build(Action onChange)
        {
            entries.Clear();
            Loc.Set(Loc.System());

            Header("header.info", Palette.HeaderInfoBackground);
            Text(Title);
            info = Text(Info);

            Header("header.language", Palette.HeaderLanguageBackground);
            Language = Settings.AddDropDownList("language", "", Loc.LanguageNames, (int)Loc.System(), OnLanguage);
            Helper.HideLabel(Language);

            Header("header.upscaling");
            Upscaler = DropDown("upscaler", "set.upscaler", UpscalerItems, DefaultUpscaler(), onChange);
            DlssMode = DropDown("dlssmode", "set.mode", DlssModeItems, 0, onChange);
            FsrMode = DropDown("fsrmode", "set.mode", FsrModeItems, 0, onChange);
            Sharpness = Slider("sharpness", "set.sharpness", 0, 10, 0, onChange, SharpnessValues);
            upNote = Note(() => upNoteKey != null ? Loc.T(upNoteKey) : "");
            Hint("hint.upscaling");

            Header("header.fg");
            FgType = DropDown("fgtype", "set.fg", FgItems, 0, onChange);
            DlssFg = DropDown("dlssfg", "set.multiplier", () => new[] { "2x", "3x", "4x", "5x", "6x" }, 0, onChange);
            fsrMultiplier = Text(() => Loc.T("fg.fsr_multiplier"));
            FgCounter = CheckBox("fgfps", "set.fg_counter", true, onChange);
            fgNote = Note(() => fgNoteKey != null ? Loc.F(fgNoteKey, fgNoteArg) : "");
            Hint("hint.fg");

            Header("header.latency");
            Reflex = DropDown("reflex", "set.reflex", ReflexItems, 1, onChange);
            reflexNote = Note(() => Loc.T("note.reflex_none"));
            reflexHint = Hint("hint.reflex");

            Header("header.diagnostics");
            Verbose = CheckBox("verbose", "set.verbose", false, onChange);
            Hint("hint.verbose");

            Header("header.about", true);
            Text(About);
            UpdateVisibility();
        }

        // ================================================================ texts

        static string Title() =>
            Palette.Brand + " " + Palette.Colorize(Palette.Accent, "v" + MsfMod.ModVersion) + "\n" +
            Loc.F("info.by", Palette.Colorize(Palette.Author, "ANICKON"));

        static string Info()
        {
            string card = Palette.Colorize(Palette.Hint, SystemInfo.graphicsDeviceName);
            if (!nativeReady) return card + "\n" + Palette.Colorize(Palette.Hint, Loc.T("info.checking"));
            return card + "\n\n" + CapabilitiesSettings();
        }

        static string About() =>
            Palette.Colorize(Palette.Hint, "My Summer Frames v" + MsfMod.ModVersion + " · MSCLoader v" + ModLoader.MSCLoader_Ver + "\n" +
                                           Loc.T("about.sdks") + "\n" + Loc.T("about.trademarks"));

        /// <summary>Capabilities for the settings page: structured list with colors.</summary>
        static string CapabilitiesSettings()
        {
            string fsr = First(Native.GetText(Native.Text.FsrUpVersions));
            string fsrFg = First(Native.GetText(Native.Text.FsrFgVersions));
            int fg = Native.Get(Native.Key.FgAvailable) != 0 ? Mathf.Max(1, Native.Get(Native.Key.FgMultiFrameMax)) + 1 : 0;
            bool fsrUp = DlssCamera.FsrAvailable && fsr.Length > 0;
            bool fsrFgOk = Native.Get(Native.Key.FsrFgAvailable) == 1 && fsrFg.Length > 0;

            return Loc.F("info.caps_line", "DLSS", Av(DlssCamera.DlssAvailable)) + "\n" +
                   Loc.F("info.caps_line", "DLSS FG", fg > 0 ? Av(true) + " " + Palette.Colorize(Palette.Muted, Loc.F("info.up_to", fg)) : Av(false)) + "\n" +
                   Loc.F("info.caps_line", "FSR", fsrUp ? Av(true) + " " + Palette.Colorize(Palette.Muted, fsr) : Av(false)) + "\n" +
                   Loc.F("info.caps_line", "FSR FG", Av(fsrFgOk)) + "\n" +
                   Loc.F("info.caps_line", "Reflex", Av(ReflexAvailable));
        }

        /// <summary>Capabilities for the console: structured list.</summary>
        public static string CapabilitiesConsole()
        {
            string fsr = First(Native.GetText(Native.Text.FsrUpVersions));
            string fsrFg = First(Native.GetText(Native.Text.FsrFgVersions));
            int fg = Native.Get(Native.Key.FgAvailable) != 0 ? Mathf.Max(1, Native.Get(Native.Key.FgMultiFrameMax)) + 1 : 0;
            bool fsrUp = DlssCamera.FsrAvailable && fsr.Length > 0;
            bool fsrFgOk = Native.Get(Native.Key.FsrFgAvailable) == 1 && fsrFg.Length > 0;

            return Loc.F("info.caps_line", "DLSS", AvConsole(DlssCamera.DlssAvailable)) + "\n" +
                   Loc.F("info.caps_line", "DLSS FG", fg > 0 ? AvConsole(true) + " " + Palette.Colorize(Palette.Muted, "(" + Loc.F("info.up_to", fg) + ")") : AvConsole(false)) + "\n" +
                   Loc.F("info.caps_line", "FSR", fsrUp ? AvConsole(true) + " " + Palette.Colorize(Palette.Muted, "(" + fsr + ")") : AvConsole(false)) + "\n" +
                   Loc.F("info.caps_line", "FSR FG", AvConsole(fsrFgOk)) + "\n" +
                   Loc.F("info.caps_line", "Reflex", AvConsole(ReflexAvailable));
        }

        static string Av(bool ok) => Palette.Colorize(ok ? Palette.Good : Palette.Muted, Loc.T(ok ? "info.available" : "info.unavailable"));
        static string AvConsole(bool ok) => Palette.Colorize(ok ? Palette.Good : Palette.Muted, Loc.T(ok ? "info.available" : "info.unavailable"));

        static bool ReflexAvailable => Native.Get(Native.Key.ReflexAvailable) == 1;

        static string Value(bool colored, bool good, string text) =>
            colored ? Palette.Colorize(good ? Palette.Good : Palette.Muted, text) : text;

        static string First(string list)
        {
            if (string.IsNullOrEmpty(list)) return "";
            int c = list.IndexOf(',');
            return (c >= 0 ? list.Substring(0, c) : list).Trim();
        }

        static string[] UpscalerItems() => new[] { Loc.T("upscaler.off"), "NVIDIA DLSS", "AMD FSR" };
        static string[] DlssModeItems() => ModeItems(DlssCamera.UpscalerDlss);
        static string[] FsrModeItems() => ModeItems(DlssCamera.UpscalerFsr);
        static string[] FgItems() => new[] { Loc.T("state.off"), "NVIDIA DLSS", "AMD FSR" };
        static string[] ReflexItems() => new[] { Loc.T("state.off"), Loc.T("state.on"), Loc.T("reflex.boost") };

        static string[] ModeItems(int upscaler)
        {
            string[] items = new string[5];
            for (int i = 0; i < 5; i++) items[i] = Loc.Mode(upscaler, DlssCamera.ModeDlaa + i);
            return items;
        }

        static string[] SharpnessValues()
        {
            string[] v = new string[11];
            for (int i = 0; i <= 10; i++) v[i] = Loc.Sharpness(i);
            return v;
        }

        /// <summary>The upscaler chosen when nothing is saved yet: DLSS on a GeForce RTX, FSR on every other card.</summary>
        static int DefaultUpscaler()
        {
            string name = SystemInfo.graphicsDeviceName ?? "";
            bool rtx = SystemInfo.graphicsDeviceVendorID == 0x10DE && name.IndexOf("RTX", StringComparison.OrdinalIgnoreCase) >= 0;
            return rtx ? 1 : 2;
        }

        // ================================================================ state

        /// <summary>Only what applies: the chosen upscaler's modes, the sharpness while an upscaler runs, the DLSS
        /// multiplier or FSR's 2x, the FG counter while frame generation is chosen, Reflex on NVIDIA.</summary>
        public static void UpdateVisibility()
        {
            if (Upscaler == null) return;
            int u = Upscaler.GetSelectedItemIndex(), f = FgType.GetSelectedItemIndex();
            bool reflex = !nativeReady || ReflexAvailable;
            Show(DlssMode, u == 1);
            Show(FsrMode, u == 2);
            Show(Sharpness, u != 0);
            Show(upNote, upNoteKey != null);
            Show(DlssFg, f == 1);
            Show(fsrMultiplier, f == 2);
            Show(FgCounter, f != 0);
            Show(fgNote, fgNoteKey != null);
            Show(Reflex, reflex);
            Show(reflexHint, reflex);
            Show(reflexNote, !reflex);
        }

        /// <summary>The native plugin has started: what this video card runs.</summary>
        public static void OnNativeReady()
        {
            nativeReady = true;
            Relabel(info);
            UpdateVisibility();
        }

        /// <summary>What runs differs from what is chosen (DLSS without RTX, a window wanted, ...): the notes.</summary>
        public static void SetNotes(string upKey, string fgKey, object fgArg)
        {
            if (upKey == upNoteKey && fgKey == fgNoteKey && Equals(fgArg, fgNoteArg)) return;
            upNoteKey = upKey;
            fgNoteKey = fgKey;
            fgNoteArg = fgArg;
            Relabel(upNote);
            Relabel(fgNote);
            UpdateVisibility();
        }

        static void Show(ModSetting s, bool visible)
        {
            if (s == null) return;
            try { s.SetVisibility(visible); }
            catch (Exception e) { Say.Detail("settings page: " + e.Message); }
        }

        // ================================================================ language

        /// <summary>ModSettingsLoaded: the saved (or reset) language.</summary>
        public static void ApplyLanguage()
        {
            if (Language == null) return;
            int i = Language.GetSelectedItemIndex();
            if (Loc.Set(Enum.IsDefined(typeof(Language), i) ? (Language)i : MySummerFrames.Language.English)) Refresh();
        }

        static void OnLanguage()
        {
            ApplyLanguage();
            Helper.RefillLater();
        }

        // ================================================================ builders (each registers its text)

        enum Kind { Label, Text, Header, Items, Values }

        sealed class Entry
        {
            public ModSetting Setting;
            public Kind Kind;
            public Func<string> Label;
            public Func<string[]> Items;
        }

        static readonly List<Entry> entries = new List<Entry>();

        static void Register(ModSetting s, Kind kind, Func<string> label, Func<string[]> items = null)
        {
            if (s != null) entries.Add(new Entry { Setting = s, Kind = kind, Label = label, Items = items });
        }

        static SettingsHeader Header(string key, bool collapsed = false)
        {
            SettingsHeader h = Settings.AddHeader(Loc.T(key), collapsed);
            Register(h, Kind.Header, () => Loc.T(key));
            return h;
        }

        static SettingsHeader Header(string key, Color background)
        {
            SettingsHeader h = Settings.AddHeader(Loc.T(key), background, Palette.HeaderTitle);
            Register(h, Kind.Header, () => Loc.T(key));
            return h;
        }

        static SettingsText Text(Func<string> producer)
        {
            SettingsText t = Settings.AddText(producer());
            Register(t, Kind.Text, producer);
            return t;
        }

        static SettingsText Hint(string key) => Text(() => Palette.Colorize(Palette.Hint, Loc.T(key)));

        static SettingsText Note(Func<string> producer)
        {
            SettingsText t = Text(() => Palette.Colorize(Palette.Note, producer()));
            Show(t, false);
            return t;
        }

        static SettingsDropDownList DropDown(string id, string key, Func<string[]> items, int selected, Action onChange)
        {
            SettingsDropDownList d = Settings.AddDropDownList(id, Loc.T(key), items(), selected, onChange);
            Register(d, Kind.Label, () => Loc.T(key));
            Register(d, Kind.Items, null, items);
            return d;
        }

        static SettingsSliderInt Slider(string id, string key, int min, int max, int value, Action onChange, Func<string[]> values)
        {
            SettingsSliderInt s = Settings.AddSlider(id, Loc.T(key), min, max, value, onChange, values());
            Register(s, Kind.Label, () => Loc.T(key));
            Register(s, Kind.Values, null, values);
            return s;
        }

        static SettingsCheckBox CheckBox(string id, string key, bool value, Action onChange)
        {
            SettingsCheckBox c = Settings.AddCheckBox(id, Loc.T(key), value, onChange);
            Register(c, Kind.Label, () => Loc.T(key));
            return c;
        }

        // ================================================================ relabeling

        const BindingFlags Any = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        static readonly FieldInfo nameField = typeof(ModSetting).GetField("Name", Any);
        static readonly FieldInfo itemsField = typeof(SettingsDropDownList).GetField("ArrayOfItems", Any);
        static readonly FieldInfo valuesField = typeof(SettingsSliderInt).GetField("TextValues", Any);

        static void Relabel(SettingsText t)
        {
            foreach (Entry e in entries)
                if (e.Setting == t && e.Kind == Kind.Text) Apply(e);
        }

        /// <summary>Every registered text in the current language.</summary>
        static void Refresh()
        {
            foreach (Entry e in entries) Apply(e);
        }

        static void Apply(Entry e)
        {
            try
            {
                switch (e.Kind)
                {
                    case Kind.Text: ((SettingsText)e.Setting).SetValue(e.Label()); break;
                    case Kind.Label:
                    case Kind.Header: if (nameField != null) nameField.SetValue(e.Setting, e.Label()); break;
                    case Kind.Items: if (itemsField != null) itemsField.SetValue(e.Setting, e.Items()); break;
                    case Kind.Values: if (valuesField != null) valuesField.SetValue(e.Setting, e.Items()); break;
                }
            }
            catch (Exception ex) { Say.Detail("settings page relabel: " + ex.Message); }
        }

        /// <summary>Layout work MSCLoader's API has no call for: the language dropdown without its empty label row,
        /// the open page filled again after a language switch. MSCLoader builds the elements whenever the page opens.</summary>
        sealed class Helper : MonoBehaviour
        {
            static Helper instance;
            static readonly List<ModSetting> hidden = new List<ModSetting>();
            static readonly FieldInfo elementField = typeof(ModSetting).GetField("SettingsElement", Any);
            bool refill;

            static Helper Instance
            {
                get
                {
                    if (instance == null)
                    {
                        GameObject go = new GameObject("MSF settings page");
                        DontDestroyOnLoad(go);
                        instance = go.AddComponent<Helper>();
                        instance.StartCoroutine(instance.Loop());
                    }
                    return instance;
                }
            }

            public static void HideLabel(ModSetting s)
            {
                if (s == null) return;
                hidden.Add(s);
                Helper unused = Instance;
            }

            public static void RefillLater() => Instance.refill = true;

            IEnumerator Loop()
            {
                for (;;)
                {
                    if (refill)
                    {
                        refill = false;
                        try { Refill(); } catch (Exception e) { Say.Detail("settings page refill: " + e.Message); }
                    }
                    foreach (ModSetting s in hidden)
                    {
                        try { HideLabelNow(s); } catch { }
                    }
                    yield return new WaitForSeconds(0.3f);
                }
            }

            static void HideLabelNow(ModSetting s)
            {
                Component element = elementField != null ? elementField.GetValue(s) as Component : null;
                if (element == null) return;
                FieldInfo f = element.GetType().GetField("settingName", Any);
                Text label = f != null ? f.GetValue(element) as Text : null;
                if (label == null || !label.gameObject.activeSelf) return;
                RectTransform rt = label.GetComponent<RectTransform>();
                if (rt != null) rt.sizeDelta = new Vector2(rt.sizeDelta.x, 0f);
                LayoutElement le = label.GetComponent<LayoutElement>() ?? label.gameObject.AddComponent<LayoutElement>();
                le.minHeight = 0f;
                le.preferredHeight = 0f;
                label.gameObject.SetActive(false);
            }

            // The open page of this mod, built again from the relabeled settings.
            static void Refill()
            {
                Type view = typeof(ModSetting).Assembly.GetType("MSCLoader.UniversalView");
                if (view == null) return;
                FieldInfo modField = view.GetField("mod", Any);
                MethodInfo fill = view.GetMethod("FillSettings", Any);
                if (modField == null || fill == null) return;
                foreach (UnityEngine.Object o in Resources.FindObjectsOfTypeAll(view))
                {
                    MonoBehaviour b = o as MonoBehaviour;
                    if (b == null || !b.gameObject.activeInHierarchy) continue;
                    Mod m = modField.GetValue(b) as Mod;
                    if (m == null || m != MsfMod.Instance) continue;
                    fill.Invoke(b, new object[] { m });
                }
            }
        }
    }
}
