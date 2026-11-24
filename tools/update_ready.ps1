param(
    [string]$Source = "build\native",     # build tree with the freshly built sinandpun.exe
    [string]$Target = "ready",
    [switch]$FetchDlss,                   # download the NVIDIA Streamline / DLSS SDK release (263 MB zip) into third_party\nvidia first
    [switch]$NoDlss                       # do not copy the NVIDIA DLSS DLLs
)
$ErrorActionPreference = 'Stop'
# Refreshes the self-contained, ready-to-play folder `ready\` from the current build.
#   * copies the exe, runtime DLLs, pipeline cache, wii_bootstrap and the prelinked stage modules (modules\stgNN.bin)
#   * NEVER overwrites ready\UserData (settings and saves) once it exists; Config.toml is only created the first time
#   * writes BUILD-INFO.txt (git commit, exe SHA-256, date) and Play.bat
#   * copies the NVIDIA DLSS DLLs (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll + their licence texts)
#     from third_party\nvidia\streamline\bin\x64 when present (-FetchDlss downloads the SDK; -NoDlss skips them).
#     They are NVIDIA-licensed (NVIDIA RTX SDKs LICENSE) and are NOT part of the GPL source tree; the game starts without
#     them and the DLSS option then reports "NVIDIA RTX GPU and nvngx_dlss.dll required" (docs/DLSS-NOTES.md).
# Usage (repo root): powershell -File tools\update_ready.ps1
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if ($FetchDlss) { & "$PSScriptRoot/fetch_graphics_sdks.ps1" }
$src = Join-Path $root $Source
$dst = Join-Path $root $Target
$exe = Join-Path $src "sinandpun.exe"
if (-not (Test-Path $exe)) { throw "no exe at $exe - build first" }
$runtimeAssets = @("dsp_coef.bin", "initial_pipeline_cache.db")
foreach ($asset in ($runtimeAssets + @("SDL3.dll", "webgpu_dawn.dll"))) {
    if (-not (Test-Path -LiteralPath (Join-Path $src $asset) -PathType Leaf)) {
        throw "missing runtime asset in build: $asset"
    }
}
$bootstrap = Join-Path $src "wii_bootstrap"
if (-not (Test-Path -LiteralPath $bootstrap -PathType Container)) {
    throw "missing Wii bootstrap directory in build: $bootstrap"
}
$amd = Join-Path $root "third_party/amd"
$fsr = Join-Path $amd "FidelityFX-SDK"
$amdFiles = @{
    "$fsr/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll" = "amd_fidelityfx_loader_dx12.dll"
    "$fsr/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll" = "amd_fidelityfx_upscaler_dx12.dll"
    "$fsr/docs/license.md" = "licenses/amd-fidelityfx-sdk.md"
    "$fsr/3rdpartynotice.md" = "licenses/amd-3rdpartynotice.md"
    "$amd/FidelityFX-FSR/license.txt" = "licenses/amd-fsr1.txt"
}
foreach ($path in $amdFiles.Keys) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "missing AMD runtime asset: $path" }
}
if (Get-Process sinandpun -ErrorAction SilentlyContinue | Where-Object { $_.Path -and (Split-Path $_.Path -Parent) -eq $dst }) {
    throw "the game in $Target is running; close it first"
}
$moduleNames = 0..7 | ForEach-Object { "stg0$_.bin" }
$moduleSources = @{}
foreach ($moduleName in $moduleNames) {
    $modulePath = Join-Path "$root\build\modules" $moduleName
    if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) { throw "missing linked stage module: $modulePath" }
    $moduleSources[$moduleName] = $modulePath
}
New-Item -ItemType Directory -Force $dst | Out-Null
Get-ChildItem -LiteralPath $dst -File -Filter "*.dll" | Remove-Item -Force
Copy-Item $exe (Join-Path $dst "sinandpun.exe") -Force
foreach ($asset in $runtimeAssets) {
    Copy-Item -LiteralPath (Join-Path $src $asset) -Destination $dst -Force
}
$achievementNotices = Join-Path $src "recomp_achievements_licenses"
if (Test-Path -LiteralPath $achievementNotices) {
    $noticesDir = New-Item -ItemType Directory -Force (Join-Path $dst "recomp_achievements_licenses")
    Copy-Item (Join-Path $achievementNotices "*.txt") $noticesDir -Force
}
Get-ChildItem $src -File -Filter "*.dll" | ForEach-Object {
    if (!$NoDlss -or $_.Name -notmatch "^(sl\.|nvngx_)") { Copy-Item $_.FullName $dst -Force }
}
Copy-Item (Join-Path $root "LICENSE") $dst -Force
Copy-Item (Join-Path $root "recomplauncher.json") $dst -Force
$codecNotices = New-Item -ItemType Directory -Force (Join-Path $dst "disc_codec_licenses")
Copy-Item (Join-Path $root "third_party\disc_codecs\*.txt") $codecNotices -Force

