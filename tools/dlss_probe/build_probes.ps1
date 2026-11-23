# Builds the DLSS probes into build/tmp_dlss/ (run from anywhere). Needs the staged SDK in third_party/nvidia and the
# Dawn prebuilt of the game build tree (build/native/_deps/dawn_prebuilt-src).
param([string[]]$Only = @('probe_sr', 'probe_dawn_share', 'probe_e2e'))
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$cc = 'C:\Users\meath\.local\share\retcomm\toolchains\cmake-clang-v1\latest\bin\clang++.exe'
$dawn = 'build/native/_deps/dawn_prebuilt-src'
New-Item -ItemType Directory -Force build/tmp_dlss | Out-Null
$sl = '-Ithird_party/nvidia/streamline/include'
if ($Only -contains 'probe_sr') {
    & $cc -std=gnu++20 -O1 -DNOMINMAX -w $sl tools/dlss_probe/probe_sr.cpp -o build/tmp_dlss/probe_sr.exe -ld3d12 -ldxgi -ldxguid -lole32 -static
}
if ($Only -contains 'probe_dawn_share') {
    & $cc -std=gnu++20 -O1 -DNOMINMAX -DWEBGPU_DAWN -w $sl -isystem "$dawn/include" tools/dlss_probe/probe_dawn_share.cpp -o build/tmp_dlss/probe_dawn_share.exe "$dawn/lib/webgpu_dawn.lib" -ld3d12 -ldxgi -ldxguid -lole32
}
if ($Only -contains 'probe_e2e') {
    & $cc -std=gnu++20 -O1 -DNOMINMAX -DWEBGPU_DAWN -w $sl -Ithird_party/amd/FidelityFX-SDK/Kits/FidelityFX/api/include -Ithird_party/amd/FidelityFX-SDK/Kits/FidelityFX/upscalers/include -isystem "$dawn/include" tools/dlss_probe/probe_e2e.cpp aurora-main/lib/webgpu/dlss.cpp aurora-main/lib/webgpu/motion_flow.cpp aurora-main/lib/webgpu/reflex.cpp -o build/tmp_dlss/probe_e2e.exe "$dawn/lib/webgpu_dawn.lib" -lole32 -luuid
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
foreach ($f in 'webgpu_dawn.dll', 'dxil.dll', 'dxcompiler.dll', 'libc++.dll', 'libunwind.dll') {
    if (Test-Path "ready/$f") { Copy-Item "ready/$f" build/tmp_dlss/ -Force }
}
Write-Host 'probes built in build/tmp_dlss (run from the repo root; delete build/tmp_dlss when done)'
