param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$Source = "ready",
    [string]$Output = "dist",
    [string]$Python = "python"
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$sourcePath = Join-Path $root $Source
$outputPath = Join-Path $root $Output
$cleanVersion = $Version.TrimStart("v")
$productName = "SinAndPunishmentRecomp-$cleanVersion-Windows-x64"
$stagePath = Join-Path $outputPath $productName
$archivePath = Join-Path $outputPath "$productName.zip"

foreach ($required in @("WiiCompiled.exe", "recomplauncher.json", "LICENSE", "wii_bootstrap")) {
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
# Copy only known release assets. Loose keys and foreign tools never enter staging.
$nativePath = Join-Path $root "build/native"
$runtimeDlls = Get-ChildItem $nativePath -File -Filter "*.dll" | Select-Object -ExpandProperty Name
$allowed = @("WiiCompiled.exe", "LICENSE", "wii_bootstrap", "Play.bat", "initial_pipeline_cache.db") + @($runtimeDlls)
Get-ChildItem $sourcePath -Force | Where-Object {
    $_.Name -in $allowed -and
    $_.Name -notmatch "^(sl\.(interposer|common|dlss|dlss_g|pcl|reflex)|nvngx_dlss(g)?)\.dll$"
} | ForEach-Object {
    Copy-Item $_.FullName $stagePath -Recurse -Force
}
$achievementNotices = Join-Path $nativePath "recomp_achievements_licenses"
if (Test-Path -LiteralPath $achievementNotices) {
    Copy-Item -LiteralPath $achievementNotices -Destination $stagePath -Recurse
}

Copy-Item (Join-Path $root "recomplauncher.json") (Join-Path $stagePath "recomplauncher.json") -Force
Copy-Item (Join-Path $root "third_party\disc_codecs") (Join-Path $stagePath "disc_codec_licenses") -Recurse

$exePath = Join-Path $stagePath "WiiCompiled.exe"
$hash = (Get-FileHash $exePath -Algorithm SHA256).Hash
@"
Sin & Punishment: Star Successor - recomp
Version : $cleanVersion
System  : Windows x64
Exe     : WiiCompiled.exe $((Get-Item $exePath).Length) bytes
SHA-256 : $hash

Stage module images are generated from the player's own copy during setup.
RecompLauncher stores settings, saves, caches and imported game data under its managed recompdata folder.
Manual launches use the normal per-user data folder unless --data is supplied.
"@ | Set-Content (Join-Path $stagePath "BUILD-INFO.txt")

& $Python (Join-Path $root "tools\release_audit.py") `
    --source (Join-Path $root "build\native") `
    --package $stagePath `
    --modules (Join-Path $root "build\modules") `
    --require-launcher-clean
if ($LASTEXITCODE -ne 0) {
    throw "release audit failed"
}

Compress-Archive -Path (Join-Path $stagePath "*") -DestinationPath $archivePath -CompressionLevel Optimal
Write-Output "created launcher release: $archivePath"
