// MSF TestBot - development-only automation for My Summer Car.
// Copyright (C) 2026 ANICKON. MIT license (LICENSE). Contains AI-assisted code.
//
// Inert unless Mods\Config\Mod Settings\MSF_TestBot\run.json exists. The run file is consumed
// (renamed) immediately, so a crashed test can never make the bot start in a normal play session.

using System.IO;
using System.Reflection;
using UnityEngine;
using MSCLoader;

[assembly: AssemblyTitle("MSF TestBot")]
[assembly: AssemblyTrademark("Contains AI-assisted code (Kiro).")]
[assembly: AssemblyVersion("0.1.0.0")]

namespace MSF.TestBot
{
    public class TestBotMod : Mod
    {
        public override string ID => "MSF_TestBot";
        public override string Name => "MSF TestBot (dev only)";
        public override string Author => "ANICKON";
        public override string Version => "0.1.0";
        public override string Description => "Development-only test automation for My Summer Frames. Does nothing without a run file. Contains AI-assisted code.";

        internal static string Folder;

        public override void ModSetup()
        {
            SetupFunction(Setup.OnMenuLoad, OnMenuLoad);
        }

        void OnMenuLoad()
        {
            Folder = ModLoader.GetModSettingsFolder(this);

            // A "reloadgame" step went back to the menu: MSCLoader destroyed the runner and set the mods up again.
            if (BotRunner.HasResume)
            {
                if (BotRunner.Instance != null) Object.Destroy(BotRunner.Instance.gameObject);
                GameObject resumed = new GameObject("MSF_TestBot");
                Object.DontDestroyOnLoad(resumed);
                resumed.AddComponent<BotRunner>().Resume();
                return;
            }

            if (BotRunner.Instance != null) return; // persistent across scene loads

            string run = Path.Combine(Folder, "run.json");
            if (!File.Exists(run)) return;

            string active = Path.Combine(Folder, "run.active.json");
            if (File.Exists(active)) File.Delete(active);
            File.Move(run, active);

            GameObject go = new GameObject("MSF_TestBot");
            Object.DontDestroyOnLoad(go);
            BotRunner runner = go.AddComponent<BotRunner>();
            runner.Begin(active);
        }
    }
}
