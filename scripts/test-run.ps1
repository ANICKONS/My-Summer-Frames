<#
.SYNOPSIS
    Runs one automated game session with the MSF TestBot and collects its report. Usually called by scripts\run.ps1,
    which installs My Summer Frames for the session first.

.DESCRIPTION
    1. Builds the TestBot against the real game (unless -NoBuild).
    2. Backs up the save folder, the game's screen PlayerPrefs (registry) and every mod's settings.
    3. Installs the TestBot + run.json; takes MOPR (Mods\MOPR.dll) and the -DisableMods out for the session.
    4. Launches the game through Steam in a window, waits for the bot to finish (it quits without saving).
    5. Collects report.json, the bot's files and output_log.txt into _testruns\<timestamp>-<label>.
    6. ALWAYS restores the save folder, the PlayerPrefs, the mods and their settings, even on failure.

.EXAMPLE
    .\scripts\test-run.ps1 -Scenario tests\scenarios\probe.json -Label probe
#>
[CmdletBinding()]
param(
    [string]$Scenario = 'tests\scenarios\probe.json',
    [string]$Label = '',
    [int]$Width = 1280,
    [int]$Height = 720,
    # Extra Unity command line arguments for the game (e.g. "-popupwindow": a borderless window, for runs at the
    # monitor's own resolution).
    [string]$ExtraArgs = '',
    [int]$TimeoutMinutes = 25,
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\My Summer Car',
    [switch]$NoBuild,
    # The game's first scene shows "PRESS ANY KEY TO CONTINUE" before the main menu (and before any mod is loaded).
    # The runner clicks the centre of the game window until the main menu loads. -NoSplashClick disables it.
    [switch]$NoSplashClick,
    [int]$SplashClickIntervalSeconds = 2,
    [int]$SplashTimeoutSeconds = 120,
    # Other mods left out of this run, comma separated DLL names without ".dll" (e.g. "BetterMSC"): moved out of
    # Mods for the run and moved back after it. The settings of every mod are backed up and restored anyway.
    [string]$DisableMods = ''
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)   # scripts -> the repository
function Step([string]$m, [string]$c = 'Cyan') { Write-Host "[test] $m" -ForegroundColor $c }
. (Join-Path $RepoRoot 'scripts\GameWindows.ps1')

$SaveDir    = Join-Path $env:USERPROFILE 'AppData\LocalLow\Amistech\My Summer Car'
$PrefsKey   = 'HKCU\Software\Amistech\My Summer Car'
$ModsDir    = Join-Path $GameDir 'Mods'
$ManagedDir = Join-Path $GameDir 'mysummercar_Data\Managed'
$BotName    = 'MSF.TestBot'
$BotCfgDir  = Join-Path $ModsDir 'Config\Mod Settings\MSF_TestBot'
# Several scenarios separated by commas run in ONE game session (one launch instead of several): their steps are
# joined, with a "reloadgame" (back to the menu without saving, load again) between two of them.
$ScenarioPaths = @($Scenario -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ } |
    ForEach-Object { if ([IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $RepoRoot $_ } })
$ScenarioPath = $ScenarioPaths[0]

# --- Preconditions ------------------------------------------------------------------------------
if (Get-Process mysummercar -ErrorAction SilentlyContinue) { Step 'The game is already running. Close it first.' 'Red'; exit 1 }
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { Step 'Steam is not running.' 'Red'; exit 1 }
if (-not (Test-Path (Join-Path $ManagedDir 'MSCLoader.dll'))) { Step 'MSCLoader is not installed in the game.' 'Red'; exit 1 }
foreach ($p in $ScenarioPaths) { if (-not (Test-Path $p)) { Step "Scenario not found: $p" 'Red'; exit 1 } }
$SteamExe = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamExe
if (-not $SteamExe) { $SteamExe = "${env:ProgramFiles(x86)}\Steam\steam.exe" }

