<#
.SYNOPSIS
    Builds My Summer Frames: the native core (MSVC) and the MSCLoader mod (dotnet), staged in out\ like the game folder.

.DESCRIPTION
    out\Mods\MySummerFrames.dll                       the mod
    out\Mods\Assets\MySummerFrames\msf_core.dll       the native core (the mod copies it to mysummercar_Data\Plugins)
    out\Mods\Assets\MySummerFrames\nvngx_dlss*.dll   NVIDIA DLSS feature libraries from the SDK (rel, or dev with -DevFeatures)
    out\Mods\Assets\MySummerFrames\amd_fidelityfx_*_dx12.dll, AMD_FSR_LICENSE.txt   AMD FSR SDK (signed DLLs) and its notice

    The NVIDIA DLSS SDK, NVAPI and the AMD FSR SDK are fetched into _sdk\ on the first build (sparse clones).

.EXAMPLE
    .\build.ps1
    .\build.ps1 -DevFeatures      # NGX development DLLs: on-screen indicator and debug overlays
#>
[CmdletBinding()]
param(
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\My Summer Car',
    [switch]$DevFeatures,
    [switch]$NativeOnly,
    [switch]$ManagedOnly
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
function Step([string]$m, [string]$c = 'Cyan') { Write-Host "[msf] $m" -ForegroundColor $c }
# git writes its progress to stderr: a terminating error in Windows PowerShell under ErrorActionPreference Stop.
function GitQuiet { $ErrorActionPreference = 'Continue'; & git.exe @args 2>&1 | Out-Null }

$Sdk = Join-Path $Root '_sdk\DLSS'
if (-not (Test-Path (Join-Path $Sdk 'include\nvsdk_ngx.h'))) {
    Step 'Fetching the NVIDIA DLSS SDK...'
    New-Item -ItemType Directory -Force (Join-Path $Root '_sdk') | Out-Null
    Push-Location (Join-Path $Root '_sdk')
    try {
        GitQuiet clone --depth 1 --filter=blob:none --sparse https://github.com/NVIDIA/DLSS.git
        Set-Location DLSS
        GitQuiet sparse-checkout set include lib/Windows_x86_64 doc
    } finally { Pop-Location }
    if (-not (Test-Path (Join-Path $Sdk 'include\nvsdk_ngx.h'))) { Step 'DLSS SDK fetch failed.' 'Red'; exit 1 }
}

# NVAPI (Reflex), MIT license: github.com/NVIDIA/nvapi
$Nvapi = Join-Path $Root '_sdk\nvapi'
if (-not (Test-Path (Join-Path $Nvapi 'nvapi.h'))) {
    Step 'Fetching NVAPI...'
    Push-Location (Join-Path $Root '_sdk')
    try { GitQuiet clone --depth 1 https://github.com/NVIDIA/nvapi.git } finally { Pop-Location }
    if (-not (Test-Path (Join-Path $Nvapi 'nvapi.h'))) { Step 'NVAPI fetch failed.' 'Red'; exit 1 }
}

# AMD FSR SDK (signed DLLs + headers, MIT for these files): github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
$FfxTag = 'v2.4.0'
$Ffx = Join-Path $Root '_sdk\FidelityFX-SDK'
$FfxKit = Join-Path $Ffx 'Kits\FidelityFX'
if (-not (Test-Path (Join-Path $FfxKit 'signedbin\amd_fidelityfx_loader_dx12.dll'))) {
    Step "Fetching the AMD FSR SDK $FfxTag..."
    Push-Location (Join-Path $Root '_sdk')
    try {
        GitQuiet clone --depth 1 --branch $FfxTag --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
        Set-Location FidelityFX-SDK
        GitQuiet sparse-checkout set --no-cone '/Kits/FidelityFX/api/' '/Kits/FidelityFX/framegeneration/' '/Kits/FidelityFX/upscalers/' `
            '/Kits/FidelityFX/docs/' '/Kits/FidelityFX/signedbin/*dx12*' '/Kits/FidelityFX/readme.md' '/readme.md'
    } finally { Pop-Location }
    if (-not (Test-Path (Join-Path $FfxKit 'signedbin\amd_fidelityfx_loader_dx12.dll'))) { Step 'FSR SDK fetch failed.' 'Red'; exit 1 }
}

$Out = Join-Path $Root 'out'
$Assets = Join-Path $Out 'Mods\Assets\MySummerFrames'
$Obj = Join-Path $Root 'obj\core'
New-Item -ItemType Directory -Force $Assets, $Obj | Out-Null

if (-not $ManagedOnly) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { $vs = & $vswhere -latest -products * -property installationPath }
    # An installation the installer marks incomplete (cl.exe present all the same) shows with -all only.
    if (-not $vs) { $vs = & $vswhere -all -latest -products * -property installationPath }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { Step "vcvars64.bat not found under '$vs'." 'Red'; exit 1 }

    $sources = (Get-ChildItem (Join-Path $Root 'core\src\*.cpp') | ForEach-Object { "`"$($_.FullName)`"" }) -join ' '
    $lib = Join-Path $Sdk 'lib\Windows_x86_64\x64'
    $dll = Join-Path $Obj 'msf_core.dll'
    $bat = Join-Path $Obj 'build.bat'
    $hlsl = Join-Path $Root 'core\shaders\motion.hlsl'
    @(
        '@echo off',
        "call `"$vcvars`" >nul || exit /b 1",
        "fxc /nologo /T cs_5_0 /O3 /E MotionCS /Vn g_MotionCS /Fh `"$Obj\motion_cs.h`" `"$hlsl`" >nul || exit /b 1",
        "fxc /nologo /T cs_5_0 /O3 /E ErrorCS /Vn g_ErrorCS /Fh `"$Obj\error_cs.h`" `"$hlsl`" >nul || exit /b 1",
        "fxc /nologo /T cs_5_0 /O3 /E FlipCS /Vn g_FlipCS /Fh `"$Obj\flip_cs.h`" `"$hlsl`" >nul || exit /b 1",
        "fxc /nologo /T cs_5_0 /O3 /E HudlessCS /Vn g_HudlessCS /Fh `"$Obj\hudless_cs.h`" `"$hlsl`" >nul || exit /b 1",
        "fxc /nologo /T cs_5_0 /O3 /E SharpenCS /Vn g_SharpenCS /Fh `"$Obj\sharpen_cs.h`" `"$hlsl`" >nul || exit /b 1",
        "cl /nologo /I`"$Obj`" /std:c++20 /O2 /MT /EHsc /W4 /Zi /utf-8 /DUNICODE /D_UNICODE /I`"$(Join-Path $Sdk 'include')`" /I`"$Nvapi`" /I`"$FfxKit`" /Fo`"$Obj\\`" /Fd`"$Obj\\`" $sources /LD /Fe`"$dll`" /link /DEBUG /OPT:REF /OPT:ICF /LIBPATH:`"$lib`" /LIBPATH:`"$(Join-Path $Nvapi 'amd64')`" nvsdk_ngx_s.lib nvapi64.lib d3d11.lib d3d12.lib dxgi.lib user32.lib advapi32.lib dbghelp.lib || exit /b 1"
    ) | Set-Content -Encoding ASCII $bat
    Step 'Building the native core...'
    & cmd.exe /c "`"$bat`" 2>&1" # stderr inside cmd: vcvars warns there, a terminating error in Windows PowerShell
    if ($LASTEXITCODE -ne 0) { Step 'Native build failed.' 'Red'; exit 1 }
    Copy-Item $dll $Assets -Force

    $flavour = if ($DevFeatures) { 'dev' } else { 'rel' }
    foreach ($f in 'nvngx_dlss.dll', 'nvngx_dlssg.dll') {
        Copy-Item (Join-Path $Sdk "lib\Windows_x86_64\$flavour\$f") $Assets -Force
    }
    Step "NGX feature libraries: $flavour"

    # AMD FSR: the loader and the upscaler / frame generation DLLs (signed: shipped as they are), with AMD's notice.
    foreach ($f in 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll') {
        Copy-Item (Join-Path $FfxKit "signedbin\$f") $Assets -Force
    }
    $license = Get-Content (Join-Path $FfxKit 'docs\license.md') -Encoding UTF8
    $at = 0; for ($i = 0; $i -lt $license.Count; $i++) { if ($license[$i] -match 'are subject to the following license') { $at = $i + 1; break } }
    $notice = @('AMD FSR SDK (amd_fidelityfx_*.dll), github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK ' + $FfxTag, '') +
        ($license[$at..($license.Count - 1)] | Where-Object { $_.Trim() })
    $notice | Set-Content -Encoding UTF8 (Join-Path $Assets 'AMD_FSR_LICENSE.txt')
    Step "AMD FSR $FfxTag DLLs"
}

if (-not $NativeOnly) {
    $dotnet = Join-Path $env:USERPROFILE '.dotnet\dotnet.exe'
    if (-not (Test-Path $dotnet)) { $dotnet = 'dotnet' }
    $env:DOTNET_CLI_UI_LANGUAGE = 'en'
    Step 'Building the mod...'
    & $dotnet build (Join-Path $Root 'mod\MySummerFrames.csproj') -c Release -nologo -v q "-p:ManagedDir=$(Join-Path $GameDir 'mysummercar_Data\Managed')"
    if ($LASTEXITCODE -ne 0) { Step 'Mod build failed.' 'Red'; exit 1 }
    Copy-Item (Join-Path $Root 'mod\bin\Release\MySummerFrames.dll') (Join-Path $Out 'Mods') -Force
}

Step "Done: $Out" 'Green'
