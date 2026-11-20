$root = Split-Path $PSScriptRoot -Parent; Set-Location $root
$lock = "$root\build\agent.lock"; $got = $false
for ($i = 0; $i -lt 3000 -and -not $got; $i++) { try { New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null; Set-Content "$lock\owner.txt" "qa-build $(Get-Date -f HH:mm:ss)"; $got = $true } catch { Start-Sleep 1 } }
if (-not $got) { "no lock"; exit 1 }
try { & "C:/Users/meath/.local/share/retcomm/toolchains/cmake-clang-v1/latest/bin/cmake.exe" --build build/native --parallel 30 2>&1 | Select-Object -Last 25 } finally { Remove-Item -Recurse -Force $lock -ErrorAction SilentlyContinue }
