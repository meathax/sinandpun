param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$Source = "ready",
    [string]$Output = "dist"
)

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$sourcePath = Join-Path $root $Source
$outputPath = Join-Path $root $Output
$cleanVersion = $Version.TrimStart("v")
$productName = "SinAndPunishmentRecomp-$cleanVersion-Windows-x64"
$stagePath = Join-Path $outputPath $productName
$archivePath = Join-Path $outputPath "$productName.zip"

foreach ($required in @("WiiCompiled.exe", "recomplauncher.json", "LICENSE", "modules", "wii_bootstrap")) {
    if (-not (Test-Path (Join-Path $sourcePath $required))) {
        throw "release source is missing $required; refresh it with tools\update_ready.ps1 first"
    }
}

New-Item -ItemType Directory -Force $outputPath | Out-Null
if (Test-Path $stagePath) {
    throw "refusing to overwrite existing release staging directory: $stagePath"
}
if (Test-Path $archivePath) {
    throw "refusing to overwrite existing release archive: $archivePath"
}

New-Item -ItemType Directory $stagePath | Out-Null
Get-ChildItem $sourcePath -Force | Where-Object {
    $_.Name -notin @("UserData", "recompdata", "portable.txt", "licenses") -and
    $_.Name -notmatch "^(sl\.(interposer|common|dlss|dlss_g|pcl|reflex)|nvngx_dlss(g)?)\.dll$"
} | ForEach-Object {
    Copy-Item $_.FullName $stagePath -Recurse -Force
}

Get-Content (Join-Path $root "recomplauncher.json") -Raw |
    Set-Content (Join-Path $stagePath "recomplauncher.json") -NoNewline

$exePath = Join-Path $stagePath "WiiCompiled.exe"
$hash = (Get-FileHash $exePath -Algorithm SHA256).Hash
@"
Sin & Punishment: Star Successor - recomp
Version : $cleanVersion
System  : Windows x64
Exe     : WiiCompiled.exe $((Get-Item $exePath).Length) bytes
SHA-256 : $hash

This archive contains no Nintendo game data.
RecompLauncher stores settings, saves, caches and imported game data under its managed recompdata folder.
Manual launches use the normal per-user data folder unless --data is supplied.
"@ | Set-Content (Join-Path $stagePath "BUILD-INFO.txt")

python (Join-Path $root "tools\release_audit.py") `
    --source (Join-Path $root "build\native") `
    --package $stagePath `
    --modules (Join-Path $root "build\modules") `
    --require-launcher-clean
if ($LASTEXITCODE -ne 0) {
    throw "release audit failed"
}

Compress-Archive -Path (Join-Path $stagePath "*") -DestinationPath $archivePath -CompressionLevel Optimal
Write-Output "created launcher release: $archivePath"
