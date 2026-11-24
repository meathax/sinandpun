param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$Source = "ready",
    [string]$Output = "dist",
    [string]$Python = "python",
    [string]$BuildDirectory = "build/native"
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
foreach ($required in @("LICENSE")) {
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
$nativePath = Join-Path $root $BuildDirectory
$runtimeDlls = @("SDL3.dll", "webgpu_dawn.dll", "dxcompiler.dll", "dxil.dll", "libc++.dll", "libpng16.dll", "libunwind.dll", "libzlib1.dll")
foreach ($name in (@("sinandpun.exe", "dsp_coef.bin", "initial_pipeline_cache.db") + $runtimeDlls)) {
    Copy-Item -LiteralPath (Join-Path $nativePath $name) -Destination $stagePath
}
New-Item -ItemType Directory (Join-Path $stagePath "wii_bootstrap") | Out-Null
Copy-Item -LiteralPath (Join-Path $root "runtime/assets/wii/shared2") -Destination (Join-Path $stagePath "wii_bootstrap/shared2") -Recurse
Copy-Item -LiteralPath (Join-Path $root "LICENSE") -Destination $stagePath
New-Item -ItemType Directory (Join-Path $stagePath "licenses") | Out-Null
foreach ($notice in $graphicsNotices) {
    Copy-Item -LiteralPath (Join-Path $sourcePath "licenses/$notice") -Destination (Join-Path $stagePath "licenses")
}
foreach ($file in $graphicsFiles) {
    Copy-Item -LiteralPath (Join-Path $sourcePath $file.Name) -Destination (Join-Path $stagePath $file.Name)
}
$hdHud = Join-Path $sourcePath "hd_hud"
if (-not (Test-Path -LiteralPath $hdHud -PathType Container) -or
    (Get-ChildItem -LiteralPath $hdHud -Filter *.png).Count -eq 0) {
    throw "release source is missing the HD HUD pack: $hdHud (refresh it with tools\update_ready.ps1)"
}
Copy-Item -LiteralPath $hdHud -Destination (Join-Path $stagePath "hd_hud") -Recurse
$hdWorld = Join-Path $sourcePath "hd_world"
if (-not (Test-Path -LiteralPath $hdWorld -PathType Container) -or
    (Get-ChildItem -LiteralPath $hdWorld -Filter *.dds).Count -eq 0) {
    throw "release source is missing the HD world pack: $hdWorld (refresh it with tools\update_ready.ps1)"
}
Copy-Item -LiteralPath $hdWorld -Destination (Join-Path $stagePath "hd_world") -Recurse
$achievementNotices = Join-Path $nativePath "recomp_achievements_licenses"
if (Test-Path -LiteralPath $achievementNotices) {
    Copy-Item -LiteralPath $achievementNotices -Destination $stagePath -Recurse
}

Copy-Item (Join-Path $root "recomplauncher.json") (Join-Path $stagePath "recomplauncher.json") -Force
Copy-Item (Join-Path $root "third_party\disc_codecs") (Join-Path $stagePath "disc_codec_licenses") -Recurse

& $Python (Join-Path $root "tools\release_audit.py") `
    --source $nativePath `
    --package $stagePath `
    --modules (Join-Path $root "build\modules") `
    --require-launcher-clean
if ($LASTEXITCODE -ne 0) {
    throw "release audit failed"
}

Compress-Archive -Path (Join-Path $stagePath "*") -DestinationPath $archivePath -CompressionLevel Optimal
Write-Output "created launcher release: $archivePath"
