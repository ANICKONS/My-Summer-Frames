// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// Localization (English, Russian), as in MOPR 5.1: plain dictionaries (Strings.cs); a missing key falls back to
// English, then to the key itself. Technical texts (the detailed log, "msf status") stay English.

using System;
using UnityEngine;

namespace MySummerFrames
{
    enum Language
    {
        English = 0,
        Russian = 1,
    }

    static class Loc
    {
        /// <summary>The names in the language dropdown, in <see cref="Language"/> order, always native.</summary>
        public static readonly string[] LanguageNames = { "English", "Русский" };

        static Language current = Language.English;

        public static Language Current => current;

        public static string T(string key) => TIn(current, key);

        public static string TIn(Language language, string key)
        {
            if (key == null) return "";
            string value;
            if (language == Language.Russian && Strings.Ru.TryGetValue(key, out value)) return value;
            if (Strings.En.TryGetValue(key, out value)) return value;
            return key;
        }

        /// <summary>string.Format over a localized string; a broken translation never throws.</summary>
        public static string F(string key, params object[] args) => FIn(current, key, args);

        public static string FIn(Language language, string key, params object[] args)
        {
            string format = TIn(language, key);
            if (args == null || args.Length == 0) return format;
            try { return string.Format(format, args); }
            catch (FormatException) { return format; }
        }

        /// <summary>Switches the language; true if it changed.</summary>
        public static bool Set(Language language)
        {
            if (!Enum.IsDefined(typeof(Language), language)) language = Language.English;
            if (language == current) return false;
            current = language;
            return true;
        }

        /// <summary>The first start: Russian for Russian, Ukrainian and Belarusian systems, else English.</summary>
        public static Language System()
        {
            try
            {
                switch (Application.systemLanguage)
                {
                    case SystemLanguage.Russian:
                    case SystemLanguage.Ukrainian:
                    case SystemLanguage.Belarusian:
                        return Language.Russian;
                }
            }
            catch { }
            return Language.English;
        }

        // ===== The mod's own words =====

        static readonly string[] ModeKeys = { "mode.off", "mode.dlaa", "mode.quality", "mode.balanced", "mode.performance", "mode.ultra" };

        /// <summary>A mode in the current language ("Quality"; mode 1: "DLAA" or "Native AA").</summary>
        public static string Mode(int upscaler, int mode)
        {
            if (mode == DlssCamera.ModeDlaa) return upscaler == DlssCamera.UpscalerFsr ? T("mode.native") : T("mode.dlaa");
            return T(ModeKeys[Mathf.Clamp(mode, 0, ModeKeys.Length - 1)]);
        }

        /// <summary>What an upscaler state is called: "Off", "DLAA", "DLSS Quality", "FSR Native AA".</summary>
        public static string Upscaler(int upscaler, int mode)
        {
            if (upscaler == 0 || mode == DlssCamera.ModeOff) return T("state.off");
            if (upscaler == DlssCamera.UpscalerDlss && mode == DlssCamera.ModeDlaa) return "DLAA";
            return (upscaler == DlssCamera.UpscalerFsr ? "FSR " : "DLSS ") + Mode(upscaler, mode);
        }

        /// <summary>"Off", "DLSS 3x", "FSR 2x".</summary>
        public static string FrameGeneration(int kind, int generatedFrames)
        {
            if (kind == 0 || generatedFrames <= 0) return T("state.off");
            return (kind == Native.FgKindFsr ? "FSR " : "DLSS ") + (generatedFrames + 1) + "x";
        }

        public static string Sharpness(int tenths) => tenths <= 0 ? T("state.off") : (tenths * 10) + "%";

        public static string OnOff(bool on) => T(on ? "state.on" : "state.off");

        /// <summary>Reflex 0 off, 1 on, 2 on + boost.</summary>
        public static string Reflex(int mode) => mode >= 2 ? T("reflex.boost") : OnOff(mode == 1);
    }
}
