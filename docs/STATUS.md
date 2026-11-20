# Sin & Punishment: Star Successor recomp - status

Last updated 2026-11-20 (M3 stage modules working for stages 0-5; M4 code written, in-game verification pending). Source of truth for the plan: `docs/PLAN-NEXT.md`.

## Milestones

| ID | Milestone | State | Evidence |
|----|-----------|-------|----------|
| M0 | Commit current work | DONE | commits `5479e53`, `8120dca`, `98f875d`, `7b8e3f4`; `git status` clean of source |
| M1 | Real guest WPAD connects the virtual remote | DONE | log: `Virtual Wii Remote + Nunchuk connected to the game (gamepad)`; guest handshake: EEPROM cal reads, extension init (0xA400F0/FB), Nunchuk ID `00 00 A4 20 00 00`, IR block writes, report mode 0x33 then 0x37, continuous 0x37 reports (`[bt] in id=0x37`) |
| M2 | First real frame | DONE | `build/screenshots/m2-first-frame.png`: 49,750 colours, 80.7% non-black, real title screen (Sin & Punishment: Star Successor logo + background); `[dvd] read ...` lines (fonts, texture.arc, comdat.arc, titdat.arc, stream/title.brstm, s01dat20.arc) |
| M3 | Stage modules (RSO loader + code-set switching) | PARTIAL | all 8 modules prelinked at MEM2 window 0x97DC0000, translated per module (`tools/module_shards.py`), `RSOLinkListFixed`/`RSOUnLinkList` natives in `hle/os/os_rso.cpp`; stages 0-5 ran 17k-19k frames each with gameplay screenshots (`build/screenshots/stage*.png`); stages 6,7 and A->B->A swaps: see stage table |
| M4 | Dual-analog aiming + setup UI | CODE WRITTEN | aim curves/deadzones, Classic Controller + pointer schemes, keyboard/mouse/gamepad remap, tabbed settings overlay (`settings_overlay.cpp`, `controls_menu.cpp`, `input_map.h`, `aim_input.cpp`); `mkw_controls_tests` passes; stick direction verified in game earlier; Classic extension, mouse pointer, menu navigation NOT verified in game |
| M5 | Full playthrough validation | IN PROGRESS | top-left rectangle fixed (GXSetViewport native); audio mixer produces PCM; see checklist |

## What was wrong, in order (all recorded in the lessons DB)

1. `WPADInit` hung in `BTA_Init` (busy-poll): the BTE periodic alarm never fired because alarms were only pumped
   from the scheduler idle path. Fix: periodic alarm service on the guest-call path (`abi_bridge.cpp`,
   `dispatch.h`), plus the simulated decrementer `rfi` restores PPCMtdec's host snapshot (`os_context.cpp`).
2. The guest refused the remote (L2CAP result 3 / disconnect): `SCGet/SetBtDeviceInfoArray` natives were disabled
   donor addresses. Real Sin addresses `0x801B6BC0/BD0` (DINF) and `0x801B6BE0/BF0` (CmpDev) found by scanning
   `main.dol` for the `addi r4,0,0x461; addi r5,0,0x1C; b SCFindByteArrayItem` thunk.
3. VI retrace callbacks never ran: HLE ignored guest-memory slots `0x804B2410/0x804B240C` and guarded on a donor
   EGG `sSystem` address (`vi.cpp`).
4. The main loop body ran once: the game paces on the GP breakpoint interrupt (`GXEnableBreakPt` +
   `GXSetBreakPtCallback`, callback `fn_8003EB9C` sets the frame flag at `r13-32208`). Emulated in
   `hle/gx/gx_breakpoint.cpp`, delivered from the VI retrace service.
5. No DVD reads: the game's OS idle function (`0x800380D8`, via `OSSetIdleFunction`) IS the asset loader. It is a
   call-free spin loop and the SDK idle thread is never scheduled. Fix: translator option
   `translation.loop_poll_functions` (inserts `GuestLoopPoll(ctx)` at every basic-block entry of the listed
   functions) + `hle/os/os_idle.cpp` (host-owned fiber resumed from the scheduler idle loop).
6. Permanently black window: the donor startup cover waited on a Mario Kart strap-scene hook (auto-dismissed
   after 120 presented frames now), and `dynamic_aspect.cpp` wrote MKWii EGG records into Sin memory (disabled).
7. Black picture although draws were submitted: `GXSetProjection` (`0x801DAFE0`) and `GXFlush` (`0x801D6BB0`)
   natives were at disabled donor addresses.

## Controller (M4) measured so far

`SIN_VREMOTE_SCRIPT` (`seconds[:buttons[:rx/ry[:lx/ly]]]` segments joined by `|`; `/` for pairs) drives the
virtual remote deterministically. Log lines `[bt] pointer cam=(x,y) stick R=.. L=.. buttons=..`:

