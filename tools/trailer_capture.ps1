param(
    [Parameter(Mandatory)] [string]$Name,
    [int]$Stage = -1,            # -1 = normal boot (title screen); else SIN_AUTOSTAGE stage
    [int]$A = 0,
    [int]$B = 0,
    [double]$Lead = 25,          # seconds after launch before recording starts
    [double]$Record = 60,        # seconds recorded
    [string]$Character = "0",
    [int]$Seed = 7,
    [switch]$NoBot,
    [string]$OutDir = "build\trailer\raw",
    [string[]]$Env = @()
)
# Capture-only: records the primary 3840x2160 display at 60 fps (lossless-ish HEVC NVENC) while the
# game runs with the trailer bot driving both seats. Usage (repo root):
#   powershell -File tools\trailer_capture.ps1 -Name s1 -Stage 1 -Lead 20 -Record 60
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$ffmpeg = "D:\Arcade\AI\ffmpeg-8.1.2-full_build\bin\ffmpeg.exe"
New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-Process sinandpun -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

$vars = @{}
if (-not $NoBot) { $vars["SIN_TRAILER_BOT"] = "1"; $vars["SIN_TRAILER_BOT_GOD"] = "1"; $vars["SIN_TRAILER_BOT_SEED"] = "$Seed"; $vars["SIN_VIRTUAL_REMOTE"] = "1" }
if ($Stage -ge 0) {
    $vars["SIN_AUTOSTAGE"] = "$Stage"; $vars["SIN_AUTOSTAGE_A"] = "$A"; $vars["SIN_AUTOSTAGE_B"] = "$B"
    $vars["SIN_AUTOSTAGE_CHARACTER"] = $Character
}
foreach ($e in ($Env | ForEach-Object { $_ -split ',' })) { if ($e) { $k, $v = $e -split '=', 2; $vars[$k] = $v } }
foreach ($k in $vars.Keys) { Set-Item -Path "Env:$k" -Value $vars[$k] }

$log = Join-Path $OutDir "$Name.game.log"
$p = Start-Process -FilePath "$root\build\native\sinandpun.exe" -WorkingDirectory "$root\build\native" `
    -RedirectStandardOutput "$log.out" -RedirectStandardError $log -PassThru
Start-Sleep -Milliseconds ([int]($Lead * 1000))
$video = Join-Path $OutDir "$Name.mkv"
& $ffmpeg -hide_banner -loglevel error -y -f lavfi -i "gfxcapture=window_exe=sinandpun.exe:capture_cursor=0:max_framerate=60,hwdownload,format=bgra,fps=60" `
    -t $Record -c:v hevc_nvenc -preset p4 -tune hq -rc constqp -qp 14 $video
"ffmpeg exit $LASTEXITCODE"
$null = $p.CloseMainWindow()
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
foreach ($k in $vars.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
Get-Item $video | Select-Object Name, Length
