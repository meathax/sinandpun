param(
    [Parameter(Mandatory = $true)][ValidateSet("classic", "pointer")][string]$Scheme
)
# Sets [input] scheme in build\native\UserData\Config.toml (the virtual remote reads it at link time, so restart the game).
# Usage (repo root): powershell -File tools\set_scheme.ps1 -Scheme classic
$root = Split-Path $PSScriptRoot -Parent
$cfg = Join-Path $root "build\native\UserData\Config.toml"
if (-not (Test-Path $cfg)) { New-Item -ItemType File -Force $cfg | Out-Null }
$text = Get-Content $cfg -Raw
if ($null -eq $text) { $text = "" }
if ($text -match '(?ms)^\[input\]\s*$') {
    if ($text -match '(?m)^scheme\s*=') {
        $text = [regex]::Replace($text, '(?m)^scheme\s*=.*$', "scheme = `"$Scheme`"")
    } else {
        $text = [regex]::Replace($text, '(?m)^\[input\]\s*$', "[input]`nscheme = `"$Scheme`"")
    }
} else {
    $text = $text.TrimEnd() + "`n`n[input]`nversion = 1`nscheme = `"$Scheme`"`n"
}
Set-Content $cfg $text -NoNewline
Select-String $cfg -Pattern '^\[input\]|^scheme' | ForEach-Object { $_.Line }