# Built-in HD HUD/GUI pack (rebuilt high-resolution HUD textures; see docs/HD-HUD.md). It lives outside git in
# assets-local\hd_hud and is loaded from hd_hud\ beside the exe. Replaced wholesale so removed files do not linger.
$hdHudSrc = Join-Path $root "assets-local\hd_hud"
$hdHudDst = Join-Path $dst "hd_hud"
if (Test-Path -LiteralPath $hdHudSrc -PathType Container) {
    if (Test-Path -LiteralPath $hdHudDst) { Remove-Item -LiteralPath $hdHudDst -Recurse -Force }
    Copy-Item -LiteralPath $hdHudSrc -Destination $hdHudDst -Recurse -Force
    Write-Host ("HD HUD pack: {0} textures" -f (Get-ChildItem -LiteralPath $hdHudDst -File -Filter "*.png").Count)
} else {
    Write-Warning "assets-local\hd_hud not found: the package will use the original low-resolution HUD"
}

# Built-in HD world pack (292 remastered environment/object/character textures, DXT1/DXT5 DDS with mips, ~350 MB).
# Outside git in assets-local\hd_world, loaded from hd_world\ beside the exe (see docs/HD-WORLD.md).
$hdWorldSrc = Join-Path $root "assets-local\hd_world"
$hdWorldDst = Join-Path $dst "hd_world"
if (Test-Path -LiteralPath $hdWorldSrc -PathType Container) {
    if (Test-Path -LiteralPath $hdWorldDst) { Remove-Item -LiteralPath $hdWorldDst -Recurse -Force }
    Copy-Item -LiteralPath $hdWorldSrc -Destination $hdWorldDst -Recurse -Force
    Write-Host ("HD world pack: {0} textures" -f (Get-ChildItem -LiteralPath $hdWorldDst -File -Filter "*.dds").Count)
} else {
    throw "assets-local\hd_world not found: the HD world texture pack must ship with every build (docs/HD-WORLD.md)"
}

$dlssDir = Join-Path $root "third_party\nvidia\streamline"
if (-not $NoDlss) {
    $dllSrc = Join-Path $dlssDir "bin\x64"
    if (Test-Path (Join-Path $dllSrc "sl.interposer.dll")) {
        foreach ($f in "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "nvngx_dlss.dll", "sl.dlss_g.dll", "nvngx_dlssg.dll", "sl.reflex.dll", "sl.pcl.dll") {
            $p = Join-Path $dllSrc $f
            if (Test-Path $p) { Copy-Item $p $dst -Force } else { Write-Warning "DLSS: missing $f in $dllSrc" }
        }
        New-Item -ItemType Directory -Force (Join-Path $dst "licenses") | Out-Null
        foreach ($f in "reflex.license.txt", "nvngx_dlss.license.txt", "..\..\license.txt", "..\..\3rd-party-licenses.md") {
            $p = Join-Path $dllSrc $f
            if (Test-Path $p) { Copy-Item $p (Join-Path $dst ("licenses\nvidia-" + (Split-Path $f -Leaf))) -Force }
        }
        "copied the NVIDIA DLSS DLLs from $dllSrc"
    } else {
        "NVIDIA DLSS DLLs not staged ($dllSrc); the in-game DLSS option will report them as missing (run with -FetchDlss)"
    }
}
New-Item -ItemType Directory -Force (Join-Path $dst "licenses") | Out-Null
foreach ($path in $amdFiles.Keys) {
    Copy-Item -LiteralPath $path -Destination (Join-Path $dst $amdFiles[$path]) -Force
}
Copy-Item -LiteralPath $bootstrap -Destination $dst -Recurse -Force
New-Item -ItemType Directory -Force (Join-Path $dst "modules") | Out-Null
Remove-Item (Join-Path $dst "modules\stg0?.bin") -Force -ErrorAction SilentlyContinue
foreach ($moduleName in $moduleNames) {
    Copy-Item $moduleSources[$moduleName] (Join-Path $dst "modules") -Force
}

