param(
    [string]$Source = "build\native",     # build tree with the freshly built WiiCompiled.exe
    [string]$Target = "ready",
    [switch]$FetchDlss,                   # download the NVIDIA Streamline / DLSS SDK release (263 MB zip) into third_party\nvidia first
    [switch]$NoDlss                       # do not copy the NVIDIA DLSS DLLs
)
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
$exe = Join-Path $src "WiiCompiled.exe"
if (-not (Test-Path $exe)) { throw "no exe at $exe - build first" }
$runtimeAssets = @("dsp_coef.bin", "initial_pipeline_cache.db")
foreach ($asset in $runtimeAssets) {
    if (-not (Test-Path -LiteralPath (Join-Path $src $asset) -PathType Leaf)) {
        throw "missing runtime asset in build: $asset"
    }
}
if (Get-Process WiiCompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $dst "WiiCompiled.exe") }) {
    throw "the game in $Target is running; close it first"
}
$moduleNames = 0..7 | ForEach-Object { "stg0$_.bin" }
$moduleSources = @{}
foreach ($moduleName in $moduleNames) {
    $modulePath = Join-Path "$root\build\modules" $moduleName
    if (-not (Test-Path $modulePath)) { throw "missing linked stage module: $modulePath" }
    $moduleSources[$moduleName] = $modulePath
}
New-Item -ItemType Directory -Force $dst | Out-Null
Remove-Item (Join-Path $dst "*.dll") -Force -ErrorAction SilentlyContinue
Copy-Item $exe (Join-Path $dst "WiiCompiled.exe") -Force
foreach ($asset in $runtimeAssets) {
    Copy-Item -LiteralPath (Join-Path $src $asset) -Destination $dst -Force
}
$achievementNotices = Join-Path $src "recomp_achievements_licenses"
if (Test-Path -LiteralPath $achievementNotices) {
    $noticesDir = New-Item -ItemType Directory -Force (Join-Path $dst "recomp_achievements_licenses")
    Copy-Item (Join-Path $achievementNotices "*.txt") $noticesDir -Force
}
Get-ChildItem $src -File -Filter "*.dll" | ForEach-Object {
    Copy-Item $_.FullName $dst -Force
}
Copy-Item (Join-Path $root "LICENSE") $dst -Force
Copy-Item (Join-Path $root "recomplauncher.json") $dst -Force
$codecNotices = New-Item -ItemType Directory -Force (Join-Path $dst "disc_codec_licenses")
Copy-Item (Join-Path $root "third_party\disc_codecs\*.txt") $codecNotices -Force

$dlssDir = Join-Path $root "third_party\nvidia\streamline"
if (-not $NoDlss) {
    $dllSrc = Join-Path $dlssDir "bin\x64"
    if (Test-Path (Join-Path $dllSrc "sl.interposer.dll")) {
        foreach ($f in "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "nvngx_dlss.dll") {
            $p = Join-Path $dllSrc $f
            if (Test-Path $p) { Copy-Item $p $dst -Force } else { Write-Warning "DLSS: missing $f in $dllSrc" }
        }
        New-Item -ItemType Directory -Force (Join-Path $dst "licenses") | Out-Null
        foreach ($f in "nvngx_dlss.license.txt", "..\..\license.txt", "..\..\3rd-party-licenses.md") {
            $p = Join-Path $dllSrc $f
            if (Test-Path $p) { Copy-Item $p (Join-Path $dst ("licenses\nvidia-" + (Split-Path $f -Leaf))) -Force }
        }
        "copied the NVIDIA DLSS DLLs from $dllSrc"
    } else {
        "NVIDIA DLSS DLLs not staged ($dllSrc); the in-game DLSS option will report them as missing (run with -FetchDlss)"
    }
}
$amd = Join-Path $root "third_party/amd"
$fsr = Join-Path $amd "FidelityFX-SDK"
foreach ($name in "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll") {
    Copy-Item -LiteralPath (Join-Path $fsr "Kits/FidelityFX/signedbin/$name") -Destination $dst -Force -ErrorAction Stop
}
New-Item -ItemType Directory -Force (Join-Path $dst "licenses") | Out-Null
Copy-Item "$fsr/docs/license.md" "$dst/licenses/amd-fidelityfx-sdk.md" -Force -ErrorAction Stop
Copy-Item "$fsr/3rdpartynotice.md" "$dst/licenses/amd-3rdpartynotice.md" -Force -ErrorAction Stop
Copy-Item "$amd/FidelityFX-FSR/license.txt" "$dst/licenses/amd-fsr1.txt" -Force -ErrorAction Stop
if (Test-Path (Join-Path $src "wii_bootstrap")) {
    Copy-Item (Join-Path $src "wii_bootstrap") $dst -Recurse -Force
}
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
start "" "%~dp0WiiCompiled.exe"
"@ | Set-Content (Join-Path $dst "Play.bat") -Encoding ASCII

$commit = (git rev-parse --short HEAD) 2>$null
$dirty = if ((git status --short) 2>$null) { " (+uncommitted changes)" } else { "" }
$hash = (Get-FileHash (Join-Path $dst "WiiCompiled.exe") -Algorithm SHA256).Hash
@"
Sin & Punishment: Star Successor - recomp (ready to play / test)
Updated : $(Get-Date -Format 'yyyy-MM-dd HH:mm')
Commit  : $commit$dirty
Exe     : WiiCompiled.exe  $((Get-Item (Join-Path $dst 'WiiCompiled.exe')).Length) bytes
SHA-256 : $hash

Run Play.bat (or WiiCompiled.exe). Your disc folder is set by dvd_root in UserData\Config.toml.
Open the in-game options with the game's own OPTIONS entry (title or pause menu).
Settings and saves live in UserData\ and are kept when this folder is refreshed.
No game data is included: the disc files stay where dvd_root points.
"@ | Set-Content (Join-Path $dst "BUILD-INFO.txt")

"ready folder updated: $dst"
Get-Content (Join-Path $dst "BUILD-INFO.txt")
"size: {0:N0} MB" -f ((Get-ChildItem $dst -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
