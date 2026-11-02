// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// Every color of the mod in one place: the console and the settings page (the MOPR 5.1 palette). Text colors are
// "#RRGGBB" for Unity rich text.

using UnityEngine;

namespace MySummerFrames
{
    static class Palette
    {
        // ===== Console =====
        public const string TagText = "[MSF]";
        public const string Tag = "#FF7A1A";        // [MSF], bold orange
        public const string Body = "#F0C562";       // a normal line, soft beige
        public const string Accent = "#35E0E0";     // names, versions, values: cyan
        public const string Good = "#3FB950";       // works / on
        public const string Muted = "#9AA6B2";      // off / details, grey
        public const string TitleYellow = "#F5C518"; // "My Summer" yellow
        public const string TitleGreen = "#3FB950";  // "Frames" green

        // ===== Settings page =====
        public const string Nvidia = "#76B900";
        public const string Amd = "#ED1C24";
        public const string Author = "#F5A623";
        public const string Hint = "#9AA6B2";       // the short explanation under a setting
        public const string Note = "#F5C518";       // something the player should read now

        public static readonly Color HeaderTitle = Rgb(0xF5, 0xC5, 0x18);
        public static readonly Color HeaderInfoBackground = Rgb(0x00, 0x00, 0x00);
        public static readonly Color HeaderLanguageBackground = Rgb(0x2E, 0x7D, 0x32);

        static Color Rgb(int r, int g, int b) => new Color(r / 255f, g / 255f, b / 255f);

        public static string Colorize(string hex, string text) => "<color=" + hex + ">" + text + "</color>";
        public static string Bold(string text) => "<b>" + text + "</b>";

        /// <summary>"My Summer Frames", "Frames" in the mod's orange.</summary>
        public static string Brand => Bold("My Summer " + Colorize(Tag, "Frames"));

        /// <summary>"My Summer Frames" for the console: "My Summer" yellow, "Frames" green.</summary>
        public static string ConsoleBrand => Bold(Colorize(TitleYellow, "My Summer ") + Colorize(TitleGreen, "Frames"));
    }
}
