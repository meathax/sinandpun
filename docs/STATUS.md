# Sin & Punishment: Star Successor recomp - status

Last reconciled 2026-11-20. This file records current repository evidence only. The single authoritative completion
checklist is in `docs/REMAINING.md`; `docs/PLAN-NEXT.md` is the ordered plan for closing it.

## Verified milestones

| ID | Milestone | State | Evidence |
|----|-----------|-------|----------|
| M0 | Source translation | DONE | main DOL and `stg00`-`stg07` translate; translator baseline recorded as 767 passed, 0 failed, 8 skipped |
| M1 | Guest WPAD connection | DONE | guest handshake logs show the virtual Wii Remote/Nunchuk, EEPROM and extension setup, report mode 0x33 then 0x37, and continuous input reports |
| M2 | First rendered frame | DONE | `build/screenshots/m2-first-frame.png` is a rendered title frame; DVD reads are present in the corresponding run |
| M3 | Isolated stage-module execution | DONE (isolated runs) | all eight modules loaded and presented gameplay in the stage table below; stages 6 and 7 are no longer open |
| M3b | Repeated module swapping | DONE (scripted QA) | `autopilot-swap3`: stage order `[1, 2, 1]`, two normal scene-exit transitions, two unlink/relink pairs, no fatal marker, 0.38 MiB/min maximum log growth |
| M4 | Input feature and options UI | PARTIAL | virtual right-stick aiming and the Nunchuk path run through the guest; title and pause OPTIONS both open the recomp overlay; physical-device and persistence coverage remains open |
| M5 | End-to-end validation | OPEN | A->B->A, full playthrough, save/quit/reload, real-device input/persistence, audible output, and performance/fidelity still need runs |

## Stage-module evidence

Runs used scripted idle input and prove module loading/rendering, not completion of the stage.

| Autostage id | Linked module | Presented frames | Screenshot evidence | Notes |
|---|---|---:|---|---|
| 0 | `stg00`, 1729 translated functions | 19,200 | `build/screenshots/m8a.png` | "The Basics" tutorial and gameplay |
| 1 | `stg01`, 3050 functions | 17,400 | `build/screenshots/stage1.png`, `build/screenshots/rfix1s.png` | bridge gameplay and cutscene |
| 2 | `stg02`, 2773 functions | 18,000 | `build/screenshots/stage2.png`, `build/screenshots/rfix2s.png` | underwater gameplay and HUD; a longer isolated run reached 290 s |
| 3 | `stg05`, 3643 functions | 17,400 | `build/screenshots/stage3.png` | stage id 3 maps to `stg05` |
| 4 | `stg04`, 2223 functions | 18,000 | `build/screenshots/stage4.png` | `build/stage_matrix.txt` records the link and frame count |
| 5 | `stg03`, 2164 functions | 17,400 | `build/screenshots/stage5.png`, `build/screenshots/rfix5s.png` | desert bike gameplay; stage id 5 maps to `stg03` |
| 6 | `stg06`, 4896 functions | 9,000 | `build/screenshots/g6_90s.png` | lava level, HUD, enemies |
| 7 | `stg07`, 2775 functions | 9,000 | `build/screenshots/g7_90s.png` | space stage |

`build/qa/autopilot-swap3` proves `A -> B -> A` through the guest scene-exit path. The run reached `[1, 2, 1]`,
unlinked `stg01`, linked `stg02`, unlinked `stg02`, linked `stg01`, continued for 151 seconds, and produced no fatal
or missing-target marker. `qa_report.py` measured 0.38 MiB/min maximum log growth.

## Other verified evidence

- Final integrated local clean build: configure 50 s, full build 222 s, exit 0. `build/clean/WiiCompiled.exe` is
  132,228,608 bytes with SHA-256 `09B487635ADB7360ED2DE5D7A24E1BE9D3DBE2BAB9FB74FC89EAEDA6382E28FF`.