* stick R=(1,0)  -> camera x 512 -> 60 (clamped); R=(-1,0) -> x 60 -> 600
* stick R=(0,1)  -> camera y 384 -> 60; R=(0,-1) -> y 60 -> 480
* A pulse -> buttons=0x0008 (A bit) reaches the guest as input report 0x37
* the camera x/y are inverted relative to the stick by design (sensor-bar logic); whether the on-screen cursor moves
  the right way is NOT verified yet (needs a menu with a visible cursor).


## Stage module results (autostage run, 90-150 s each, scripted idle player; fatal = crash artifact or fatal log line)

| Autostage id | Module loaded (RSOLinkListFixed log) | Fatal | Frames presented | Screenshot evidence | Notes |
|---|---|---|---|---|---|
| 0 (normal menu path) | stg00, 1729 fns | none | 19,200 | uild/screenshots/m8a.png ("The Basics" tutorial, 60.1 fps) | tutorial, then gameplay |
| 1 | stg01, 3050 fns | none | 17,400 | stage1.png, ix1s.png (HUD, bridge), fix1s.png (cutscene) | |
| 2 | stg02, 2773 fns | none | 18,000 | stage2.png, fix2s.png (underwater gameplay + HUD) | |
| 3 | stg05, 3643 fns | none | 17,400 | stage3.png (sepia HUD), fix4s.png boss | autostage 3 loads stg05 (id != module number) |
| 4 | stg04, 2223 fns | none | 18,000 | no gameplay screenshot kept | run completed, see batch log uild/stage_matrix.txt |
| 5 | stg03, 2164 fns | none | 17,400 | stage5.png, fix5s.png (desert bike gameplay) | autostage 5 loads stg03 |
| 6 | stg06, 4896 fns | none | 9,000 | g6_90s.png (lava level, HUD, enemies) | |
| 7 | stg07, 2775 fns | none | 9,000 | g7_90s.png (space stage) | |

Audio mixer (SIN_AUDIO_LOG=1): 103-112 of 150 logged seconds had non-silent PCM blocks in stages 6 and 7 (up to 8 voices). Audibility on the real device is NOT verified.
Not yet tested: A -> B -> A module swap, boss/credits/continue flow, save -> quit -> reload, Classic extension / mouse / aim sweep in game, the tabbed options menu in game.
## Acceptance checklist (docs/PLAN-NEXT.md section 11) - evidence as of 2026-11-20

| Item | State | Evidence |
|---|---|---|
| Clean build from scratch with documented commands | PASS | `tools/clean_build.ps1`: fresh configure 34 s + full build 157 s total, exit 0; `build/clean/WiiCompiled.exe` 131,651,072 bytes, SHA-256 `9E11C5976188E15EA11B21480DAA93975A1F90BF990475310B41CD7D9C2F6ED5`; that exe boots to the title at 60 fps with 0 fatal lines (`build/screenshots/clean_boots.png`) |
| Translator tests pass | PASS | `dotnet test translator`: Passed 767, Failed 0, Skipped 8 (after the budget/entry_hooks translator changes) |
| Boot to title with picture and audio | PASS (audio: mixer only) | `rfixTs.png`, `clean_boots.png`; `SIN_AUDIO_LOG=1`: ~333 AX frames/s, non-silent PCM in 103-112 of 150 s; audibility on a real device not verified |
| All 8 stages load and play | PASS (idle player) | stage table above (8/8 modules link and render, `g6_90s.png`, `g7_90s.png`, `pz3_124s.png`, ...); module swap A -> B proven (`RSOUnLinkList: stg01 unlinked`, `RSOLinkListFixed: stg02`); A -> B -> A NOT proven: the second, forced swap crashed (see `docs/SWAP-CRASH-NOTES.md`); stage 2 alone ran 290 s without a crash |
| Right stick aims, left stick moves, buttons mapped, setup UI persists | PARTIAL | stick direction verified in game earlier (cursor follows stick); recomp options overlay opens from the game's title and pause OPTIONS (`nav2_sheet.png`, `pa2_sheet.png`); Classic Controller extension, mouse pointer, remapping with a real device and persistence across a restart NOT verified |
| Save/load works | NOT VERIFIED | save files are created; file 1 shows `A (STAGE 1 - ISA)` progress after runs; save -> quit -> reload not run |
| No fatal errors in a full playthrough, no unbounded log growth | NOT VERIFIED | stages 0-7 ran 90-290 s each with 0 fatals under an idle scripted player; no full playthrough |
| `git status` clean, committed, nothing pushed | PASS | no remote is configured; `git log` shows local commits only |

## Known problems / not done

* FIXED: top-left rectangle (native `GXSetViewport` at 0x801DB3B0 + partial EFB texture copies written 1:1 in aurora);
  title shows the full picture and "Press A" (`build/screenshots/rfixTs.png`). A sepia game-over scene was not re-checked.
