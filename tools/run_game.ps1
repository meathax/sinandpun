param(
    [int]$Seconds = 50,
    [string]$Name = "run",
    [string[]]$Env = @(),          # e.g. -Env SIN_VIRTUAL_REMOTE=1,SIN_CALLTRACE=1
    [string]$Pattern = '\[dvd\]|\[gx\]|Virtual|Wii Remote|\[bt\]|FATAL|reason|Runtime error|Missing|not translated|ERROR|adopted|PARKING'
)
# Runs the game from build\native for $Seconds, captures a PrintWindow screenshot,
# closes it politely, then prints the key log lines and pixel statistics.
# Usage (repo root): powershell -File tools\run_game.ps1 -Seconds 50 -Name m1 -Env SIN_VIRTUAL_REMOTE=1,SIN_CALLTRACE=1
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
Get-Process WiiCompiled -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Remove-Item build\native\calltrace.txt -ErrorAction SilentlyContinue
foreach ($e in ($Env | ForEach-Object { $_ -split ',' })) { $k, $v = $e -split '=', 2; Set-Item -Path "Env:$k" -Value $v }
$p = Start-Process -FilePath "$root\build\native\WiiCompiled.exe" -WorkingDirectory "$root\build\native" `
    -RedirectStandardOutput "$root\build\boot-out.log" -RedirectStandardError "$root\build\boot-err.log" -PassThru
Start-Sleep -Seconds $Seconds
# Take the screenshot in a watchdog'd child: PrintWindow(PW_RENDERFULLCONTENT)
# can block indefinitely on a busy swapchain window, which must not wedge the
# whole run. On timeout we report and carry on (the game is closed next anyway).
$shot = "build\screenshots\$Name.png"
$shotJob = Start-Job -ScriptBlock {
    param($root, $out)
    Set-Location $root
    powershell -File "$root\tools\screenshot.ps1" -OutFile $out
} -ArgumentList $root, $shot
if (Wait-Job $shotJob -Timeout 45) {
    Receive-Job $shotJob | Write-Output
} else {
    Write-Output "screenshot timed out after 45 s (PrintWindow wedged); continuing"
}
Remove-Job $shotJob -Force -ErrorAction SilentlyContinue
$null = $p.CloseMainWindow()
Start-Sleep -Seconds 6
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "(had to kill the process; crash artifacts may be missing)" } else { "exit code: $($p.ExitCode)" }
foreach ($e in $Env) { $k = ($e -split '=', 2)[0]; Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
"---- key log lines (last 25) from build\boot-err.log"
Select-String -Path build\boot-err.log -Pattern $Pattern | Select-Object -Last 25 |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
"---- log size: $((Get-Content build\boot-err.log | Measure-Object -Line).Lines) lines"
"---- screenshot stats"
python tools\shot_stats.py $shot
if (Test-Path build\native\calltrace.txt) { "---- calltrace: $((Get-Content build\native\calltrace.txt | Measure-Object -Line).Lines) distinct call targets (python tools\calltrace_sym.py --tail 40)" }
$art = Get-ChildItem build\native\UserData\Logs\base_* -Directory -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
if ($art) { "---- newest artifact: $($art.FullName) (python tools\thread_dump.py `"$($art.FullName)`")" }
