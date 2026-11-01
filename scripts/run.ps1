<#
.SYNOPSIS
    One game session with My Summer Frames: installs out\ into the game, runs a TestBot scenario through
    scripts\test-run.ps1 (MOPR is taken out for the session), collects the mod's files, removes the mod again.

.DESCRIPTION
    Installed for the run only: Mods\MySummerFrames.dll, Mods\Assets\MySummerFrames\. The mod itself copies its native core to
    mysummercar_Data\Plugins (that is tested too); the run removes it afterwards, with Mods\Config\Mod Settings\MySummerFrames.
    The mod's log (msf.log, NGX lines, cameras_*.txt) ends up in the run folder under msf\.

    The player's own installation of the mod (any of those four, also under the old name MSCDLSS, with its settings and log) is copied to
    obj\user-install\ before the run, checked file by file, taken out of the game for the run (the run starts
    from the mod's defaults) and put back exactly afterwards, also when the run fails.

.EXAMPLE
    .\scripts\run.ps1 -Scenario tests\scenarios\probe.json -Label probe
    .\scripts\run.ps1 -Scenario tests\scenarios\perf.json -Label perf -Width 2560 -Height 1440 -ExtraArgs -popupwindow
#>
[CmdletBinding()]
param(
    [string]$Scenario = 'tests\scenarios\probe.json',
    [string]$Label = 'run',
    [int]$Width = 1280,
    [int]$Height = 720,
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\My Summer Car',
    [switch]$NoBuild,
    [switch]$DevFeatures,
    [int]$TimeoutMinutes = 25,
    # Count the images the desktop shows from the game window (tools\FrameCounter, DXGI desktop duplication) into
    # frames.csv of the run folder: the only measure of generated frames (the game's own FPS counts real ones).
    [switch]$FrameCounter,
    # Passed to the game (scripts\test-run.ps1 -ExtraArgs), e.g. "-popupwindow" for a borderless 2560x1440 run.
    [string]$ExtraArgs = ''
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
function Step([string]$m, [string]$c = 'Cyan') { Write-Host "[msf-run] $m" -ForegroundColor $c }

$Mods = Join-Path $GameDir 'Mods'
$Plugin = Join-Path $GameDir 'mysummercar_Data\Plugins\msf_core.dll'
$ModDll = Join-Path $Mods 'MySummerFrames.dll'
$ModAssets = Join-Path $Mods 'Assets\MySummerFrames'
$ModCfg = Join-Path $Mods 'Config\Mod Settings\MySummerFrames'
# Name in the stash -> place in the game.
$Installed = [ordered]@{ 'MySummerFrames.dll' = $ModDll; 'Assets' = $ModAssets; 'Config' = $ModCfg; 'msf_core.dll' = $Plugin }
# The same mod under its old name (MSCDLSS, before the rename to My Summer Frames) would run as a second copy:
# stashed and put back the same way.
$Legacy = [ordered]@{ 'MSCDLSS.dll' = (Join-Path $Mods 'MSCDLSS.dll'); 'Assets-MSCDLSS' = (Join-Path $Mods 'Assets\MSCDLSS');
    'Config-MSCDLSS' = (Join-Path $Mods 'Config\Mod Settings\MSCDLSS'); 'mscdlss_ngx.dll' = (Join-Path $GameDir 'mysummercar_Data\Plugins\mscdlss_ngx.dll') }
$Player = [ordered]@{}
foreach ($k in $Installed.Keys) { $Player[$k] = $Installed[$k] }
foreach ($k in $Legacy.Keys) { $Player[$k] = $Legacy[$k] }
$UserStash = Join-Path $RepoRoot 'obj\user-install'

if (Get-Process mysummercar -ErrorAction SilentlyContinue) { Step 'The game is running. Close it first.' 'Red'; exit 1 }
if (Test-Path $UserStash) {
    Step "$UserStash exists: the player's installation of an earlier run was not put back. Restore it by hand first." 'Red'
    exit 1
}

function HashList([string]$root) {
    if (-not (Test-Path $root)) { return @() }
    $item = Get-Item $root -Force
    if (-not $item.PSIsContainer) { return @("|" + (Get-FileHash $root -Algorithm MD5).Hash) }
    @(Get-ChildItem $root -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($root.Length) + '|' + (Get-FileHash $_.FullName -Algorithm MD5).Hash } | Sort-Object)
}

function Same([string]$a, [string]$b) { @(Compare-Object @(HashList $a) @(HashList $b)).Count -eq 0 }

if (-not $NoBuild) {
    $buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $RepoRoot 'build.ps1'), '-GameDir', $GameDir)
    if ($DevFeatures) { $buildArgs += '-DevFeatures' }
    & powershell @buildArgs
    if ($LASTEXITCODE -ne 0) { Step 'Build failed.' 'Red'; exit 1 }
}
$Out = Join-Path $RepoRoot 'out\Mods'
if (-not (Test-Path (Join-Path $Out 'MySummerFrames.dll'))) { Step 'out\ is empty: build first.' 'Red'; exit 1 }

