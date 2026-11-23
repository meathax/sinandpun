param(
    [string]$Name = "save-home-audio",
    [string]$SaveScript,
    [string]$ReloadScript,
    [int]$SaveSeconds = 180,
    [int]$ReloadSeconds = 120,
    [string]$Exe = "",
    [string]$NandDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if (-not $SaveScript) { throw "SaveScript is required and must finish with a deterministic HOME-menu quit path." }
if (-not $ReloadScript) { throw "ReloadScript is required and must finish with a deterministic HOME-menu quit path." }
if ($SaveSeconds -le 0 -or $ReloadSeconds -le 0) { throw "phase timeouts must be positive" }
if (-not $Exe) { $Exe = "$root\build\native\sinandpun.exe" }
if (-not $NandDir) { $NandDir = "$root\build\native\UserData\NAND\title\00010004\52325645\data" }
$tool = "$root\tools\qa_acceptance.py"
$outDir = "$root\build\qa\$Name"
$lock = "$root\build\agent.lock"
$acceptExe = "$root\build\native\SinAcceptance.exe"
New-Item -ItemType Directory -Force $outDir | Out-Null
if (-not (Test-Path $Exe)) { throw "game executable does not exist: $Exe" }
if (-not (Test-Path $tool)) { throw "acceptance tool does not exist: $tool" }
if (Get-Process SinAcceptance -ErrorAction SilentlyContinue) {
    throw "another SinAcceptance process is already running"
}

$gotLock = $false
for ($i = 0; $i -lt 3000; $i++) {
    try {
        New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null
        Set-Content "$lock\owner.txt" "qa-acceptance-$Name $(Get-Date -f HH:mm:ss)"
        $gotLock = $true
        break
    } catch {
        Start-Sleep 1
    }
}
if (-not $gotLock) { throw "could not acquire build\agent.lock; owner: $(Get-Content "$lock\owner.txt" -ErrorAction SilentlyContinue)" }

function Invoke-AcceptancePython {
    param([string[]]$Arguments)
    & python $tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "qa_acceptance.py failed: $($Arguments -join ' ')" }
}

function Run-Phase {
    param(
        [string]$Phase,
        [string]$Script,
        [int]$TimeoutSeconds
    )
    $outLog = "$outDir\$Phase-out.log"
    $errLog = "$outDir\$Phase-err.log"
    Remove-Item $outLog,$errLog -Force -ErrorAction SilentlyContinue
    $vars = @{
        SIN_VIRTUAL_REMOTE = "1"
        SIN_VREMOTE_SCRIPT = $Script
        SIN_AUDIO_LOG = "1"
        SIN_NAND_LOG = "1"
    }
    $original = @{}
    foreach ($name in $vars.Keys) {
        $original[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
        [Environment]::SetEnvironmentVariable($name, $vars[$name], "Process")
    }
    try {
        $process = Start-Process -FilePath $acceptExe -WorkingDirectory "$root\build\native" `
            -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit()
            throw "$Phase exceeded ${TimeoutSeconds}s without a guest-requested clean exit"
        }
        $process.Refresh()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            ErrorLog = $errLog
        }
    } finally {
        foreach ($name in $original.Keys) {
            [Environment]::SetEnvironmentVariable($name, $original[$name], "Process")
        }
    }
}

try {
    Copy-Item $Exe $acceptExe -Force
    $before = "$outDir\before.json"
    $saved = "$outDir\saved.json"

    Invoke-AcceptancePython @("snapshot", "--nand-dir", $NandDir, "--output", $before)
    $saveRun = Run-Phase "save" $SaveScript $SaveSeconds
    Invoke-AcceptancePython @("verify-runtime", "--log", $saveRun.ErrorLog, "--exit-code", "$($saveRun.ExitCode)")
    Invoke-AcceptancePython @("verify-save", "--before", $before, "--nand-dir", $NandDir, "--output", $saved)

    $reloadRun = Run-Phase "reload" $ReloadScript $ReloadSeconds
    Invoke-AcceptancePython @(
        "verify-reload", "--before", $before, "--saved", $saved,
        "--nand-dir", $NandDir, "--log", $reloadRun.ErrorLog,
        "--exit-code", "$($reloadRun.ExitCode)"
    )

    "PASS: artifacts are in $outDir" | Set-Content "$outDir\summary.txt"
} finally {
    Remove-Item -Recurse -Force $lock -ErrorAction SilentlyContinue
    Remove-Item $acceptExe -Force -ErrorAction SilentlyContinue
}
