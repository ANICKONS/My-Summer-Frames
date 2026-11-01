// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// Texture mip bias while DLSS upscales (DLSS programming guide 3.5): textures are sampled for the screen's resolution,
// not the lower one the game is drawn at: bias = log2(render width / screen width) - 1. Unity 5.0 has no global
// bias, so every mip-mapped Texture2D gets it on top of its own (kept, put back when DLSS stops upscaling).
// Textures loaded later (items, a new load) are picked up every 15 s.

using System.Collections.Generic;
using UnityEngine;

namespace MySummerFrames
{
    static class MipBias
    {
        static readonly Dictionary<int, KeyValuePair<Texture2D, float>> original = new Dictionary<int, KeyValuePair<Texture2D, float>>();
        static float applied;
        static float nextScan;

        public static float Current => applied;
        public static int Textures => original.Count;

        /// <summary>Every frame from the camera: the bias wanted now (0 = none).</summary>
        public static void Want(float bias)
        {
            if (Mathf.Abs(bias - applied) > 0.001f)
            {
                applied = bias;
                Apply(true);
                nextScan = Time.realtimeSinceStartup + 15f;
                Native.Log("mip bias " + bias.ToString("0.00") + " on " + original.Count + " textures");
                return;
            }
            if (applied != 0f && Time.realtimeSinceStartup >= nextScan)
            {
                nextScan = Time.realtimeSinceStartup + 15f;
                Apply(false);
            }
        }

        // all = set every known texture again (the bias changed); else only the new ones.
        static void Apply(bool all)
        {
            if (all)
            {
                List<int> gone = null;
                foreach (KeyValuePair<int, KeyValuePair<Texture2D, float>> kv in original)
                {
                    Texture2D t = kv.Value.Key;
                    if (t == null) { (gone ?? (gone = new List<int>())).Add(kv.Key); continue; }
                    t.mipMapBias = kv.Value.Value + applied;
                }
                if (gone != null) foreach (int id in gone) original.Remove(id);
                if (applied == 0f)
                {
                    original.Clear();
                    return;
                }
            }
            foreach (Object o in Resources.FindObjectsOfTypeAll(typeof(Texture2D)))
            {
                Texture2D t = o as Texture2D;
                if (t == null || t.mipmapCount <= 1) continue;
                int id = t.GetInstanceID();
                if (original.ContainsKey(id)) continue;
                original[id] = new KeyValuePair<Texture2D, float>(t, t.mipMapBias);
                t.mipMapBias += applied;
            }
        }
    }
}
