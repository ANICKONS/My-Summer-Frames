// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// Puts the native plugin where Unity 5.0 loads plugins from (mysummercar_Data\Plugins). The mod ships it in its
// assets folder; the copy happens before the first call into it, so an update is picked up on the next start.

using System;
using System.IO;
using System.Runtime.InteropServices;
using UnityEngine;

namespace MySummerFrames
{
    static class Installer
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);

        public static string PluginsDir => Path.Combine(Application.dataPath, "Plugins");

        /// <summary>Null when the plugin is in place, else what went wrong.</summary>
        public static string EnsurePlugin(string assetsDir)
        {
            string source = Path.Combine(assetsDir, Native.DllFile);
            string target = Path.Combine(PluginsDir, Native.DllFile);
            try
            {
                if (GetModuleHandleW(Native.DllFile) != IntPtr.Zero) return null; // already loaded this session
                if (!File.Exists(source)) return File.Exists(target) ? null : "missing " + source;
                if (File.Exists(target) && SameBytes(source, target)) return null;
                Directory.CreateDirectory(PluginsDir);
                File.Copy(source, target, true);
                return null;
            }
            catch (Exception e)
            {
                return "cannot copy " + Native.DllFile + " to " + PluginsDir + ": " + e.Message;
            }
        }

        static bool SameBytes(string a, string b)
        {
            byte[] x = File.ReadAllBytes(a), y = File.ReadAllBytes(b);
            if (x.Length != y.Length) return false;
            for (int i = 0; i < x.Length; i++) if (x[i] != y[i]) return false;
            return true;
        }
    }
}
