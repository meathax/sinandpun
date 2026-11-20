# GOAL - Sin & Punishment: Star Successor (R2VE01) PC recomp

## Target

Complete a 1:1 static recompilation of the original Wii game to native x86-64, with one deliberate feature change:
full dual-analog gamepad support. The right stick aims, the left stick moves, and the in-game controller setup is
straightforward to use.

Everything else must preserve original behavior, including the 60 Hz game loop, assets, menus, stages, saves, audio, and
progression. Optional later features such as uncapped presentation or higher internal resolution are outside the
completion target and must not mask 1:1 regressions.

## Verified baseline

- The main DOL and all eight stage modules translate and the runtime links.
- The game boots to a rendered title screen.
- The guest WPAD path connects the virtual remote.
- All eight stage modules link and render in isolated idle-player runs; stages 6 and 7 are verified.
- The game-styled recomp options overlay opens from both the title and pause OPTIONS pages.
- Audio produces non-silent mixed PCM; save files are created.
- The hardened `tools/clean_build.ps1` completed a local clean runtime configure/build and all 15 native tests.
  This is workstation-local evidence, not a promise of cross-machine or package-independent reproducibility.

## Definition of done

The recomp is complete only when the canonical checklist in `docs/REMAINING.md` is fully verified with run evidence.
`docs/COMPLETION-PLAN.md` is the exhaustive execution plan, `docs/REMAINING.md` is the live open checklist,
and `docs/STATUS.md` records evidence.

In particular, completion still requires:

1. A full playthrough through bosses, game-over/continue, and credits with no fatal errors or unbounded logs.
2. Save -> quit -> reload validation.
3. Real-device input and settings-persistence validation.
4. Audibility validation on a real output device.
5. Performance and visual/behavioral fidelity validation against original/reference behavior.

## Rules

- No game data is committed. `orig/`, `generated/`, and `build/` remain local evidence only.
- Do not claim success from compilation, code inspection, scripted setup, or another workstream's pending QA alone.
- Preserve strict translation/test behavior and the 1:1 baseline.
- Record concrete logs or screenshots in `docs/STATUS.md` before removing an item from the canonical list.
