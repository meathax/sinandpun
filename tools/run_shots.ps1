param(
    [string]$At = "60,90,120",     # comma-separated seconds after launch at which to capture
    [string]$Name = "shots",
    [string]$Env = ""              # ONE string, comma-joined K=V items (see run_game.ps1)
)
# Runs the game once and captures a screenshot at each time in -At (build\screenshots\<Name>_<sec>.png),
# then closes it. Usage: powershell -File tools\run_shots.ps1 -At "70,90,110" -Name s6 -Env "SIN_VIRTUAL_REMOTE=1,SIN_AUTOSTAGE=6"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
Get-Process WiiCompiled -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
foreach ($e in ($Env -split ',')) { if ($e) { $k, $v = $e -split '=', 2; Set-Item -Path "Env:$k" -Value $v } }
$p = Start-Process -FilePath "$root\build\native\WiiCompiled.exe" -WorkingDirectory "$root\build\native" `
    -RedirectStandardOutput "$root\build\boot-out.log" -RedirectStandardError "$root\build\boot-err.log" -PassThru
$t0 = Get-Date
foreach ($s in (($At -split ',') | ForEach-Object { [int]$_ } | Sort-Object)) {
    $wait = $s - ((Get-Date) - $t0).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Seconds $wait }
    if ($p.HasExited) { "process exited before ${s}s (exit code $($p.ExitCode))"; break }
    $out = "build\screenshots\${Name}_$s.png"
    powershell -File "$root\tools\screenshot.ps1" -OutFile $out | Out-Null
    python -c "
from PIL import Image
im=Image.open(r'$out').convert('RGB'); im.resize((im.width//3,im.height//3)).save(r'$($out -replace '\.png$','s.png')')"
    "captured $out"
}
if (-not $p.HasExited) { $null = $p.CloseMainWindow(); Start-Sleep -Seconds 6; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
foreach ($e in ($Env -split ',')) { if ($e) { $k = ($e -split '=', 2)[0]; Remove-Item "Env:$k" -ErrorAction SilentlyContinue } }