# portable mode marker: user state (Config.toml, NAND saves, Cache, Logs) lives in ready\UserData, not %LOCALAPPDATA%
Set-Content (Join-Path $dst "portable.txt") "portable"
# settings + saves live in ready\UserData and are never overwritten
$ud = Join-Path $dst "UserData"
New-Item -ItemType Directory -Force $ud | Out-Null
$cacheSrc = Join-Path $src "UserData\Cache"
$cacheDst = Join-Path $ud "cache"
if ((Test-Path $cacheSrc) -and -not (Test-Path $cacheDst)) {
    # warmed shader/pipeline caches (this machine's GPU/driver) so the first run in ready\ does not hitch
    Copy-Item $cacheSrc $cacheDst -Recurse
    "seeded shader/pipeline cache into $cacheDst"
}
$cfg = Join-Path $ud "Config.toml"
if (-not (Test-Path $cfg)) {
    $dvd = (Resolve-Path "orig\R2VE01" -ErrorAction SilentlyContinue)
    $dvdPath = if ($dvd) { $dvd.Path.Replace('\', '/') } else { "D:/path/to/R2VE01" }
    @"
[paths]
dvd_root = '$dvdPath'

[video]
display_mode = "windowed"
vrr = "off"
window_width = 1920
window_height = 1080
force_aspect = true

[input]
version = 1
scheme = "pointer"
"@ | Set-Content $cfg
    "created $cfg (dvd_root = $dvdPath)"
}

@"
@echo off
cd /d "%~dp0"
start "" "%~dp0sinandpun.exe"
"@ | Set-Content (Join-Path $dst "Play.bat") -Encoding ASCII

$commit = (git -c core.fsmonitor=false rev-parse --short HEAD) 2>$null
if ($LASTEXITCODE -ne 0) { throw "could not determine the source commit" }
$dirty = if ((git -c core.fsmonitor=false status --short --untracked-files=no) 2>$null) { " (+uncommitted tracked changes)" } else { "" }
$hash = (Get-FileHash (Join-Path $dst "sinandpun.exe") -Algorithm SHA256).Hash
@"
Sin & Punishment: Star Successor - recomp (ready to play / test)
Updated : $(Get-Date -Format 'yyyy-MM-dd HH:mm')
Commit  : $commit$dirty
Exe     : sinandpun.exe  $((Get-Item (Join-Path $dst 'sinandpun.exe')).Length) bytes
SHA-256 : $hash

Run Play.bat (or sinandpun.exe). Your disc folder is set by dvd_root in UserData\Config.toml.
Open the in-game options with the game's own OPTIONS entry (title or pause menu).
Settings and saves live in UserData\ and are kept when this folder is refreshed.
No game data is included: the disc files stay where dvd_root points.
"@ | Set-Content (Join-Path $dst "BUILD-INFO.txt")

"ready folder updated: $dst"
Get-Content (Join-Path $dst "BUILD-INFO.txt")
"size: {0:N0} MB" -f ((Get-ChildItem $dst -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