* NOT VERIFIED: A -> B -> A stage swap (A -> B proven: `RSOUnLinkList: stg01 unlinked`, `RSOLinkListFixed: stg02`),
  Classic Controller extension accepted by the game, aim sweep, mouse pointer, keyboard/mouse/gamepad remapping with a
  real device, save -> quit -> reload, HOME/quit flow, audio audibility on a real output device, a full playthrough
  (bosses, credits), performance (frame-time) pass.
* Audio: AX/AI pipeline fixed (AI callback + donor DSP/AX addresses); 103-112 of 150 logged seconds are non-silent PCM
  (`SIN_AUDIO_LOG=1`). Per-category sliders were removed (they only worked for Mario Kart).
* Save files `sdat0..3.bin` are created on first boot; file 1 shows "STAGE 7 - ISA" progress after earlier runs.
* The game's own Options page stays alive under the recomp overlay; the hook presses B on the virtual remote when the
  overlay closes (`VirtualRemote::InjectCancel`, verified in `pa3_sheet.png`).
* Debug logging is rate-limited; `[bt]`, `[trace]`, `[prof]`, `[copylog]`, `[audio-dbg]` lines are diagnostics only.

## Debug switches and tools

* `SIN_CALLTRACE=1` writes `build/native/calltrace.txt` (first-seen guest call targets).
* `SIN_CALLPROF=1` (with CALLTRACE) prints the hottest call targets every 10 s (`[prof]`, `[prof-game]`).
* `SIN_TRACE_ARGS=hex+hex` logs r3..r6/lr on entry to listed targets (200 hits each).
* `SIN_WATCHDOG_SECS=N` writes a crash-style snapshot and host-RIP samples every N s (3 times).
* `SIN_GX_COPYLOG=<frame>` (+ `SIN_GX_COPYLOG_EVERY=<n>`) logs texture copies, viewport, scissor and draw state for 3 frames.
* `SIN_AUTOSTAGE=<0..7>` redirects the stage the game loads (after the menu run; the game stage id to module map is not identity: 3->stg05, 5->stg03). `SIN_AUDIO_LOG=1` logs AX/AI mixer stats per second. `SIN_DUMP_ON=<hex>` + `SIN_DUMP_EXIT=1` snapshot memory at first entry of a traced target and exit.
* `SIN_AUTOSTAGE_SEQ=a,b,c` (+`SIN_AUTOSTAGE_GAP=<s>`) swaps stages in sequence via the game's own stage-change path; `SIN_OPEN_OPTIONS_AT=<s>`, `SIN_OPEN_OPTIONS=1`, `SIN_MENU_AUTOCLOSE=<s>`, `SIN_MENU_HOOK_LOG=1`, `SIN_MENU_HOOK=0` drive/inspect the menu hook; script field `@u/v` places the pointer at screen fractions.
* `SIN_VIRTUAL_REMOTE=1` forces the virtual remote; `SIN_VREMOTE_SCRIPT=...` scripted input; `SIN_GX_TEST_RED=1`
  is an unreliable renderer probe (see lesson).
* `tools/run_game.ps1 -Env "A=1,B=2"` (ONE quoted string; `-Env` is split on commas), `tools/xref_up.py`,
  `tools/callees.py`, `tools/gx_retarget_report.py`, `tools/find_sc_funcs.py`, `tools/thread_dump.py`,
  `tools/calltrace_sym.py`, `tools/shot_stats.py`.

## Build

```
# full chain (needed after adding/changing/removing any native override or recomp.yml)
$t = 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
dotnet $t emit-native-index --project projects/r2ve01/recomp.yml --out runtime/native-index.json
dotnet $t translate-recursive 0x80006310 --project projects/r2ve01/recomp.yml --threads 12 --output-metadata generated/base_translation_output.json
dotnet $t generate-data-init --project projects/r2ve01/recomp.yml
dotnet $t emit-build-shards --project projects/r2ve01/recomp.yml
cmake --build build/native --parallel 30

# stage modules (after changing tools/rso_link.py, module maps or the translator): for each stgNN
python tools/rso_link.py orig/R2VE01/files/stgNN.rso orig/R2VE01/sys/main.dol orig/R2VE01/files/gs2.sel 0x97DC0000 build/modules/stgNN
python tools/module_seeds.py stgNN --write --prune-jt   # then keep only text-range + farjump_ map entries
dotnet $t translate-recursive 0x80006310 --project projects/r2ve01/stgNN.yml --threads 12 --output-metadata build/generated-stgNN/base_translation_output.json
python tools/module_coverage.py ; python tools/module_shards.py ; then the full chain above
```

Point `dvd_root` in `UserData/Config.toml` at the extracted disc (`orig/R2VE01/files`). License: GPL-3 (forked from
Wiicompiled). No game data is included.
