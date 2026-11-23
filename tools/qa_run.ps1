param(
    [string]$Name = "qa",
    [int]$Seconds = 300,
    [string]$Env = "",             # ONE string, comma-joined K=V; use ';' when a value contains commas
    [int]$Stage = -1,              # SIN_AUTOSTAGE
    [string]$Style = "fight",
    [int]$Seed = 1,
    [int]$ShotEvery = 60,
    [double]$PauseEvery = 0,
    [double]$Lead = 30,
    [double]$AEvery = 7,
    [string]$MenuScript = "0.2:A:@0.113/0.72|2.8::@0.113/0.72|loop",
    [string]$Script = ""           # explicit SIN_VREMOTE_SCRIPT (overrides the bot)
)
# Automated QA run: takes the shared lock, runs the game with the input bot, screenshots every $ShotEvery s,
# stops on exit/crash, releases the lock. Output in build\qa\<Name>\ (out.log, err.log, shots, summary.txt).
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$lock = "$root\build\agent.lock"
function Take-Lock($who) { for ($i = 0; $i -lt 3000; $i++) { try { New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null; Set-Content "$lock\owner.txt" "$who $(Get-Date -f HH:mm:ss)"; return } catch { Start-Sleep 1 } }; throw "no lock" }
$dir = "$root\build\qa\$Name"; New-Item -ItemType Directory -Force $dir | Out-Null
$scriptFile = "$dir\script.txt"
if (-not $Script) {
    $Script = (python tools\qa_bot.py --seed $Seed --seconds ($Seconds + 60) --style $Style --pause-every $PauseEvery --lead 0 --a-every $AEvery)
    $MenuScript | Set-Content $scriptFile -NoNewline   # menu phase: A taps only (default focus = 1P Start / file 1)
    $phased = $true
}
$envs = @{ SIN_VIRTUAL_REMOTE = "1"; SIN_PERF_LOG = "1"; SIN_VREMOTE_SCRIPT = "1"; SIN_VREMOTE_SCRIPT_FILE = $scriptFile; SIN_AUTOSTAGE_TRACE = "1" }
if ($Stage -ge 0) { $envs.SIN_AUTOSTAGE = "$Stage" }
if (-not $phased) { $envs.SIN_VREMOTE_SCRIPT = $Script; $envs.Remove("SIN_VREMOTE_SCRIPT_FILE") }
$envEntries = if ($Env.Contains(';')) { $Env -split ';' } else { $Env -split ',' }
foreach ($e in $envEntries) { if ($e) { $k, $v = $e -split '=', 2; $envs[$k] = $v } }
Take-Lock "qa-$Name"
try {
    $src = "$root\build\native\sinandpun.exe"; $exe = "$root\build\native\SinQA.exe"
    if (-not (Test-Path $exe) -or (Get-Item $src).LastWriteTime -gt (Get-Item $exe).LastWriteTime) { Copy-Item $src $exe -Force }
    Get-Process SinQA -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    foreach ($k in $envs.Keys) { Set-Item -Path "Env:$k" -Value $envs[$k] }
    $logsBefore = @(Get-ChildItem build\native\UserData\Logs -Directory -ErrorAction SilentlyContinue | % Name)
    $p = Start-Process -FilePath $exe -WorkingDirectory "$root\build\native" `
        -RedirectStandardOutput "$dir\out.log" -RedirectStandardError "$dir\err.log" -PassThru
    $t0 = Get-Date; $nextShot = $ShotEvery; $shots = 0; $ended = "timeout"; $nextGrowth = 0
    $growthFile = "$dir\log_growth.csv"
    "elapsed_s,err_bytes,out_bytes,runtime_bytes" | Set-Content $growthFile -Encoding UTF8
    while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
        Start-Sleep -Seconds 2
        if ($phased -and -not $fight -and ((Select-String -Path "$dir\err.log" -Pattern "F state \S+ -> 4 " -Quiet))) { $Script | Set-Content $scriptFile -NoNewline; $fight = $true; "fight phase at $([int]((Get-Date) - $t0).TotalSeconds)s" | Add-Content "$dir\summary.txt" }
        if ($p.HasExited) { $ended = "EXITED code $($p.ExitCode) at $([int]((Get-Date) - $t0).TotalSeconds)s"; break }
        $el = ((Get-Date) - $t0).TotalSeconds
        if ($el -ge $nextGrowth) {
            $runtimeBytes = 0L
            $runLogDirs = @(Get-ChildItem build\native\UserData\Logs -Directory -ErrorAction SilentlyContinue |
                Where-Object { $logsBefore -notcontains $_.Name })
            foreach ($runLogDir in $runLogDirs) {
                $runtimeBytes += (Get-ChildItem $runLogDir.FullName -Recurse -File -ErrorAction SilentlyContinue |
                    Measure-Object Length -Sum).Sum
            }
            $errBytes = if (Test-Path "$dir\err.log") { (Get-Item "$dir\err.log").Length } else { 0 }
            $outBytes = if (Test-Path "$dir\out.log") { (Get-Item "$dir\out.log").Length } else { 0 }
            "$([int]$el),$errBytes,$outBytes,$runtimeBytes" | Add-Content $growthFile -Encoding UTF8
            $nextGrowth = $el + 10
        }
        if ($ShotEvery -gt 0 -and $el -ge $nextShot) {
            $nextShot += $ShotEvery
            $j = Start-Job { param($r, $o) Set-Location $r; powershell -File "$r\tools\screenshot.ps1" -ProcessName SinQA -OutFile $o } -ArgumentList $root, "$dir\s$([int]$el).png"
            if (-not (Wait-Job $j -Timeout 40)) { "screenshot wedged at $([int]$el)s" | Add-Content "$dir\summary.txt" }
            Remove-Job $j -Force -ErrorAction SilentlyContinue; $shots++
        }
    }
    $elapsed = [int]((Get-Date) - $t0).TotalSeconds
    if (-not $p.HasExited) { $null = $p.CloseMainWindow(); Start-Sleep 6; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "(killed)" | Add-Content "$dir\summary.txt" } }
    foreach ($k in $envs.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
} finally { Remove-Item -Recurse -Force $lock -ErrorAction SilentlyContinue }
$newLogs = @(Get-ChildItem build\native\UserData\Logs -Directory -ErrorAction SilentlyContinue | ? { $logsBefore -notcontains $_.Name } | % FullName)
"$Name ended: $ended elapsed ${elapsed}s artifacts: $($newLogs -join ';')" | Tee-Object -Append "$dir\summary.txt"
foreach ($d in $newLogs) { Get-ChildItem $d | % { "  $($_.Name) $($_.Length)" } | Add-Content "$dir\summary.txt" }
$pat = 'FATAL|Runtime error|not translated|ERROR|sound-slot-guard|DANGLING|Access|unhandled|abort|watchdog'
Select-String -Path "$dir\err.log","$dir\out.log" -Pattern $pat | Select-Object -First 25 | % { $_.Line.Substring(0,[Math]::Min(200,$_.Line.Length)) }
