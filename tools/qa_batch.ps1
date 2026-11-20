param([int[]]$Stages = @(2,0,1,3,4,5,6,7), [int]$Seconds = 480, [string]$Tag = "b1")
Set-Location (Split-Path $PSScriptRoot -Parent)
foreach ($s in $Stages) { powershell -File tools\qa_run.ps1 -Name "${Tag}_st$s" -Seconds $Seconds -Stage $s -ShotEvery 120 -PauseEvery 110 -Seed (10 + $s) | Out-Null; python tools\qa_report.py "${Tag}_st$s" 40 | Out-File "build\qa\${Tag}_st$s\report.txt" }