# --- Build --------------------------------------------------------------------------------------
$Dotnet = Join-Path $env:USERPROFILE '.dotnet\dotnet.exe'
if (-not (Test-Path $Dotnet)) { $Dotnet = 'dotnet' }
$BotProject = Join-Path $RepoRoot "tests\TestBot\$BotName.csproj"
$BotDll = Join-Path $RepoRoot "tests\TestBot\bin\Release\$BotName.dll"
if (-not $NoBuild -or -not (Test-Path $BotDll)) {
    Step 'Building TestBot...'
    & $Dotnet build $BotProject -c Release -nologo -v q "-p:ManagedDir=$ManagedDir" | Out-Null
    if ($LASTEXITCODE -ne 0) { Step 'TestBot build failed.' 'Red'; exit 1 }
}
if (-not (Test-Path $BotDll)) { Step "TestBot not built: $BotDll" 'Red'; exit 1 }
$Disabled = @($DisableMods -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($d in $Disabled) {
    if ($d -match '[\\/:*?"<>|]' -or $d -ieq 'MOPR' -or $d -ieq 'MySummerFrames' -or $d -ieq $BotName) { Step "DisableMods: invalid mod name '$d'" 'Red'; exit 1 }
    if (-not (Test-Path (Join-Path $ModsDir "$d.dll"))) { Step "DisableMods: $d.dll is not in $ModsDir" 'Red'; exit 1 }
}

# --- Run folder + backups -------------------------------------------------------------------------
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$name   = if ($Label) { "$stamp-$Label" } else { "$stamp-run" }
$RunDir = Join-Path $RepoRoot "_testruns\$name"
$Stash  = Join-Path $RunDir 'stash'
New-Item -ItemType Directory -Force -Path $RunDir, $Stash | Out-Null

Step "Backing up the save folder -> $RunDir\save-backup"
Copy-Item -Recurse -Force $SaveDir (Join-Path $RunDir 'save-backup')
$srcCount = (Get-ChildItem $SaveDir -Recurse -File).Count
$bakCount = (Get-ChildItem (Join-Path $RunDir 'save-backup') -Recurse -File).Count
if ($srcCount -ne $bakCount) { Step "Save backup incomplete ($bakCount of $srcCount files). Aborting." 'Red'; exit 1 }
& reg.exe export $PrefsKey (Join-Path $RunDir 'prefs.reg') /y | Out-Null
if ($LASTEXITCODE -ne 0) { Step 'Could not back up the game PlayerPrefs. Aborting.' 'Red'; exit 1 }

# MOPR (the player's optimization mod) changes what the game draws and how fast: the sessions run without it.
$userMopr = Join-Path $ModsDir 'MOPR.dll'
$hadUserMopr = Test-Path $userMopr
if ($hadUserMopr) { Copy-Item $userMopr (Join-Path $Stash 'MOPR.dll') -Force }

# The settings of every mod (the bot may change them, e.g. "modsetting" in the menu): backed up, restored after.
$ModSettingsDir = Join-Path $ModsDir 'Config\Mod Settings'
$CfgBackup = Join-Path $RunDir 'mod-settings-backup'
$cfgDirs = @(Get-ChildItem $ModSettingsDir -Directory -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'MSF_TestBot' })
New-Item -ItemType Directory -Force -Path $CfgBackup | Out-Null
foreach ($dir in $cfgDirs) { Copy-Item -Recurse -Force $dir.FullName (Join-Path $CfgBackup $dir.Name) }
$cfgCount = @($cfgDirs | ForEach-Object { Get-ChildItem $_.FullName -Recurse -File -Force }).Count
$cfgBak = @(Get-ChildItem $CfgBackup -Recurse -File -Force).Count
if ($cfgCount -ne $cfgBak) { Step "Mod settings backup incomplete ($cfgBak of $cfgCount files). Aborting." 'Red'; exit 1 }
$DisabledStash = Join-Path $Stash 'disabled-mods'

