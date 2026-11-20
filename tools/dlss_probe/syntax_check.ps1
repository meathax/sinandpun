# clang++ -fsyntax-only on the aurora files touched by the DLSS work, with the flags of the real build
# (taken from build/native/build.ninja). Does not run cmake/ninja and writes nothing.
param([string[]]$Files = @('lib/aurora.cpp', 'lib/webgpu/gpu.cpp', 'lib/webgpu/upscale.cpp', 'lib/webgpu/dlss.cpp', 'lib/webgpu/motion_flow.cpp', 'lib/webgpu/dlss_stub.cpp', 'lib/gfx/frame.cpp', 'lib/gfx/encoding.cpp', 'lib/window.cpp'))
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$ninja = Get-Content build/native/build.ninja
$ref = $ninja | Select-String -Pattern 'build aurora-build/CMakeFiles/aurora_core.dir/lib/webgpu/gpu.cpp.obj:' | Select-Object -First 1
$idx = $ref.LineNumber
$defines = ($ninja[$idx] -replace '^\s*DEFINES = ', '')
$flags = ($ninja[$idx + 2] -replace '^\s*FLAGS = ', '')
$includes = ($ninja[$idx + 3] -replace '^\s*INCLUDES = ', '')
$cc = 'C:\Users\meath\.local\share\retcomm\toolchains\cmake-clang-v1\latest\bin\clang++.exe'
$extra = '-isystem third_party/nvidia/streamline/include'
$failed = 0
foreach ($f in $Files) {
    $cmd = "$cc -fsyntax-only $defines $flags $includes $extra aurora-main/$f"
    $out = cmd /c "$cmd 2>&1"
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL $f"; $out | Select-Object -First 30; $failed++ } else { Write-Host "ok   $f" }
}
exit $failed
