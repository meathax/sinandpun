param(
    [string]$BuildDir = "build\clean",
    [string]$ToolchainBin = "",
    [switch]$Keep           # keep the build tree (default: delete it afterwards, keep only the exe)
)
# Fresh configure + full build of the runtime into a separate directory, using the same toolchain as build\native.
# Prerequisite: the translator chain has been run (generated\build_shards exists); see docs/STATUS.md "Build".
# Usage (repo root): powershell -File tools\clean_build.ps1
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if (-not $ToolchainBin) { $ToolchainBin = $env:MKW_TOOLCHAIN_BIN }
if (-not $ToolchainBin) {
    $managed = Join-Path $HOME ".local\share\retcomm\toolchains\cmake-clang-v1\latest\bin"
    if (Test-Path $managed) { $ToolchainBin = $managed }
}

function Resolve-Tool([string]$Name) {
    if ($ToolchainBin) {
        $candidate = Join-Path $ToolchainBin "$Name.exe"
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path.Replace('\', '/') }
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source.Replace('\', '/') }
    throw "Required tool '$Name' not found. Pass -ToolchainBin or set MKW_TOOLCHAIN_BIN."
}

$cmake = Resolve-Tool "cmake"
$clang = Resolve-Tool "clang"
$clangxx = Resolve-Tool "clang++"
$ninja = Resolve-Tool "ninja"
$windres = Resolve-Tool "x86_64-w64-mingw32-windres"

if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
$sw = [Diagnostics.Stopwatch]::StartNew()
& $cmake -S runtime -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER="$clang" -DCMAKE_CXX_COMPILER="$clangxx" `
    -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_RC_COMPILER="$windres" `
    -DMKW_X86_CPU_PROFILE=v3 2>&1 | Select-Object -Last 6
$configureExit = $LASTEXITCODE
"configure: $([int]$sw.Elapsed.TotalSeconds)s (exit $configureExit)"
if ($configureExit -ne 0) { throw "CMake configure failed with exit $configureExit" }

& $cmake --build $BuildDir --parallel 30 2>&1 | Tee-Object -FilePath "$BuildDir\build_out.txt" |
    Select-String -Pattern ' error |FAILED|CMake Error' | Select-Object -First 20
$buildExit = $LASTEXITCODE
"build: $([int]$sw.Elapsed.TotalSeconds)s total"
if ($buildExit -ne 0) { throw "CMake build failed with exit $buildExit" }

$exe = Join-Path $BuildDir "sinandpun.exe"
if (Test-Path $exe) {
    $h = (Get-FileHash $exe -Algorithm SHA256).Hash
    "exe: $exe  $((Get-Item $exe).Length) bytes  sha256 $h"
} else {
    throw "Build exited successfully but did not produce $exe"
}
if (-not $Keep -and (Test-Path $exe)) {
    Copy-Item $exe "build\sinandpun.exe" -Force
    Remove-Item -Recurse -Force $BuildDir
    "build tree removed; exe copied to build\sinandpun.exe"
}
