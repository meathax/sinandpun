# GOAL — Sin & Punishment: Star Successor (R2VE01) PC recomp

## Target (user-mandated 2026-11-19)
Fully complete 1:1 recomp of the original game, with exactly one feature change:
**full dual-analog Xbox One style controller support — right stick for aiming,
with a really nice controller setup UI and great right-analog aim feel.**

Everything else must match the original (60 Hz logic tick, original behavior).
Optional later (docs/TOOLING.md): interpolated uncapped framerate, high internal
resolution with nearest-filtered HUD — only after 1:1 + dual-analog is complete.

## Non-negotiables
- Right stick aims (synthesized absolute Wii-Remote IR pointer at the WPAD/KPAD
  layer, BlackKnight technique; deadzone, sensitivity, response curve settings).
- Left stick = Nunchuk movement. Aim must feel great: smoothing/ramp curves,
  configurable, no lost pointer state, no sensor-bar fiction exposed to the user.
- 1:1 game behavior otherwise: main.dol + all 8 stg RSO modules translated,
  original assets read from `orig/R2VE01/files` via DVD HLE.

## Milestone ladder (state as of 2026-11-19)
1. [x] main.dol translates (11,169 funcs, 0 unsupported insns) — generated/functions
2. [x] stg00-07 linked flat @ 0x80800000 (tools/rso_link.py), 0 unresolved imports
3. [x] Translator default test baseline green (767/0/8)
4. [x] First native compile of the generated tree (compile-only pass + full link)
5. [x] Runtime fork builds end-to-end: build/native/WiiCompiled.exe links with
   all 11,165 translated functions + aurora/SDL3/Dawn (d48a113)
   - music_attenuation.cpp: SMTC monitoring degrades without C++/WinRT headers
   - recomp.yml output root moved to generated/ (donor runtime CMake contract)
6. [ ] **Sin product glue (IN PROGRESS)** — replace MKWii base_product/game glue,
   retarget native registrations (MKWii PAL addrs -> R2VE01), DVD HLE over
   orig/R2VE01/files, first-boot wiring
7. [ ] Module translation support (drop Program.cs module guard; module image at
   base + per-module maps; per-module code registration + dispatch)
8. [ ] Locate game RSO loader (RSOStaticLocateObject 0x8020D990 xref) -> place
   modules at 0x80800000 window or HLE the loader
9. [ ] Boot to title (first pixel), then gameplay smoke test per stage module
10. [ ] Dual-analog aim implementation + controller setup UI (the one feature)
11. [ ] Polish: aim feel tuning, full playthrough validation vs original

## Known gaps carried
- __OSDBJump `bla 0x60` out-of-RAM call has no target (debugger stub; ignore until it matters)
- 4 missing extabindex relocs per stg module (CW exception tables; benign for recomp)

## Working rules
- Scratch in build/, clean per global disk rule; no game data committed (orig/ untracked)
- Translator changes: dotnet test translator/Translator.sln -c Release must stay green
- Lesson DB entries after each verified fix
