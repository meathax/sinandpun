# Sin & Punishment: Star Successor recomp - status

Last updated 2026-11-19 (end of the M0/M1/M2 session). Source of truth for the plan: `docs/PLAN-NEXT.md`.

## Milestones

| ID | Milestone | State | Evidence |
|----|-----------|-------|----------|
| M0 | Commit current work | DONE | commits `5479e53`, `8120dca`, `98f875d`, `7b8e3f4`; `git status` clean of source |
| M1 | Real guest WPAD connects the virtual remote | DONE | log: `Virtual Wii Remote + Nunchuk connected to the game (gamepad)`; guest handshake: EEPROM cal reads, extension init (0xA400F0/FB), Nunchuk ID `00 00 A4 20 00 00`, IR block writes, report mode 0x33 then 0x37, continuous 0x37 reports (`[bt] in id=0x37`) |
| M2 | First real frame | DONE | `build/screenshots/m2-first-frame.png`: 49,750 colours, 80.7% non-black, real title screen (Sin & Punishment: Star Successor logo + background); `[dvd] read ...` lines (fonts, texture.arc, comdat.arc, titdat.arc, stream/title.brstm, s01dat20.arc) |
| M3 | Stage modules (RSO loader + code-set switching) | NOT STARTED | translator emits stg00..07 trees but they are not wired into the runtime |
| M4 | Dual-analog aiming + setup UI | PARTIAL | pointer synthesis + scripted test input (`SIN_VREMOTE_SCRIPT`) verified numerically (see below); in-game reticle direction, feel tuning and the Controller UI are NOT done |
| M5 | Full playthrough validation | NOT STARTED | |

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

## Known problems / not done

* Title screen shows a pale rectangle top-left and no "press" text; right edge looks cut (window is 698x422, aspect
  handling unverified). Likely GX fidelity gaps (TEV/texture formats). Re-run `tools/gx_retarget_report.py`
  whenever a new screen is reached.
* The scripted A press did not advance the title screen within the run; input handling past the title is untested.
* M3 (stage modules) is not implemented at all. Translator test suite was not re-run this session
  (`dotnet test translator`); the translator gained the optional `loop_poll_functions` manifest field.
* NAND save files `sdat0..3.bin` do not exist on first boot (`IOS_Open FAILED` lines); save/load is untested.
* Audio: streaming/voices start (calltrace shows StrmSound/AXAcquireVoice) but audible output is unverified.
* Debug logging added this session is rate-limited; `[bt]`, `[trace]`, `[prof]` lines are diagnostics only.

## Debug switches and tools

* `SIN_CALLTRACE=1` writes `build/native/calltrace.txt` (first-seen guest call targets).
* `SIN_CALLPROF=1` (with CALLTRACE) prints the hottest call targets every 10 s (`[prof]`, `[prof-game]`).
* `SIN_TRACE_ARGS=hex+hex` logs r3..r6/lr on entry to listed targets (200 hits each).
* `SIN_WATCHDOG_SECS=N` writes a crash-style snapshot and host-RIP samples every N s (3 times).
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
```

Point `dvd_root` in `UserData/Config.toml` at the extracted disc (`orig/R2VE01/files`). License: GPL-3 (forked from
Wiicompiled). No game data is included.
