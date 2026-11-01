// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// Diagnostics for the camera integration: every camera of the scene with its components, the quality settings,
// and (probe) what Unity has bound on the render thread at OnPostRender / OnRenderImage of the main camera.

using System;
using System.Collections;
using System.IO;
using System.Text;
using UnityEngine;

namespace MySummerFrames
{
    static class Diag
    {
        public static string Path(Transform t)
        {
            if (t == null) return "";
            StringBuilder sb = new StringBuilder(t.name);
            for (Transform p = t.parent; p != null; p = p.parent) sb.Insert(0, p.name + "/");
            return sb.ToString();
        }

        /// <summary>Writes cameras_[label].txt to the mod's folder; returns a short summary.</summary>
        public static string DumpCameras(string folder, string label)
        {
            StringBuilder sb = new StringBuilder();
            sb.AppendLine("level " + Application.loadedLevelName + ", frame " + Time.frameCount);
            sb.AppendLine("screen " + Screen.width + "x" + Screen.height + " fullscreen=" + Screen.fullScreen +
                          " refresh=" + Screen.currentResolution.refreshRate);
            sb.AppendLine("quality '" + QualitySettings.names[QualitySettings.GetQualityLevel()] + "' aa=" + QualitySettings.antiAliasing +
                          " vsync=" + QualitySettings.vSyncCount + " targetFrameRate=" + Application.targetFrameRate +
                          " shadows=" + QualitySettings.shadowDistance + " lodBias=" + QualitySettings.lodBias +
                          " maxQueuedFrames=" + QualitySettings.maxQueuedFrames);
            sb.AppendLine("gpu " + SystemInfo.graphicsDeviceName + " | " + SystemInfo.graphicsDeviceVersion +
                          " | shader level " + SystemInfo.graphicsShaderLevel + " | RT count " + SystemInfo.supportedRenderTargetCount +
                          " | compute " + SystemInfo.supportsComputeShaders + " | RGHalf " + SystemInfo.SupportsRenderTextureFormat(RenderTextureFormat.RGHalf));
            Camera main = Camera.main;
            sb.AppendLine("Camera.main = " + (main != null ? Path(main.transform) : "null"));
            UnityEngine.Object[] all = Resources.FindObjectsOfTypeAll(typeof(Camera));
            int active = 0;
            foreach (UnityEngine.Object o in all)
            {
                Camera c = o as Camera;
                if (c == null) continue;
                bool on = c.enabled && c.gameObject.activeInHierarchy;
                if (on) active++;
                sb.AppendLine();
                sb.AppendLine((on ? "[ON ] " : "[off] ") + Path(c.transform) + (c == main ? "  (main)" : ""));
                sb.AppendLine("   depth=" + c.depth + " clear=" + c.clearFlags + " mask=0x" + c.cullingMask.ToString("X8") +
                              " near=" + c.nearClipPlane + " far=" + c.farClipPlane + " fov=" + c.fieldOfView +
                              " ortho=" + c.orthographic + " rect=" + c.rect + " pixel=" + c.pixelWidth + "x" + c.pixelHeight);
                sb.AppendLine("   path=" + c.renderingPath + " actual=" + c.actualRenderingPath + " hdr=" + c.hdr +
                              " depthTex=" + c.depthTextureMode + " target=" + (c.targetTexture != null ? c.targetTexture.name + " " +
                              c.targetTexture.width + "x" + c.targetTexture.height + " " + c.targetTexture.format : "screen") +
                              " occlusion=" + c.useOcclusionCulling + " hideFlags=" + c.hideFlags);
                foreach (Component comp in c.GetComponents<Component>())
                {
                    if (comp == null) { sb.AppendLine("   - (missing script)"); continue; }
                    Behaviour b = comp as Behaviour;
                    sb.AppendLine("   - " + comp.GetType().FullName + (b != null ? (b.enabled ? "" : " (disabled)") : ""));
                }
            }
            // GUITexture / GUIText: drawn by a camera's GUILayer (layers in its culling mask), after its image effects.
            sb.AppendLine();
            sb.AppendLine("GUI elements:");
            foreach (UnityEngine.Object o in Resources.FindObjectsOfTypeAll(typeof(GUIElement)))
            {
                GUIElement e = o as GUIElement;
                if (e == null || e.hideFlags != HideFlags.None) continue;
                sb.AppendLine("   " + e.GetType().Name + " " + Path(e.transform) + " layer " + e.gameObject.layer + " (" +
                              LayerMask.LayerToName(e.gameObject.layer) + ")" + (e.enabled ? "" : " disabled") +
                              (e.gameObject.activeInHierarchy ? "" : " inactive"));
            }
            int[] perLayer = new int[32];
            foreach (Renderer r in UnityEngine.Object.FindObjectsOfType<Renderer>()) if (r != null) perLayer[r.gameObject.layer]++;
            sb.AppendLine("Active renderers per layer:");
            for (int i = 0; i < 32; i++)
                if (perLayer[i] > 0) sb.AppendLine("   " + i + " (" + LayerMask.LayerToName(i) + "): " + perLayer[i]);
            string file = System.IO.Path.Combine(folder, "cameras_" + label + ".txt");
            try { File.WriteAllText(file, sb.ToString()); }
            catch (Exception e) { return "cannot write " + file + ": " + e.Message; }
            return all.Length + " cameras (" + active + " on), main " + (main != null ? Path(main.transform) : "null") + " -> " + file;
        }
    }

    /// <summary>One frame on the main camera: what is bound on the render thread at OnPostRender and OnRenderImage.</summary>
    sealed class TargetProbe : MonoBehaviour
    {
        bool posted, imaged;

        void OnPostRender()
        {
            if (posted) return;
            posted = true;
            Native.Issue(Native.EventProbeTargets);
        }

        void OnRenderImage(RenderTexture source, RenderTexture destination)
        {
            if (!imaged)
            {
                imaged = true;
                Native.Issue(Native.EventProbeImage);
                Native.Describe(source, "OnRenderImage source " + (source != null ? source.width + "x" + source.height + " " + source.format + " depth=" + source.depth + " aa=" + source.antiAliasing : "null"));
                Native.Describe(destination, "OnRenderImage destination " + (destination != null ? destination.width + "x" + destination.height + " " + destination.format : "null (screen)"));
            }
            Graphics.Blit(source, destination);
        }

        void LateUpdate()
        {
            // LateUpdate comes before the frame's rendering: remove only after both callbacks ran.
            if (posted && imaged) Destroy(this);
        }
    }

    /// <summary>Test motion for the motion-vector checks: the player turns and moves up and down (world frozen).</summary>
    sealed class Spinner : MonoBehaviour
    {
        public float YawPerSecond, UpPerSecond;
        float t;

        void Update()
        {
            GameObject player = GameObject.Find("PLAYER");
            if (player == null) return;
            float dt = Time.unscaledDeltaTime;
            t += dt;
            player.transform.Rotate(0f, YawPerSecond * dt, 0f, Space.World);
            float dir = (t % 2f) < 1f ? 1f : -1f;
            player.transform.position += Vector3.up * (UpPerSecond * dir * dt);
        }
    }
}