$exitCode = 0
try {
    # --- Install the bot, take the others out -----------------------------------------------------
    if ($hadUserMopr) { Remove-Item $userMopr -Force; Step 'MOPR is out for this run.' }
    Copy-Item $BotDll (Join-Path $ModsDir "$BotName.dll") -Force
    if ($Disabled.Count -gt 0) {
        New-Item -ItemType Directory -Force -Path $DisabledStash | Out-Null
        foreach ($d in $Disabled) { Move-Item (Join-Path $ModsDir "$d.dll") (Join-Path $DisabledStash "$d.dll") -Force }
        Step "Left out for this run: $($Disabled -join ', ')"
    }

    if (Test-Path $BotCfgDir) { Remove-Item -Recurse -Force $BotCfgDir }
    New-Item -ItemType Directory -Force -Path $BotCfgDir | Out-Null
    $scenarioJson = Get-Content $ScenarioPath -Raw | ConvertFrom-Json
    if ($ScenarioPaths.Count -gt 1) {
        # One session: the settings of the first scenario, every scenario's steps without its "quit", one quit at the end.
        $steps = New-Object System.Collections.ArrayList
        for ($s = 0; $s -lt $ScenarioPaths.Count; $s++) {
            $part = Get-Content $ScenarioPaths[$s] -Raw | ConvertFrom-Json
            if ($s -gt 0) { [void]$steps.Add([pscustomobject]@{ Op = 'reloadgame' }) }
            [void]$steps.Add([pscustomobject]@{ Op = 'log'; Text = "===== scenario $($part.Name) ($($s + 1)/$($ScenarioPaths.Count))" })
            foreach ($st in @($part.Steps)) { if ($st.Op -ne 'quit') { [void]$steps.Add($st) } }
        }
        [void]$steps.Add([pscustomobject]@{ Op = 'quit' })
        $scenarioJson.Steps = $steps.ToArray()
        $scenarioJson.Name = (($ScenarioPaths | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) }) -join '+')
        Step "One session for $($ScenarioPaths.Count) scenarios: $($scenarioJson.Name)"
    }
    $scenarioJson | Add-Member -NotePropertyName Variant -NotePropertyValue $name -Force
    $scenarioJson | ConvertTo-Json -Depth 10 | Set-Content -Encoding UTF8 (Join-Path $BotCfgDir 'run.json')

    # --- Launch -----------------------------------------------------------------------------------
    Step "Launching the game ($Width x $Height, windowed) - please don't touch mouse/keyboard in the game window."
    $launchTime = Get-Date
    Start-Process $SteamExe -ArgumentList ("-applaunch 516750 -screen-width $Width -screen-height $Height -screen-fullscreen 0 " + $ExtraArgs).TrimEnd()

    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $startDeadline = (Get-Date).AddMinutes(2)
    $proc = $null
    while (-not $proc -and (Get-Date) -lt $startDeadline) {
        Start-Sleep -Seconds 2
        $proc = Get-Process mysummercar -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if (-not $proc) { throw 'The game did not start within 2 minutes.' }
    Step "Game started (pid $($proc.Id)). Waiting for the bot..."

    $doneFlag  = Join-Path $BotCfgDir 'done.flag'
    $menuFlag  = Join-Path $BotCfgDir 'menu.flag'
    $menuReady = Join-Path $BotCfgDir 'menu.ready'
    $gameLog   = Join-Path $GameDir 'output_log.txt'
    $handledDialogs = @{}
    $menuHandled = $false
    $splashDone = [bool]$NoSplashClick
    $splashClicks = 0
    $lastSplashClick = [datetime]::MinValue
    $splashDeadline = (Get-Date).AddSeconds($SplashTimeoutSeconds)
    $shot = 0
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $doneFlag) { break }
        if ($proc.HasExited) { break }

        # 1. Startup dialogs of the game process (Unity's configuration window): press OK.
        foreach ($w in @(Get-GameWindows $proc.Id | Where-Object { $_.Visible -and $_.Class -eq '#32770' })) {
            $key = $w.Handle.ToInt64()
            if ($handledDialogs.ContainsKey($key)) { continue }
            $handledDialogs[$key] = $true
            $shot++
            [void](Save-Screenshot (Join-Path $RunDir ("dialog-$shot.png")))
            $controls = @(Get-DialogControls $w.Handle | Where-Object { $_.Visible -and $_.Text } | ForEach-Object { "$($_.Class)#$($_.Id) '$($_.Text)'" })
            Step "Dialog '$($w.Title)' ($($w.Width)x$($w.Height)): $($controls -join ', ')"
            $pressed = Invoke-DialogConfirm $w.Handle
            if ($pressed) { Step "  pressed '$($pressed.Text)' (id $($pressed.Id))" 'Green' } else { Step '  no OK button found - please press it manually' 'Yellow' }
        }

        # 2. "PRESS ANY KEY TO CONTINUE" splash (first scene, before the main menu and before mods load):
        #    click the centre of the game window until the main menu is loaded. The menu counts as loaded when the
        #    fresh game log says "LoadLevel: MainMenu" or the bot (a mod, so already in the menu) wrote menu.flag.
        if (-not $splashDone) {
            if ((Test-Path $menuFlag) -or (Test-LogContains $gameLog 'LoadLevel: MainMenu' $launchTime)) {
                $splashDone = $true
                Step "Splash screen passed ($splashClicks click(s)), main menu loaded." 'Green'
            } elseif ((Get-Date) -gt $splashDeadline) {
                $splashDone = $true
                Step 'Splash screen: main menu did not load in time - please press a key in the game window.' 'Yellow'
            } elseif (((Get-Date) - $lastSplashClick).TotalSeconds -ge $SplashClickIntervalSeconds) {
                $unity = Get-GameWindows $proc.Id | Where-Object { $_.Visible -and $_.Class -eq 'UnityWndClass' -and $_.Width -gt 200 } | Select-Object -First 1
                $dialogOpen = @(Get-GameWindows $proc.Id | Where-Object { $_.Visible -and $_.Class -eq '#32770' }).Count -gt 0
                if ($unity -and -not $dialogOpen) {
                    $lastSplashClick = Get-Date
                    $rc = New-Object GameWin32+RECT
                    [void][GameWin32]::GetClientRect($unity.Handle, [ref]$rc)
                    $cx = [int](($rc.Right - $rc.Left) / 2); $cy = [int](($rc.Bottom - $rc.Top) / 2)
                    if ($splashClicks -eq 0) { [void](Save-Screenshot (Join-Path $RunDir 'splash.png')) }
                    if ([GameWin32]::ClickClientIfOnTop($unity.Handle, $cx, $cy)) {
                        $splashClicks++
                        Step "Splash screen: clicked the game window (#$splashClicks)."
                    } else {
                        Step 'Splash screen: the game window is covered or minimized, not clicking (bring it to front).' 'Yellow'
                    }
                }
            }
        }

        # 3. Main menu is up (written by the bot): release the bot.
        if ($splashDone -and -not $menuHandled -and (Test-Path $menuFlag)) {
            $menuHandled = $true
            Start-Sleep -Milliseconds 800
            [void](Save-Screenshot (Join-Path $RunDir 'menu.png'))
            Set-Content -Path $menuReady -Value 'ok'
        }
        Start-Sleep -Seconds 1
    }

    if (Test-Path $doneFlag) {
        Step "Bot finished: $(Get-Content $doneFlag -Raw)" 'Green'
        if (-not $proc.WaitForExit(60000)) { Step 'Game did not quit, killing it.' 'Yellow'; Stop-Process -Id $proc.Id -Force }
    } elseif ($proc.HasExited) {
        Step "The game exited before the bot finished (exit code $($proc.ExitCode))." 'Red'; $exitCode = 2
    } else {
        Step 'Timeout - killing the game.' 'Red'; Stop-Process -Id $proc.Id -Force; $exitCode = 3
    }
    Start-Sleep -Seconds 3
}
catch {
    Step "Run failed: $($_.Exception.Message)" 'Red'
    $exitCode = 4
    Get-Process mysummercar -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 3
}
finally {
    # --- Collect ----------------------------------------------------------------------------------
    foreach ($f in 'report.json', 'run.active.json') {
        $p = Join-Path $BotCfgDir $f
        if (Test-Path $p) { Copy-Item $p $RunDir -Force }
    }
    foreach ($pattern in 'dumpui_*.txt', 'find_*.txt', 'dumpfsm_*.txt', 'probe_*.txt', 'settings_*.txt', 'fsmhist_*.txt', 'tree_*.txt',
                         'fsmcache_*.txt', 'partdiff_*.txt', 'fsmmark_*.txt', 'lightinfo_*.txt', 'visible_*.txt', 'fsmgrep_*.txt',
                         'snapshot_*.tsv', 'watch_*.tsv', 'shot_*.png', 'partsnap_*.tsv', 'fullsnap_*.tsv',
                         'proptrace_*.txt', 'writers_*.txt', 'inventory_*.jsonl', 'autodrive_*.tsv') {
        Get-ChildItem $BotCfgDir -Filter $pattern -ErrorAction SilentlyContinue | Copy-Item -Destination $RunDir -Force
    }
    $p = Join-Path $GameDir 'output_log.txt'
    if (Test-Path $p) { Copy-Item $p $RunDir -Force }

    # --- Restore ----------------------------------------------------------------------------------
    # Every restore step runs independently: one failure must never skip the others.
    Step 'Restoring the save folder, PlayerPrefs and Mods folder...'
    $restoreSteps = @(
        @{ Name = 'save';  Do = {
            Get-ChildItem $SaveDir -Force | Remove-Item -Recurse -Force
            Copy-Item -Recurse -Force (Join-Path (Join-Path $RunDir 'save-backup') '*') $SaveDir } },
        # reg.exe prints its success message to stderr; run it through cmd so PowerShell does not treat it as an error.
        @{ Name = 'prefs'; Do = {
            & cmd.exe /c "reg import `"$(Join-Path $RunDir 'prefs.reg')`" >nul 2>&1"
            if ($LASTEXITCODE -ne 0) { throw "reg import exit code $LASTEXITCODE" } } },
        @{ Name = 'bot';   Do = {
            Remove-Item (Join-Path $ModsDir "$BotName.dll") -Force -ErrorAction SilentlyContinue
            if (Test-Path $BotCfgDir) { Remove-Item -Recurse -Force $BotCfgDir } } },
        @{ Name = 'mopr';  Do = {
            if ($hadUserMopr) { Copy-Item (Join-Path $Stash 'MOPR.dll') $userMopr -Force } } },
        @{ Name = 'disabled-mods'; Do = {
            foreach ($d in $Disabled) {
                $s = Join-Path $DisabledStash "$d.dll"
                if (Test-Path $s) { Move-Item $s (Join-Path $ModsDir "$d.dll") -Force }
                if (-not (Test-Path (Join-Path $ModsDir "$d.dll"))) { throw "$d.dll is not back in Mods" }
            } } },
        @{ Name = 'mod-settings'; Do = {
            foreach ($dir in @(Get-ChildItem $CfgBackup -Directory -Force)) {
                $dst = Join-Path $ModSettingsDir $dir.Name
                if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
                Copy-Item -Recurse -Force $dir.FullName $dst
            } } }
    )
    foreach ($s in $restoreSteps) {
        try { & $s.Do } catch { Step "Restore step '$($s.Name)' FAILED: $($_.Exception.Message)" 'Red'; $exitCode = 5 }
    }

    # Verify the save byte-for-byte against the backup.
    $hashList = { param($root) Get-ChildItem $root -Recurse -File | ForEach-Object { $_.FullName.Substring($root.Length) + '|' + (Get-FileHash $_.FullName -Algorithm MD5).Hash } | Sort-Object }
    $now = & $hashList $SaveDir
    $bak = & $hashList (Join-Path $RunDir 'save-backup')
    $diff = @(Compare-Object @($now) @($bak)).Count
    if ($diff -eq 0) { Step "Save verified: $(@($now).Count) files identical to the backup. Run folder: $RunDir" 'Green' }
    else { Step "SAVE MISMATCH ($diff differences)! Backup is in $RunDir\save-backup" 'Red'; $exitCode = 6 }

    $nowCfg = @(Get-ChildItem $CfgBackup -Directory -Force | ForEach-Object { & $hashList (Join-Path $ModSettingsDir $_.Name) | ForEach-Object { $_ } })
    $bakCfg = @(Get-ChildItem $CfgBackup -Directory -Force | ForEach-Object { & $hashList $_.FullName | ForEach-Object { $_ } })
    $cfgDiff = @(Compare-Object $nowCfg $bakCfg).Count
    if ($cfgDiff -eq 0) { Step "Mod settings verified: $($nowCfg.Count) files identical to the backup." 'Green' }
    else { Step "MOD SETTINGS MISMATCH ($cfgDiff differences)! Backup is in $CfgBackup" 'Red'; $exitCode = 8 }
    if ($hadUserMopr -and -not (Test-Path $userMopr)) { Step "MOPR.dll is not back in Mods! A copy is in $Stash" 'Red'; $exitCode = 7 }
}

# --- Summary --------------------------------------------------------------------------------------
$reportPath = Join-Path $RunDir 'report.json'
if (Test-Path $reportPath) {
    $r = Get-Content $reportPath -Raw | ConvertFrom-Json
    Step "Report: load=$([math]::Round($r.LoadSeconds,1))s error=$($r.Error)"
    if ($r.InputFrames -gt 0 -and $r.InputLocked) { Step "  real input in $($r.InputFrames) frame(s) ($($r.InputFirst)), kept from the game by the input lock" }
    elseif ($r.InputFrames -gt 0) { Step "  USER INPUT reached the game in $($r.InputFrames) frame(s), first: $($r.InputFirst) - not valid for a comparison" 'Yellow' }
    foreach ($m in $r.Measures) { Step ("  measure {0,-14} {1,6:0.0} fps  1%low {2,6:0.0}  p99 {3,6:0.0} ms  GC {4}" -f $m.Label, $m.AvgFps, $m.Low1PercentFps, $m.FrameMsP99, $m.GcCollections) }
    foreach ($p in $r.Profiles) { Step ("  profile {0,-14} frame {1:0.00} ms  scripts {2:0.00} ms  PlayMaker {3:0.00} ms" -f $p.Label, $p.FrameMsAvg, $p.ScriptMsPerFrame, $p.PlayMakerMsPerFrame) }
}
# Written last: the run folder is complete (collected and restored) - for scripts that wait for a run.
Set-Content -Path (Join-Path $RunDir 'run.done') -Value $exitCode
exit $exitCode
