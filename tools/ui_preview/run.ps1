# Builds and runs the offline options-menu preview (no game, GPU or window needed).
#   pwsh tools/ui_preview/run.ps1 [-Width 1920] [-Height 1080] [-Background build/screenshots/rfix2s.png] [-Out build/screenshots] [-Script "open wait 30 shot display"]
# Output: <Out>/ui_<label>.png (menu composited over -Background). Scratch is build/uitest, removed at the end.
param(
    [int]$Width = 1920,
    [int]$Height = 1080,
    [string]$Background = "build/screenshots/rfix2s.png",
    [string]$Out = "build/screenshots",
    [string]$Prefix = "ui",
    [string]$Script = "open wait 40 shot display",
    [switch]$Keep
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
Set-Location $root
$scratch = "build/uitest"
New-Item -ItemType Directory -Force $scratch | Out-Null
$sdl = "build/native/_deps/sdl3_prebuilt-src/x86_64-w64-mingw32"
$imgui = "build/native/_deps/imgui-src"
$inc = @("runtime/include", "aurora-main/include", $imgui, "$sdl/include", "runtime/third_party/toml11", "runtime/third_party") | ForEach-Object { "-I$_" }
$flags = @("-std=c++20", "-O1", "-w", "-DNOMINMAX", "-DTARGET_PC", "-DSDL_MAIN_HANDLED", "-c")
$srcs = @("tools/ui_preview/ui_preview.cpp", "tools/ui_preview/stubs.cpp", "runtime/src/settings_overlay.cpp", "runtime/src/game_ui.cpp",
    "runtime/src/controls_menu.cpp", "runtime/src/controller_mapping_wizard.cpp", "runtime/src/ui_font_data.cpp",
    "$imgui/imgui.cpp", "$imgui/imgui_draw.cpp", "$imgui/imgui_tables.cpp", "$imgui/imgui_widgets.cpp")
$jobs = foreach ($s in $srcs) {
    $o = "$scratch/" + [IO.Path]::GetFileNameWithoutExtension($s) + ".o"
    if ((Test-Path $o) -and ((Get-Item $o).LastWriteTime -gt (Get-Item $s).LastWriteTime) -and ($s -like "$imgui*" -or $s -like "*ui_font_data*")) { continue }
    Start-Job -ScriptBlock { param($s, $o, $flags, $inc, $wd) Set-Location $wd; & clang++ @flags @inc $s -o $o 2>&1 } -ArgumentList $s, $o, $flags, $inc, $root
}
if ($jobs) { $jobs | Wait-Job | Receive-Job | Out-String | Write-Host }
& clang++ (Get-ChildItem "$scratch/*.o").FullName "$sdl/lib/libSDL3.dll.a" -lole32 -lshell32 -luuid -o "$scratch/ui_preview.exe" 2>&1 | Write-Host
Copy-Item "$sdl/bin/SDL3.dll" $scratch -Force
$args2 = @($Width, $Height, "$scratch/$Prefix") + ($Script -split ' ')
& "$scratch/ui_preview.exe" @args2
New-Item -ItemType Directory -Force $Out | Out-Null
python "$PSScriptRoot/compose.py" "$scratch" $Prefix $Background $Out
if (-not $Keep) { Remove-Item "$scratch/*.rgba" -ErrorAction SilentlyContinue }
