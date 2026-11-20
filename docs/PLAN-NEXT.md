# Plan to close the recomp

Updated 2026-11-20. The goal and acceptance boundary are in `GOAL.md`; evidence belongs in `docs/STATUS.md`.
`docs/REMAINING.md` is the one canonical remaining-task list. This document gives execution order only and does not
create additional completion claims.

## Evidence rules

- Do not close an item from compilation, code inspection, screenshots from an unrelated scenario, or another
  workstream's pending QA.
- Record the exact command/run, relevant fatal-free log lines, screenshot/log paths, and the limitation of the test in
  `docs/STATUS.md`.
- Keep `A -> B`, isolated stage runs, and scripted input distinct from `A -> B -> A`, a playthrough, and physical-device
  verification.
- `autopilot-swap3` verified `A -> B -> A`, and the hardened clean-build rerun is recorded in `docs/STATUS.md`.

## 1. Complete module-swap verification

Run a deterministic sequence that loads A, B, then A again through the guest stage-change path.

1. Preserve the unlink/link, ctor/dtor, code-set, and frame-count logs.
2. After returning to A, capture a gameplay screenshot and prove new module calls execute.
3. Fail the item on a crash, stale target, fatal log, stuck transition, or incorrect rendering.
4. Update only the first canonical item when this run is independently complete.

## 2. Validate progression and saves

Exercise one complete playthrough rather than a collection of isolated idle-stage runs:

1. Boot, title/options, stage entry, every boss, all cutscenes, game-over/continue, pause/quit, and credits.
2. At deliberate save points, quit the process and restart; verify the displayed slot and resumed progress.
3. Check `sdat*.bin` for nonzero, stable, usable data and include the HOME/quit path.
4. Record any fatal, stuck state, unexpected pacing, missing text/effect, or unbounded log.

## 3. Validate real devices and persistence

Repeat the input checks with actual hardware and the normal configuration path:

1. Right-stick aim direction/range/response, left-stick movement, every mapped button, pause/menu navigation, and
   device arbitration.
2. Mouse pointer input, keyboard remapping, gamepad remapping, and Classic Controller acceptance.
3. Restart the process and confirm changed settings persist; then verify reset-to-default.

Scripted sweeps may diagnose or reproduce issues, but they do not close physical-device verification.

## 4. Validate audio

Mixer activity is already proven. On a real output device:

1. Confirm music, SFX, and voice are audible and not severely distorted or missing.
2. Compare muting, volume, and any category sliders with original/intended behavior.
3. Test transitions, cutscenes, bosses, game over, and pause/resume for stuck or absent channels.

## 5. Validate performance and fidelity

1. Measure sustained frame time and 60 fps behavior during menus, ordinary gameplay, bosses, stage swaps, and
   first-time shader/pipeline creation.
2. Compare against known-good original/emulator reference for visuals, pacing, physics, effects, filters, fog,
   aspect handling, text, and cutscenes.
3. Keep optional uncapped/HID features from concealing failures in the baseline 1:1 path.

## 6. Final build and documentation hygiene

After the hardened helper and parent work have settled:

1. Run `powershell -File tools\clean_build.ps1 -Keep` and record the local toolchain, exit status, elapsed time,
   executable path/size/hash, and boot result.
2. Do not call the result cross-machine reproducible. It is proof for the tested local environment only.
3. Run `dotnet test translator`; retain the actual pass/fail/skip counts.
4. Recheck every `build/screenshots` path and inspect documents for control characters, stale "open" items, or
   competing checklists.
5. Keep `docs/REMAINING.md` authoritative and remove an item only when `docs/STATUS.md` contains its evidence.
