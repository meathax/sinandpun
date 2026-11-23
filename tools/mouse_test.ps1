param(
    [string]$Name = "m1",
    [int]$Width = 1280,
    [int]$Height = 720,
    [string]$Display = "windowed",           # windowed | borderless
    [string]$ForceAspect = "true",            # video.force_aspect
    [switch]$Rel,                             # [mouse] raw = true (relative mode)
    [string]$Script = "",                     # SIN_VMOUSE_SCRIPT text
    [string]$ScriptFile = "",                 # or a file holding it
    [int]$Seconds = 120,
    [string]$Shots = "",                      # comma list of seconds after launch
    [switch]$Warp,                            # SIN_VMOUSE_WARP=1 (also move the real OS cursor)
    [string]$Stage = "1",                     # SIN_AUTOSTAGE ('' = none: title/menu run)
    [string]$Cfg = "",                        # extra Config.toml edits: "mouse.sensitivity=1.0;aim.smoothing_ms=0"
    [string]$Env = ""                         # extra env: "A=1,B=2"
)
# One instrumented game run for the mouse/keyboard verification (docs/AIM-VERIFICATION-MOUSE.md).
# Takes the shared agent lock, writes a scratch Config.toml (restored afterwards), runs the game with the
# synthetic mouse harness (SIN_VMOUSE_SCRIPT) and aim telemetry (SIN_AIM_LOG), keeps logs in build\mouse\<Name>\.
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$lock = "$root\build\agent.lock"
$got = $false
for ($i = 0; $i -lt 1500; $i++) {
    try { New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null; Set-Content "$lock\owner.txt" "mouse-test-$Name $(Get-Date -f HH:mm:ss)"; $got = $true; break }
    catch { Start-Sleep 1 }
}
if (-not $got) { throw "no lock; owner: $(Get-Content $lock\owner.txt -ErrorAction SilentlyContinue)" }
$cfgPath = "$root\build\native\UserData\Config.toml"
$dir = "$root\build\mouse\$Name"
New-Item -ItemType Directory -Force $dir | Out-Null
New-Item -ItemType Directory -Force "$root\build\screenshots" | Out-Null
$backup = "$root\build\mouse\Config.toml.orig"
if (-not (Test-Path $backup) -or -not (Select-String -Path $cfgPath -Pattern "# mouse-test-config" -SimpleMatch -Quiet)) { Copy-Item $cfgPath $backup -Force }
$setEnv = @()
$p = $null
try {
    $edits = @("video.window_width=$Width", "video.window_height=$Height", "video.display_mode=`"$Display`"",
               "video.force_aspect=$ForceAspect", "video.widescreen=true", "video.window_x=40", "video.window_y=40",
               'input.scheme="auto"', 'input.version=2', "mouse.raw=$(if ($Rel) { 'true' } else { 'false' })")
    if ($Cfg) { $edits += ($Cfg -split ';') }
    python "$root\tools\mouse_cfg.py" $cfgPath @edits
    Add-Content $cfgPath "# mouse-test-config"
    $envs = @{ SIN_VIRTUAL_REMOTE = "1"; SIN_AIM_LOG = "1"; SIN_VMOUSE_LOG = "100" }
    if ($Stage -ne "") { $envs.SIN_AUTOSTAGE = $Stage }
    if ($ScriptFile) { $envs.SIN_VMOUSE_SCRIPT = (Get-Content $ScriptFile -Raw).Trim() } elseif ($Script) { $envs.SIN_VMOUSE_SCRIPT = $Script }
    if ($Warp) { $envs.SIN_VMOUSE_WARP = "1" }
    foreach ($e in ($Env -split ',')) { if ($e) { $k, $v = $e -split '=', 2; $envs[$k] = $v } }
    foreach ($k in $envs.Keys) { Set-Item -Path "Env:$k" -Value $envs[$k]; $setEnv += $k }
    $p = Start-Process -FilePath "$root\build\native\sinandpun.exe" -WorkingDirectory "$root\build\native" `
        -RedirectStandardOutput "$dir\out.log" -RedirectStandardError "$dir\err.log" -PassThru
    $t0 = Get-Date
    $shotList = @(); if ($Shots) { $shotList = ($Shots -split ',') | ForEach-Object { [double]$_ } | Sort-Object }
    foreach ($s in $shotList) {
        $wait = $s - ((Get-Date) - $t0).TotalSeconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
        if ($p.HasExited) { "process exited before $s s"; break }
        $out = "$root\build\screenshots\mouse_${Name}_$([int]$s).png"
        $job = Start-Job -ScriptBlock { param($r, $o) Set-Location $r; powershell -File "$r\tools\screenshot.ps1" -OutFile $o } -ArgumentList $root, $out
        if (-not (Wait-Job $job -Timeout 30)) { "screenshot timed out at $s" }
        Remove-Job $job -Force -ErrorAction SilentlyContinue
        "shot $out"
    }
    $left = $Seconds - ((Get-Date) - $t0).TotalSeconds
    if ($left -gt 0 -and -not $p.HasExited) { Start-Sleep -Milliseconds ([int]($left * 1000)) }
    if (-not $p.HasExited) { $null = $p.CloseMainWindow(); Start-Sleep 6 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "(killed)" } else { "exit code: $($p.ExitCode)" }
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    foreach ($k in $setEnv) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    if (Test-Path $backup) { Copy-Item $backup $cfgPath -Force }
    Remove-Item -Recurse -Force $lock -ErrorAction SilentlyContinue
}
"logs: $dir  vmouse lines: $((Select-String -Path $dir\err.log -Pattern '\[vmouse\]' -SimpleMatch).Count)  aim lines: $((Select-String -Path $dir\err.log -Pattern '\[aim\]' -SimpleMatch).Count)"

