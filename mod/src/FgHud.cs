// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// The frame generation counter: one line under the game's own FPS counter (GUI/HUD/FPS), "FG <frames shown per
// second>". It is a copy of the game's counter (same font, size, shadow) with the copy's FSMs off; its value is the
// frames the bridge presents (real + generated) per second, updated once a second like the game's own (which
// averages ten samples a second and counts the game's frames only). Shown while the game's counter is shown, the
// setting is on and the bridge runs; it is drawn by the HUD camera, so it is in the bridge's frames as well.

using UnityEngine;

namespace MySummerFrames
{
    sealed class FgHud : MonoBehaviour
    {
        const string SourcePath = "GUI/HUD/FPS";
        const float LineHeight = 0.4f; // the HUD's lines (Thirst, Hunger, ...) are 0.4 apart

        Transform source;
        Renderer sourceValue;
        GameObject copy;
        TextMesh value, valueShadow;
        float nextFind, nextUpdate, lastTime;
        int lastFrames = -1;

        void Update()
        {
            bool running = MsfMod.ShowFgFps && Native.Loaded && Native.Get(Native.Key.FgStage) == 2;
            if (source == null && running && Time.realtimeSinceStartup >= nextFind)
            {
                nextFind = Time.realtimeSinceStartup + 1f;
                Find();
            }
            bool visible = running && source != null && source.gameObject.activeInHierarchy &&
                           (sourceValue == null || sourceValue.enabled);
            if (visible && copy == null) Build();
            if (copy != null && copy.activeSelf != visible) copy.SetActive(visible);
            if (!visible)
            {
                lastFrames = -1;
                return;
            }

            float now = Time.realtimeSinceStartup;
            int frames = Native.Get(Native.Key.FgReal) + Native.Get(Native.Key.FgGenerated);
            if (lastFrames < 0)
            {
                lastFrames = frames;
                lastTime = now;
                nextUpdate = now + 1f;
                SetText("-");
                return;
            }
            if (now < nextUpdate) return;
            float fps = (frames - lastFrames) / Mathf.Max(0.001f, now - lastTime);
            lastFrames = frames;
            lastTime = now;
            nextUpdate = now + 1f;
            SetText(Mathf.RoundToInt(fps).ToString());
        }

        void Find()
        {
            GameObject gui = GameObject.Find("GUI");
            if (gui == null) return;
            source = gui.transform.Find("HUD/FPS");
            Transform v = source != null ? source.Find("HUDValue") : null;
            sourceValue = v != null ? v.GetComponent<Renderer>() : null;
        }

        void Build()
        {
            copy = (GameObject)Instantiate(source.gameObject);
            copy.name = "FPS (DLSS frame generation)";
            copy.transform.SetParent(source.parent, false);
            copy.transform.localPosition = source.localPosition + Vector3.down * LineHeight;
            copy.transform.localRotation = source.localRotation;
            copy.transform.localScale = source.localScale;
            // The copy's own FSMs would measure the game's FPS again (by type name: no PlayMaker reference needed).
            foreach (Behaviour b in copy.GetComponentsInChildren<Behaviour>(true))
                if (b != null && b.GetType().Name == "PlayMakerFSM") b.enabled = false;
            Transform label = copy.transform.Find("HUDLabel");
            if (label != null)
            {
                SetMesh(label, "FG");
                SetMesh(label.Find("HUDLabelShadow"), "FG");
            }
            Transform v = copy.transform.Find("HUDValue");
            value = v != null ? v.GetComponent<TextMesh>() : null;
            Transform vs = v != null ? v.Find("HUDValueShadow") : null;
            valueShadow = vs != null ? vs.GetComponent<TextMesh>() : null;
            Native.Log("FG counter built under " + SourcePath);
        }

        static void SetMesh(Transform t, string text)
        {
            TextMesh m = t != null ? t.GetComponent<TextMesh>() : null;
            if (m != null) m.text = text;
        }

        void SetText(string text)
        {
            if (value != null) value.text = text;
            if (valueShadow != null) valueShadow.text = text;
        }

        void OnDestroy()
        {
            if (copy != null) Destroy(copy);
        }
    }
}
