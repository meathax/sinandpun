# Remaining work to finish the Sin & Punishment: Star Successor recomp

Updated 2026-11-20. "Verified" means shown by a log or screenshot from this repo's own run. Everything else is unverified.

## A. Verified so far
- Boot to title with picture (`build/screenshots/rfixTs.png`), real guest WPAD handshake, stage modules 0-5 load and run 17k-19k frames each (`stage*.png`, `rfix*.png`).
- Top-left rectangle fixed (GXSetViewport native + partial EFB texture copy semantics); title, cutscene, stages 2 and 5 clean.
- Audio mixer produces non-silent PCM (112 of 150 logged seconds, up to 8 voices).
- Translator tests were 767/0 earlier; controls unit tests pass (25).

## B. Open verification items (need a game run each)
1. Stages 6 and 7 load and run; table in `docs/STATUS.md`.
2. Stage swap A -> B -> A (module window re-link, ctors/dtors, no stale code set).
3. Sepia / game-over scene: re-check for any residual flat block after the rectangle fix.
4. Classic Controller extension accepted by the game; right stick aims; left stick moves; all buttons mapped (scripted sweep, evidence in STATUS.md).
5. Mouse as pointer, calibration, device arbitration; keyboard remap.
6. Save -> quit -> reload (NAND, sdat*.bin), HOME/quit flow, no zero-size slot file.
7. Audio audibility on the real output device; per-category volume (music/SFX/voice) support or removal of dead sliders.
8. THP/video: none on disc (verified statically); cutscenes are in-engine.
9. Full playthrough of each stage including bosses, game over/continue, credits; no fatal errors; no unbounded log growth.

## C. Work items
1. Donor cleanup (Mario Kart / Retro Rewind / online): see `docs/DONOR-CLEANUP.md` (agent output).
2. In-game options menu: styled like the game, reachable from OPTIONS on the main menu and from the pause menu; replaces F10 (UI in `settings_overlay.cpp`, hook in `hle/game_menu_hook.cpp`, notes in `docs/MENU-HOOKS.md`).
3. Remaining donor-address hazards: GX data pointers in `gx_internal.h` / `gx_init.cpp` (Sin gxData ptr at 0x804B5B50, data 0x8043A030, FIFO obj 0x80439FB0), `boot_race.cpp` native at 0x8072624C, `MusicAttenuation`.
4. Performance: profile with `SIN_WATCHDOG_SECS` / frame-time overlay; hold 60 fps at 4K; check shader/pipeline compile hitches on first draws; module code-set overlay cost.
5. Visual fidelity: compare against known-good emulator output for bloom/glare chain, depth of field, sepia/colour filters, fog (`GXSetFogRangeAdj`), widescreen aspect handling.
6. Robustness: missing indirect targets (jump tables), `module_coverage.py` residual 2 false positives, `InvokeIndirectJump` coverage across modules, OSFatal/crash artifacts, log size caps.
7. Clean from-scratch build with only documented commands; translator tests; `git status` clean; nothing pushed.
8. Docs: README, STATUS.md stage table, acceptance checklist evidence, license note (GPL-3, forked from Wiicompiled; no game data included).
