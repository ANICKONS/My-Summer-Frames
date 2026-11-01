// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// The console. Without the detailed log only the player's business shows: what the mod runs (one line at the
// start, one line per change), notes about it, and problems. The detailed log adds the technical lines (the native
// plugin's progress, the start status). Everything also goes to msf.log (without the colors).

using System.Text.RegularExpressions;
using MSCLoader;

namespace MySummerFrames
{
    static class Say
    {
        /// <summary>The "Detailed log in the console" setting.</summary>
        public static bool Verbose;

        static readonly Regex tags = new Regex("<[^>]+>");

        static string Styled(string text) =>
            Palette.Bold(Palette.Colorize(Palette.Tag, Palette.TagText)) + " " + Palette.Colorize(Palette.Body, text);

        /// <summary>A line for the player, already in the player's language (always shown).</summary>
        public static void Line(string text)
        {
            ModConsole.Print(Styled(text));
            Native.Log(tags.Replace(text, ""));
        }

        /// <summary>A localized line for the player (always shown).</summary>
        public static void Info(string key, params object[] args) => Line(Loc.F(key, args));

        /// <summary>An answer to a console command (always shown, as it is).</summary>
        public static void Answer(string text) => Line(text);

        /// <summary>A technical line: the log always, the console with the detailed log only.</summary>
        public static void Detail(string text)
        {
            Native.Log(text);
            if (Verbose) ModConsole.Print(Palette.Colorize(Palette.Muted, Palette.TagText + " " + text));
        }

        /// <summary>Something that does not work (always shown, as a warning).</summary>
        public static void Problem(string text)
        {
            Native.Log("problem: " + text);
            ModConsole.Warning(Palette.TagText + " " + text);
        }

        /// <summary>The native plugin's console lines: '!' problems (always), '~' details (detailed log only). The
        /// plugin has written them to msf.log already.</summary>
        public static void NativeLines(string lines)
        {
            if (lines == null) return;
            foreach (string l in lines.Split('\n'))
            {
                if (l.Length == 0) continue;
                char c = l[0];
                string text = c == '!' || c == '~' ? l.Substring(1) : l;
                if (c == '!') ModConsole.Warning(Palette.TagText + " " + text);
                else if (Verbose) ModConsole.Print(Palette.Colorize(Palette.Muted, Palette.TagText + " " + text));
            }
        }
    }
}
