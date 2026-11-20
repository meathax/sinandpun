param(
    [string]$Source = "build\native",     # build tree with the freshly built WiiCompiled.exe
    [string]$Target = "ready"
)
# Refreshes the self-contained, ready-to-play folder `ready\` from the current build.
#   * copies the exe, runtime DLLs, pipeline cache, wii_bootstrap and the prelinked stage modules (modules\stgNN.bin)
#   * NEVER overwrites ready\UserData (settings and saves) once it exists; Config.toml is only created the first time
#   * writes BUILD-INFO.txt (git commit, exe SHA-256, date) and Play.bat
# Usage (repo root): powershell -File tools\update_ready.ps1
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$src = Join-Path $root $Source
$dst = Join-Path $root $Target
$exe = Join-Path $src "WiiCompiled.exe"
if (-not (Test-Path $exe)) { throw "no exe at $exe - build first" }
if (Get-Process WiiCompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $dst "WiiCompiled.exe") }) {
    throw "the game in $Target is running; close it first"
}
New-Item -ItemType Directory -Force $dst | Out-Null

foreach ($f in "WiiCompiled.exe", "SDL3.dll", "webgpu_dawn.dll", "dxcompiler.dll", "dxil.dll", "libc++.dll", "libpng16.dll",
               "libunwind.dll", "libzlib1.dll", "initial_pipeline_cache.db") {
    $p = Join-Path $src $f
    if (Test-Path $p) { Copy-Item $p $dst -Force } else { Write-Warning "missing $f in $Source" }
}
if (Test-Path (Join-Path $src "wii_bootstrap")) {
    Copy-Item (Join-Path $src "wii_bootstrap") $dst -Recurse -Force
}
New-Item -ItemType Directory -Force (Join-Path $dst "modules") | Out-Null
Get-ChildItem build\modules -Filter "stg0?.bin" | ForEach-Object { Copy-Item $_.FullName (Join-Path $dst "modules") -Force }

# portable mode marker: user state (Config.toml, NAND saves, Cache, Logs) lives in ready\UserData, not %LOCALAPPDATA%
Set-Content (Join-Path $dst "portable.txt") "portable"
# settings + saves live in ready\UserData and are never overwritten
$ud = Join-Path $dst "UserData"
New-Item -ItemType Directory -Force $ud | Out-Null
$cacheSrc = Join-Path $src "UserData\Cache"
$cacheDst = Join-Path $ud "Cache"
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