# --- The player's installation out of the way (copied, verified, then removed) ------------------------------
$stashed = @()
foreach ($k in $Player.Keys) {
    $src = $Player[$k]
    if (-not (Test-Path $src)) { continue }
    $dst = Join-Path $UserStash $k
    New-Item -ItemType Directory -Force $UserStash | Out-Null
    if ((Get-Item $src -Force).PSIsContainer) { Copy-Item -Recurse -Force $src $dst } else { Copy-Item -Force $src $dst }
    if (-not (Same $src $dst)) { Step "Copy of the player's $src is incomplete: nothing changed, aborting." 'Red'; exit 1 }
    $stashed += $k
}
foreach ($k in $stashed) { Remove-Item -Recurse -Force $Player[$k] }
if ($stashed.Count) { Step "The player's installation is stashed for the run: $($stashed -join ', ') -> $UserStash" 'Yellow' }

$exit = 0
$started = Get-Date
try {
    Copy-Item (Join-Path $Out 'MySummerFrames.dll') $ModDll -Force
    New-Item -ItemType Directory -Force $ModAssets | Out-Null
    Copy-Item (Join-Path $Out 'Assets\MySummerFrames\*') $ModAssets -Force
    Step "Installed the mod ($((Get-ChildItem $ModAssets).Count) asset files)."

    $counterCsv = Join-Path $env:TEMP 'msf-frames.csv'
    $counter = $null
    if ($FrameCounter) {
        $exe = Join-Path $RepoRoot 'tools\FrameCounter\bin\Release\net8.0\FrameCounter.exe'
        if (-not (Test-Path $exe)) {
            $dotnet = Join-Path $env:USERPROFILE '.dotnet\dotnet.exe'
            if (-not (Test-Path $dotnet)) { $dotnet = 'dotnet' }
            & $dotnet build (Join-Path $RepoRoot 'tools\FrameCounter\FrameCounter.csproj') -c Release -nologo -v q | Out-Null
        }
        if (Test-Path $exe) {
            Remove-Item $counterCsv -Force -ErrorAction SilentlyContinue
            $counter = Start-Process $exe -ArgumentList "`"$counterCsv`" mysummercar 3600" -PassThru -WindowStyle Hidden
            Step "FrameCounter started (pid $($counter.Id))."
        } else { Step "FrameCounter not built: $exe" 'Yellow' }
    }
    $scenarioPath = if ([IO.Path]::IsPathRooted($Scenario)) { $Scenario } else { Join-Path $RepoRoot $Scenario }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'scripts\test-run.ps1') `
        -Scenario $scenarioPath -Label $Label -NoBuild:$NoBuild -Width $Width -Height $Height -GameDir $GameDir `
        -TimeoutMinutes $TimeoutMinutes -ExtraArgs $ExtraArgs
    $exit = $LASTEXITCODE
}
finally {
    $run = Get-ChildItem (Join-Path $RepoRoot '_testruns') -Directory -Filter "*-$Label" -ErrorAction SilentlyContinue |
        Where-Object { $_.CreationTime -ge $started.AddSeconds(-5) } | Sort-Object CreationTime | Select-Object -Last 1
    if ($counter) {
        try { Stop-Process -Id $counter.Id -Force -ErrorAction SilentlyContinue } catch { }
        Start-Sleep -Milliseconds 500
        if ($run -and (Test-Path $counterCsv)) { Copy-Item $counterCsv (Join-Path $run.FullName 'frames.csv') -Force }
    }
    if ($run -and (Test-Path $ModCfg)) {
        $dst = Join-Path $run.FullName 'msf'
        New-Item -ItemType Directory -Force $dst | Out-Null
        Copy-Item -Recurse -Force (Join-Path $ModCfg '*') $dst
        Step "Mod files -> $dst"
    }
    $pluginWasCopied = Test-Path $Plugin
    foreach ($p in $Installed.Values) {
        if (Test-Path $p) {
            try { Remove-Item -Recurse -Force $p } catch { Step "Could not remove $p : $($_.Exception.Message)" 'Red'; $exit = 9 }
        }
    }
    Step "Removed the mod (plugin copied to Plugins by the mod: $pluginWasCopied)."

    # --- The player's installation back, exactly --------------------------------------------------------------
    if ($stashed.Count) {
        $ok = $true
        foreach ($k in $stashed) {
            $src = Join-Path $UserStash $k
            $dst = $Player[$k]
            try {
                New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
                if ((Get-Item $src -Force).PSIsContainer) { Copy-Item -Recurse -Force $src $dst } else { Copy-Item -Force $src $dst }
                if (-not (Same $src $dst)) { throw 'files differ after the copy' }
            } catch { $ok = $false; Step "Could not put back the player's $dst : $($_.Exception.Message)" 'Red' }
        }
        if ($ok) {
            Remove-Item -Recurse -Force $UserStash
            Step "The player's installation is back: $($stashed -join ', ')." 'Green'
        } else {
            $exit = 10
            Step "The player's installation is kept in $UserStash - put it back by hand." 'Red'
        }
    }
    if ($run) { Step "Run folder: $($run.FullName)" 'Green' }
}
exit $exit
