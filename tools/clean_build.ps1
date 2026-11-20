param(
    [string]$BuildDir = "build\clean",
    [switch]$Keep           # keep the build tree (default: delete it afterwards, keep only the exe)
)
# Fresh configure + full build of the runtime into a separate directory, using the same toolchain as build\native.
# Prerequisite: the translator chain has been run (generated\build_shards exists); see docs/STATUS.md "Build".
# Usage (repo root): powershell -File tools\clean_build.ps1
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$tc = "C:/Users/meath/.local/share/retcomm/toolchains/cmake-clang-v1/latest/bin"
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
$sw = [Diagnostics.Stopwatch]::StartNew()
& "$tc/cmake.exe" -S runtime -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER="$tc/clang.exe" -DCMAKE_CXX_COMPILER="$tc/clang++.exe" `
    -DCMAKE_MAKE_PROGRAM="$tc/ninja.exe" -DCMAKE_RC_COMPILER="$tc/x86_64-w64-mingw32-windres.exe" `
    -DMKW_X86_CPU_PROFILE=v3 2>&1 | Select-Object -Last 6
"configure: $([int]$sw.Elapsed.TotalSeconds)s (exit $LASTEXITCODE)"
& "$tc/cmake.exe" --build $BuildDir --parallel 30 2>&1 | Tee-Object -FilePath "$BuildDir\build_out.txt" |
    Select-String -Pattern ' error |FAILED|CMake Error' | Select-Object -First 20
"build: $([int]$sw.Elapsed.TotalSeconds)s total"
$exe = Join-Path $BuildDir "WiiCompiled.exe"
if (Test-Path $exe) {
    $h = (Get-FileHash $exe -Algorithm SHA256).Hash
    "exe: $exe  $((Get-Item $exe).Length) bytes  sha256 $h"
} else { "NO EXE PRODUCED" }
if (-not $Keep -and (Test-Path $exe)) {
    Copy-Item $exe "build\WiiCompiled-clean.exe" -Force
    Remove-Item -Recurse -Force $BuildDir
    "build tree removed; exe copied to build\WiiCompiled-clean.exe"
}
