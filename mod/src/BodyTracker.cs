// My Summer Frames
// Copyright (C) 2026 ANICKON
//
// Moving bodies for the motion vectors. Unity 5.0 has no per-object motion vectors, and a second pass over the
// scene would cost the draw calls this game cannot spare. Instead each moving vehicle is a box in its own space:
// a pixel whose world point (from depth) lies in the box moved with the vehicle since the last frame. That covers
// the car the player sits in (the camera moves with it: the cabin must not smear) and passing traffic.
// Candidates: rigid bodies of 100 kg or more (scanned every 10 s) plus the body the camera is inside.

using System.Collections.Generic;
using UnityEngine;

namespace MySummerFrames
{
    sealed class BodyTracker
    {
        sealed class Entry
        {
            public Transform T;
            public Matrix4x4 Prev, Cur;
            public bool HavePrev;
            public int SeenFrame = -1;
            public Vector3 Center, Extents;
            public float Volume = -1f, BoundsTime = -100f;
        }

        const float MinMass = 100f;
        const float Range = 150f;
        const float ScanPeriod = 10f;
        const float BoundsPeriod = 5f;

        readonly Dictionary<int, Entry> entries = new Dictionary<int, Entry>();
        readonly List<Entry> scanned = new List<Entry>();
        readonly List<Entry> picked = new List<Entry>();
        readonly List<KeyValuePair<float, Entry>> order = new List<KeyValuePair<float, Entry>>();
        float nextScan;
        public readonly float[] Data = new float[Native.MaxBodies * Native.BodyFloats];
        public int Count { get; private set; }
        public string LastCarrier { get; private set; }

        Entry Get(Transform t)
        {
            Entry e;
            if (!entries.TryGetValue(t.GetInstanceID(), out e))
            {
                e = new Entry { T = t };
                entries[t.GetInstanceID()] = e;
            }
            return e;
        }

        void Scan()
        {
            scanned.Clear();
            foreach (Object o in Object.FindObjectsOfType(typeof(Rigidbody)))
            {
                Rigidbody rb = o as Rigidbody;
                if (rb != null && rb.mass >= MinMass) scanned.Add(Get(rb.transform));
            }
        }

        /// <summary>The vehicle the camera is in: the nearest heavy rigid body above it (the head pivots of the seats
        /// have light ones of their own).</summary>
        static Transform CarrierOf(Transform cam)
        {
            for (Transform p = cam.parent; p != null; p = p.parent)
            {
                if (p.name == "PLAYER") continue;
                Rigidbody rb = p.GetComponent<Rigidbody>();
                if (rb != null && rb.mass >= MinMass) return p;
            }
            return null;
        }

        /// <summary>Fills Data for this frame (call once per rendered frame, at OnPreCull). camPos = the camera's
        /// position: the matrices are camera-relative like the camera's.</summary>
        public void Update(Transform cam, Vector3 camPos)
        {
            int frame = Time.frameCount;
            if (Time.realtimeSinceStartup >= nextScan)
            {
                nextScan = Time.realtimeSinceStartup + ScanPeriod;
                Scan();
            }
            order.Clear();
            Transform carrier = CarrierOf(cam);
            LastCarrier = carrier != null ? carrier.name : null;
            Entry carrierEntry = carrier != null ? Get(carrier) : null;
            for (int i = scanned.Count - 1; i >= 0; i--)
            {
                Entry e = scanned[i];
                if (e.T == null) { scanned.RemoveAt(i); continue; }
                Step(e, frame);
                if (e == carrierEntry) continue;
                float dist = (e.T.position - camPos).sqrMagnitude;
                if (dist > Range * Range || !e.HavePrev || Same(e.Prev, e.Cur)) continue;
                if (!e.T.gameObject.activeInHierarchy) continue;
                order.Add(new KeyValuePair<float, Entry>(dist, e));
            }
            order.Sort((a, b) => a.Key.CompareTo(b.Key));
            picked.Clear();
            if (carrierEntry != null)
            {
                Step(carrierEntry, frame);
                picked.Add(carrierEntry);
            }
            for (int i = 0; i < order.Count && picked.Count < Native.MaxBodies; i++) picked.Add(order[i].Value);

            Matrix4x4 toWorld = Matrix4x4.TRS(camPos, Quaternion.identity, Vector3.one);
            Matrix4x4 fromWorld = Matrix4x4.TRS(-camPos, Quaternion.identity, Vector3.one);
            int n = 0;
            foreach (Entry e in picked)
            {
                if (Time.realtimeSinceStartup - e.BoundsTime > BoundsPeriod) Bounds(e);
                if (e.Volume <= 0f) continue;
                int at = n * Native.BodyFloats;
                Put(e.T.worldToLocalMatrix * toWorld, at);
                Put(fromWorld * (e.HavePrev ? e.Prev : e.Cur), at + 16);
                Data[at + 32] = e.Center.x; Data[at + 33] = e.Center.y; Data[at + 34] = e.Center.z; Data[at + 35] = e.Volume;
                Data[at + 36] = e.Extents.x; Data[at + 37] = e.Extents.y; Data[at + 38] = e.Extents.z; Data[at + 39] = 0f;
                n++;
            }
            Count = n;
        }

        /// <summary>Current matrix of this frame; the previous one is the last frame's.</summary>
        static void Step(Entry e, int frame)
        {
            if (e.SeenFrame == frame) return;
            Matrix4x4 m = e.T.localToWorldMatrix;
            e.HavePrev = e.SeenFrame == frame - 1;
            e.Prev = e.Cur;
            e.Cur = m;
            e.SeenFrame = frame;
        }

        static bool Same(Matrix4x4 a, Matrix4x4 b)
        {
            for (int i = 0; i < 16; i++) if (Mathf.Abs(a[i] - b[i]) > 1e-5f) return false;
            return true;
        }

        void Put(Matrix4x4 m, int at)
        {
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    Data[at + c * 4 + r] = m[r, c];
        }

        /// <summary>Local box of every visible mesh under the body, 5 cm larger.</summary>
        static void Bounds(Entry e)
        {
            e.BoundsTime = Time.realtimeSinceStartup;
            Matrix4x4 toLocal = e.T.worldToLocalMatrix;
            Vector3 min = new Vector3(float.MaxValue, float.MaxValue, float.MaxValue), max = -min;
            bool any = false;
            foreach (MeshFilter mf in e.T.GetComponentsInChildren<MeshFilter>())
            {
                Mesh mesh = mf.sharedMesh;
                MeshRenderer mr = mf.GetComponent<MeshRenderer>();
                if (mesh == null || mr == null || !mr.enabled) continue;
                Bounds b = mesh.bounds;
                Matrix4x4 m = toLocal * mf.transform.localToWorldMatrix;
                for (int i = 0; i < 8; i++)
                {
                    Vector3 c = b.center + Vector3.Scale(b.extents, new Vector3((i & 1) != 0 ? 1 : -1, (i & 2) != 0 ? 1 : -1, (i & 4) != 0 ? 1 : -1));
                    Vector3 p = m.MultiplyPoint3x4(c);
                    min = Vector3.Min(min, p);
                    max = Vector3.Max(max, p);
                }
                any = true;
            }
            if (!any) { e.Volume = -1f; return; }
            e.Center = (min + max) * 0.5f;
            e.Extents = (max - min) * 0.5f + new Vector3(0.05f, 0.05f, 0.05f);
            e.Volume = 8f * e.Extents.x * e.Extents.y * e.Extents.z;
        }
    }
}
