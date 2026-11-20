# Builds build\native under the shared agent lock (docs/PERF-AGENT-GUIDE.md). Usage: powershell -File tools\mouse_build.ps1
$root = Split-Path $PSScriptRoot -Parent
$lock = "$root\build\agent.lock"
$got = $false
for ($i = 0; $i -lt 1200; $i++) {
    try { New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null; Set-Content "$lock\owner.txt" "mouse-build $(Get-Date -f HH:mm:ss)"; $got = $true; break }
    catch { Start-Sleep 1 }
}
if (-not $got) { throw "no lock; owner: $(Get-Content $lock\owner.txt -ErrorAction SilentlyContinue)" }
try {
    Set-Location $root
    & "C:/Users/meath/.local/share/retcomm/toolchains/cmake-clang-v1/latest/bin/cmake.exe" --build build/native --parallel 30 2>&1 | Select-Object -Last 40
    "build exit: $LASTEXITCODE"
} finally {
    Remove-Item -Recurse -Force $lock -ErrorAction SilentlyContinue
}
