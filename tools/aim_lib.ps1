# Shared helpers for the aim tests (dot-source): build/run lock, game launch with env + private logs, screenshots.
$script:Root = Split-Path $PSScriptRoot -Parent
$script:LockDir = 'D:\Arcade\AI\aCORES\Sin\build\agent.lock'

function Take-Lock($who) {
    for ($i = 0; $i -lt 1200; $i++) {
        try { New-Item -ItemType Directory $script:LockDir -ErrorAction Stop | Out-Null; Set-Content "$script:LockDir\owner.txt" "$who $(Get-Date -f HH:mm:ss)"; return }
        catch { Start-Sleep 1 }
    }
    throw "could not get the build lock; owner: $(Get-Content $script:LockDir\owner.txt -ErrorAction SilentlyContinue)"
}
function Release-Lock { Remove-Item -Recurse -Force $script:LockDir -ErrorAction SilentlyContinue }

# Starts WiiCompiled with the given environment (hashtable); stderr goes to $LogPath. Returns the process.
function Start-Game([hashtable]$Env, [string]$LogPath) {
    New-Item -ItemType Directory -Force (Split-Path $LogPath) | Out-Null
    foreach ($k in $Env.Keys) { Set-Item -Path "Env:$k" -Value ([string]$Env[$k]) }
    try {
        return Start-Process -FilePath "$script:Root\build\native\WiiCompiled.exe" -WorkingDirectory "$script:Root\build\native" `
            -RedirectStandardOutput "$LogPath.out" -RedirectStandardError $LogPath -PassThru
    } finally { foreach ($k in $Env.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue } }
}

function Stop-Game($p) {
    if ($p -and -not $p.HasExited) {
        $null = $p.CloseMainWindow(); Start-Sleep -Seconds 5
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
}

# Screenshot of the game window, optionally downscaled (PNG). Racy by nature: retries once.
function Take-Shot([string]$Out, [int]$Div = 2) {
    New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
    $job = Start-Job -ScriptBlock { param($r, $o) Set-Location $r; powershell -File "$r\tools\screenshot.ps1" -OutFile $o } -ArgumentList $script:Root, $Out
    if (-not (Wait-Job $job -Timeout 40)) { Remove-Job $job -Force; return $false }
    Receive-Job $job | Out-Null; Remove-Job $job -Force
    if ($Div -gt 1 -and (Test-Path $Out)) {
        python -c "
from PIL import Image
im=Image.open(r'$Out').convert('RGB'); im.resize((im.width//$Div,im.height//$Div)).save(r'$Out')"
    }
    return (Test-Path $Out)
}

# Parses the newest "[aim] ..." line of a log into a hashtable of key -> raw value.
function Read-AimLine([string]$LogPath, [int]$Last = 1) {
    $lines = Select-String -Path $LogPath -Pattern '^\[aim\] t=' | Select-Object -Last $Last
    foreach ($l in $lines) {
        $h = @{}
        foreach ($m in [regex]::Matches($l.Line, '(\S+?)=(\([^)]*\)|\S+)')) { $h[$m.Groups[1].Value] = $m.Groups[2].Value }
        $h
    }
}
