# Stage the Windows renderer headers before configuring CMake, and the pinned release DLLs.
$ErrorActionPreference = "Stop"
$thirdParty = Join-Path (Split-Path $PSScriptRoot -Parent) "third_party"
$sl = Join-Path $thirdParty "nvidia/streamline"
$required = @("include/sl.h", "license.txt", "3rd-party-licenses.md", "bin/x64/nvngx_dlss.license.txt",
              "bin/x64/sl.interposer.dll", "bin/x64/sl.common.dll", "bin/x64/sl.dlss.dll", "bin/x64/nvngx_dlss.dll")
if ($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $sl $_) -PathType Leaf) }) {
    $version = "v2.14.16"
    $scratch = New-Item -ItemType Directory (Join-Path ([IO.Path]::GetTempPath()) ("sin-streamline-" + [guid]::NewGuid()))
    $zip = Join-Path $scratch "sdk.zip"
    Invoke-WebRequest "https://github.com/NVIDIA-RTX/Streamline/releases/download/$version/streamline-sdk-$version.zip" -OutFile $zip
    Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $scratch "sdk")
    $header = Get-ChildItem (Join-Path $scratch "sdk") -Recurse -File -Filter sl.h | Select-Object -First 1
    if (-not $header) { throw "Streamline SDK has no sl.h in $scratch" }
    $sdk = $header.Directory.Parent.FullName
    New-Item -ItemType Directory -Force "$sl/include", "$sl/bin/x64" | Out-Null
    Copy-Item "$sdk/include/*" "$sl/include" -Recurse -Force
    foreach ($file in $required | Where-Object { $_ -ne "include/sl.h" }) {
        Copy-Item -LiteralPath (Join-Path $sdk $file) -Destination (Join-Path $sl $file) -Force
    }
    if ($scratch.Parent.FullName -eq [IO.Path]::GetTempPath().TrimEnd('\') -and $scratch.Name -like "sin-streamline-*") {
        Remove-Item -LiteralPath $scratch.FullName -Recurse -Force
    }
}
function Get-AmdSdk($name, $tag, $commit, $paths) {
    $dir = Join-Path $thirdParty "amd/$name"
    if (-not (Test-Path -LiteralPath "$dir/.git")) {
        $owner = if ($name -eq "FidelityFX-SDK") { "GPUOpen-LibrariesAndSDKs" } else { "GPUOpen-Effects" }
        git clone --config core.autocrlf=true --branch $tag --depth 1 --filter=blob:none --sparse "https://github.com/$owner/$name.git" $dir
        if ($LASTEXITCODE) { throw "Could not fetch $name" }
    }
    if ((git -C $dir rev-parse HEAD) -ne $commit) { throw "Expected $name at $commit in $dir" }
    if ($paths) {
        git -C $dir sparse-checkout add @paths
        if ($LASTEXITCODE) { throw "Could not stage $name runtime and headers" }
    }
}
Get-AmdSdk "FidelityFX-SDK" "v2.4.0" "7539ae8b6568ad29ed78a753404351daf3b5cca7" @("Kits/FidelityFX/api", "Kits/FidelityFX/upscalers/include", "Kits/FidelityFX/signedbin", "docs")
Get-AmdSdk "FidelityFX-FSR" "v1.0.2" "a21ffb8f6c13233ba336352bdff293894c706575" @()
