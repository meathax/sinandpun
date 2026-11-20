# Performance work: shared rules for every agent

Several agents work in this repo at the same time. There is ONE build tree (`build/native`) and ONE game instance at a time.

## 1. Build/run lock (mandatory)

Before running cmake/ninja/dotnet OR launching `WiiCompiled.exe` (any copy), take the lock; release it right after, always (use try/finally):

```powershell
function Take-Lock($who) {            # waits up to 20 min
  $lock = 'D:\Arcade\AI\aCORES\Sin\build\agent.lock'
  for ($i = 0; $i -lt 1200; $i++) {
    try { New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null; Set-Content "$lock\owner.txt" "$who $(Get-Date -f HH:mm:ss)"; return }
    catch { Start-Sleep 1 }
  }
  throw "could not get the build lock; owner: $(Get-Content $lock\owner.txt -ErrorAction SilentlyContinue)"
}
function Release-Lock { Remove-Item -Recurse -Force 'D:\Arcade\AI\aCORES\Sin\build\agent.lock' -ErrorAction SilentlyContinue }
```

Hold the lock for one build or one measurement run only (max ~10 minutes), not while you are thinking or editing source.
Never kill a `WiiCompiled` process you did not start. If the lock is older than 30 minutes and no `WiiCompiled`/`ninja`/`cmake` process exists, it is stale: remove it.

## 2. Build

Plain C++/aurora edits: `& "C:/Users/meath/.local/share/retcomm/toolchains/cmake-clang-v1/latest/bin/cmake.exe" --build build/native --parallel 30`.
Native overrides / entry_hooks / manifest changes also need the translator chain (see docs/STATUS.md "Build").
The tree contains uncommitted work from other agents; if the build breaks in a file you did not touch, wait a few minutes and retry, or report it; do not "fix" other agents' files.

## 3. Benchmarks (identical for before/after)

Scripted run, pointer scheme, stages loaded with `SIN_AUTOSTAGE`. Per-second log: `SIN_PERF_LOG=1` (`[perf] fps= guest= present= cpu= ...`; the first 20 s are skipped).

```powershell
$script='32|0.3:A|10|0.21::-1/0|0.5|0.3:A|5|0.15::-1/0|0.3|0.19::0/-1|0.5|0.3:A|1.5|0.056::1/0|0.3|0.23::0/1|0.5|0.3:A|4|0.3:PLUS|4|0.3:A|4|0.3:A|4|0.04::-1/0|0.2|0.095::0/-1|0.5|0.3:A|4|0.3:A|4|0.3:A|4|0.3:A|120'
powershell -File tools\run_game.ps1 -Seconds 140 -Name perfX -Env "SIN_VIRTUAL_REMOTE=1,SIN_AUTOSTAGE=6,SIN_PERF_LOG=1,SIN_VREMOTE_SCRIPT=$script"
```

Standard scenes: stage 1 (mixed), stage 6 (lava, heaviest so far), stage 7 (space), plus a 60 s title/menu run. Always do a SECOND run of the same scene (pipeline/shader caches warm; first runs hitch on shader compiles which is a separate problem).
Report for each scene: samples, min, p1, median, avg `guest` fps, count of seconds below 55, CPU %. Screenshots: `tools\run_shots.ps1 -At "..." -Name x -Env "..."` (comma list in quotes; it resizes to *_s.png; copy files when `read` says not found, it is racy).
The baseline at the start of this effort: stage 1: avg 60.0, min 58.9, CPU ~41% of one core; stage 6 first run: min 21, p1 35 (4 seconds of dips = shader compiles), second run min 57, p1 58.9. Machine: i9-13900KF, RTX 4090, 4K borderless, resolution_multiplier 6 in the test config (check build/native/UserData/Config.toml).

## 4. Rules

- Guest game logic must stay at exactly 60 Hz and behave identically (no change to emulation accuracy). Verify `speed=1.000` / `vi_time=1.000` in the perf log if present.
- Measure before and after every change; keep only changes with a measurable benefit; revert your own change if it regresses or breaks something. Keep a log of what you tried (also the dead ends) in your notes file.
- Do not commit, push, checkout or reset. Do not delete other agents' files. Clean up your scratch (build/tmp_*), old `build/native/UserData/Logs/base_*` dirs older than your run, and unneeded screenshots.
- Add a lessons entry (`python C:/Users/meath/.claude/tools/lessons.py add ...`) for every verified finding.
- Re-read a file immediately before editing it: other agents edit settings_overlay.cpp, aurora lib files and runtime_config.h.