- The clean tree passed 15/15 native tests. Translator tests passed 767, failed 0, skipped 8. Seven QA-tool tests passed,
  `qa_acceptance.py self-test` passed, and all eight modules report zero actionable coverage gaps.
- `ready/` was regenerated from that clean executable and passed `tools/release_audit.py` with zero errors and zero
  warnings. Its packaged smoke boot rendered `build/screenshots/ready-final.png` and exited cleanly with code 0.
- The title/pause options integration is verified by `build/screenshots/nav2_sheet.png` and
  `build/screenshots/pa2_sheet.png`. Closing the overlay also returns the stock page correctly
  (`build/screenshots/pa3_sheet.png`).
- `SIN_AUDIO_LOG=1` observed non-silent PCM for 103-112 of 150 logged seconds in stages 6 and 7, up to 8 voices. This
  proves mixer activity, not audible device output.
- NAND save files are created and a slot can show progress. It does not prove save -> quit -> reload.
- Scripted stick directions are observed in guest input reports and in-game pointer behavior. It does not prove a
  physical gamepad, Classic Controller acceptance, mouse arbitration/remapping, or settings persistence after restart.
- Synthetic Classic Controller acceptance is verified in `autopilot-classic-hot-swap`: the guest reports `dev=2`, accepts
  extension encryption, reads calibration, hot-swaps, and continues at 60.0 average guest FPS with no fatal marker.
  This is not physical-controller acceptance.
- Model-join interpolation was changed to coefficient interpolation so non-indexed pieces preserve coincident
  endpoints. The 150 s cutscene/gameplay replay completed without renderer/fatal errors. User/reference visual
  acceptance remains required.
- Terminal orthographic GUI is recomposited with nearest-neighbor after 3D upscaling. The FSR run logged
  `2432x1244 -> 3840x2160`, and its `Play as Kachi` file-select GUI remained crisp in
  `build/qa/autopilot-hud-fsr/s62.png`. Physical visual sign-off remains required.

## Remaining work

Do not duplicate or reinterpret the list here. The canonical open checklist is `docs/REMAINING.md`:

1. Full playthrough.
2. Save -> quit -> reload.
3. Real-device input and settings persistence.
4. Audio audibility.
5. Performance and fidelity.
6. Final source/evidence hygiene after the physical/manual gates settle.

## Debug switches and tools

- `SIN_CALLTRACE=1`, `SIN_CALLPROF=1`, `SIN_TRACE_ARGS`, `SIN_WATCHDOG_SECS`.
- `SIN_AUTOSTAGE=<0..7>` and `SIN_AUTOSTAGE_SEQ=a,b,c`.
- `SIN_AUDIO_LOG=1`.
- `SIN_VREMOTE_SCRIPT` for deterministic input; script field `@u/v` gives an absolute pointer position.
- `SIN_OPEN_OPTIONS_AT`, `SIN_OPEN_OPTIONS`, `SIN_MENU_AUTOCLOSE`, `SIN_MENU_HOOK_LOG`, `SIN_MENU_HOOK`.
- `tools/run_game.ps1`, `tools/shot_stats.py`, `tools/thread_dump.py`, `tools/calltrace_sym.py`.

## Build

After translating/generated code is current and the hardened helper has settled:

```powershell
powershell -File tools\clean_build.ps1 -Keep
dotnet test translator
```

The helper is intended to support an explicit `-ToolchainBin`, `MKW_TOOLCHAIN_BIN`, the managed local toolchain, or
tools on `PATH`. With `-Keep`, the output is under `build\clean`; without it, the executable is copied to
`build\WiiCompiled-clean.exe` and the temporary tree is removed. The hardened helper completed the current clean
checkpoint successfully.

Point `dvd_root` in `UserData/Config.toml` at the extracted `R2VE01` disc root containing `sys/` and `files/`.
No game data is included. License: GPL-3; the translator and runtime are forked from Wiicompiled.
