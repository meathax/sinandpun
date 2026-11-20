param(
    [int[]]$Stages = @(-1, 0, 1, 2, 3, 4, 5, 6, 7),  # -1 = title/menu only (no SIN_AUTOSTAGE); 0..7 = SIN_AUTOSTAGE stage
    [int]$Seconds = 110,                              # per stage (the scripted pointer run is ~110 s)
    [string]$Source = "build\native",
    [switch]$NoMerge,
    [switch]$UseLock = $true                          # take build\agent.lock around every run (shared build tree)
)
# Warms the pipeline cache: runs the game through every requested stage with the standard scripted pointer run, so that
# aurora records every GX pipeline config it needs into <Source>\UserData\Cache\pipeline_cache.db, then exports the merged
# table to <Source>\initial_pipeline_cache.db (shipped next to the exe by tools\update_ready.ps1; aurora seeds the user's
# cache from it at start and compiles the whole list on background workers while the game boots).
# The cache stores only pipeline *configs* (not compiled blobs), so it is portable across GPUs/drivers.
# Usage (repo root): powershell -File tools\warm_pipeline_cache.ps1 [-Stages 1,6 -Seconds 110]
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$lockDir = Join-Path $root "build\agent.lock"
function Take-Lock($who) {
    for ($i = 0; $i -lt 1200; $i++) {
        try { New-Item -ItemType Directory $lockDir -ErrorAction Stop | Out-Null; Set-Content "$lockDir\owner.txt" "$who $(Get-Date -f HH:mm:ss)"; return }
        catch { Start-Sleep 1 }
    }
    throw "could not get the build lock; owner: $(Get-Content $lockDir\owner.txt -ErrorAction SilentlyContinue)"
}
function Release-Lock { Remove-Item -Recurse -Force $lockDir -ErrorAction SilentlyContinue }

$script = '32|0.3:A|10|0.21::-1/0|0.5|0.3:A|5|0.15::-1/0|0.3|0.19::0/-1|0.5|0.3:A|1.5|0.056::1/0|0.3|0.23::0/1|0.5|0.3:A|4|0.3:PLUS|4|0.3:A|4|0.3:A|4|0.04::-1/0|0.2|0.095::0/-1|0.5|0.3:A|4|0.3:A|4|0.3:A|4|0.3:A|120'
$db = Join-Path $root "$Source\UserData\Cache\pipeline_cache.db"
function Row-Count {
    if (-not (Test-Path $db)) { return 0 }
    [int](python -c "import sqlite3,sys; print(sqlite3.connect(sys.argv[1]).execute('select count(*) from pipeline_cache').fetchone()[0])" $db)
}
"pipeline rows before: $(Row-Count)"
foreach ($s in $Stages) {
    $envList = "SIN_VIRTUAL_REMOTE=1,SIN_VREMOTE_SCRIPT=$script"
    if ($s -ge 0) { $envList += ",SIN_AUTOSTAGE=$s" }
    if ($UseLock) { Take-Lock "warm_pipeline_cache stage $s" }
    try { powershell -File "$root\tools\run_game.ps1" -Seconds $Seconds -Name "warm_s$s" -Env $envList | Out-Null }
    finally { if ($UseLock) { Release-Lock } }
    "stage ${s}: pipeline rows now $(Row-Count)"
}
if (-not $NoMerge) {
    $out = Join-Path $root "$Source\initial_pipeline_cache.db"
    $tmp = "$out.new"
    Remove-Item $tmp -ErrorAction SilentlyContinue
    python -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); c.execute('VACUUM INTO ?', (sys.argv[2],)); c.close()" $db $tmp
    if (Test-Path $tmp) {
        Move-Item $tmp $out -Force
        "exported $out ($((Get-Item $out).Length) bytes, $(Row-Count) rows); update_ready.ps1 ships it"
    } else { Write-Warning "export failed" }
}
