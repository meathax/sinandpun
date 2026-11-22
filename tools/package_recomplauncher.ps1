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

$graphicsFiles = (Get-Content (Join-Path $PSScriptRoot "graphics_runtime.json") -Raw | ConvertFrom-Json).PSObject.Properties
foreach ($file in $graphicsFiles) {
    $path = Join-Path $sourcePath $file.Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path).Hash -ne $file.Value) {
        throw "Required graphics runtime file is missing or differs from the SDK: $path"
    }
}
foreach ($required in @("WiiCompiled.exe", "recomplauncher.json", "LICENSE", "wii_bootstrap")) {
    if (-not (Test-Path (Join-Path $sourcePath $required))) {
        throw "release source is missing $required; refresh it with tools\update_ready.ps1 first"
    }
}
$graphicsNotices = @("amd-3rdpartynotice.md", "amd-fidelityfx-sdk.md", "amd-fsr1.txt",
    "nvidia-3rd-party-licenses.md", "nvidia-license.txt", "nvidia-nvngx_dlss.license.txt", "nvidia-reflex.license.txt")
foreach ($notice in $graphicsNotices) {
    if (-not (Test-Path -LiteralPath (Join-Path $sourcePath "licenses/$notice") -PathType Leaf)) {
        throw "release source is missing graphics notice: $notice"
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
$allowed = @("WiiCompiled.exe", "LICENSE", "wii_bootstrap", "Play.bat", "dsp_coef.bin", "initial_pipeline_cache.db") + @($runtimeDlls)
Get-ChildItem $sourcePath -Force | Where-Object {
    $_.Name -in $allowed -and $_.Name -notmatch "^(sl\.|nvngx_|amd_fidelityfx_)"
} | ForEach-Object {
    Copy-Item $_.FullName $stagePath -Recurse -Force
}
New-Item -ItemType Directory (Join-Path $stagePath "licenses") | Out-Null
foreach ($notice in $graphicsNotices) {
    Copy-Item -LiteralPath (Join-Path $sourcePath "licenses/$notice") -Destination (Join-Path $stagePath "licenses")
}
foreach ($file in $graphicsFiles) {
    Copy-Item -LiteralPath (Join-Path $sourcePath $file.Name) -Destination (Join-Path $stagePath $file.Name)
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
