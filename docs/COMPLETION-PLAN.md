# Sin & Punishment: Star Successor - completion plan

Status: authoritative execution plan  
Updated: 2026-11-20

This document defines every task required to call the `R2VE01` PC static recompilation complete. A compile, isolated
stage smoke test, scripted input test, or screenshot is evidence for only the layer it exercised; none substitutes for
the end-to-end checks below.

## 1. Completion definition

The project is complete when all of these are true:

1. The game is reproducibly translated and built from the documented local inputs.
2. The main DOL and every stage module remain fully covered, with no unresolved guest targets.
3. All stages, bosses, transitions, cutscenes, game-over/continue flows, and credits are playable without fatal errors.
4. A complete playthrough preserves the original 60 Hz behavior, pacing, progression, visuals, and audio.
5. Right-stick aiming and left-stick movement work through the virtual Wii Remote and KPAD paths, with a usable setup UI.
6. Saves survive quit and relaunch without corruption or missing progress.
7. Audio is mixed, submitted to the host output, and confirmed audible and correct on a real output device.
8. Module swaps, repeated quits, and long runs are stable; logs do not grow without bound.
9. GUI/HUD/text is nearest-neighbor enlarged without FSR, DLSS, bilinear, or sharpening damage, while 3D may use the
   configured upscaler.
10. Character model joins have no interpolation-induced cracks in cutscenes or gameplay.
11. The release package contains no game data and passes the release audit.
12. Every completion claim has a command, log, screenshot, or human test record in `docs/STATUS.md`.

Optional uncapped presentation, frame interpolation, FSR, DLSS, motion estimation, VRR, and later enhancements are not
substitutes for the 1:1 baseline. They must be tested after the baseline is correct.

## 2. Evidence and workflow rules

- Keep translated output, game data, and runtime evidence under ignored paths: `generated/`, `orig/`, and `build/`.
- Never close a task from source inspection alone.
- Every gate must have:
  - the exact command or player action;
  - the build hash or run identity;
  - relevant log lines;
  - screenshots where the claim is visual;
  - a pass/fail result from the matching test or report tool;
  - the known limits of the run.
- Keep a cold boot and a warm run distinct.
- Keep isolated stage rendering distinct from stage completion.
- Keep scripted input distinct from physical-device acceptance.
- Keep non-silent PCM distinct from real-device audibility.
- A failed gate remains open even if a later, unrelated run succeeds.
- Update `docs/STATUS.md` first, then remove the item from `docs/REMAINING.md`.

## 3. Current verified foundation

These are foundations, not reasons to skip later gates:

- The main DOL and `stg00`-`stg07` translate.
- The runtime links and boots to a rendered title screen.
- The guest WPAD/Bluetooth handshake connects the virtual remote.
- All eight modules have independently loaded and rendered.
- `A -> B -> A` RSO unlink/relink has passed its deterministic QA harness and remains a repeatable regression.
- Title and pause OPTIONS reach the recomp settings overlay.
- Translator and native unit tests exist.
- Audio produces non-silent mixed PCM and has backend-queue counters.
- Save files exist and NAND unit tests cover preservation/error behavior.
- Character-transform interpolation and pixel-nearest GUI presentation have dedicated regressions and visual runs.

## 4. Freeze and prove the build inputs

### 4.1 Verify source identity

- [ ] Confirm `orig/R2VE01/sys/main.dol` matches the SHA-256 in `projects/r2ve01/recomp.yml`.
- [ ] Record the MAME-style equivalent for this project: exact DOL hash, module hashes, translator revision, and toolchain.
- [ ] Verify all eight local `stg00.rso`-`stg07.rso` inputs still match the selected disc image.
- [ ] Confirm no game data or generated translation is tracked.

### 4.2 Regenerate the full translation

Run the full documented chain whenever native registrations, translation hooks, maps, or module linking changes:

```powershell
$t = 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
dotnet $t emit-native-index --project projects/r2ve01/recomp.yml --out runtime/native-index.json
dotnet $t translate-recursive 0x80006310 --project projects/r2ve01/recomp.yml --threads 12 --output-metadata generated/base_translation_output.json
dotnet $t generate-data-init --project projects/r2ve01/recomp.yml
dotnet $t emit-build-shards --project projects/r2ve01/recomp.yml
```

For every `stg00`-`stg07` module, complete and document:

```powershell
python tools/rso_link.py orig/R2VE01/files/stgNN.rso orig/R2VE01/sys/main.dol orig/R2VE01/files/gs2.sel 0x97DC0000 build/modules/stgNN
python tools/module_seeds.py stgNN --write --prune-jt
dotnet $t translate-recursive 0x80006310 --project projects/r2ve01/stgNN.yml --threads 12 --output-metadata build/generated-stgNN/base_translation_output.json
python tools/module_coverage.py
python tools/module_shards.py
```

Acceptance:

- `dotnet test translator`: zero failures and no newly skipped tests.
- `python tools/module_coverage.py`: zero actionable uncovered entries.
- Every translated module has a recorded function count, link address, image size, and hash.
- The translator still rejects unsupported instructions rather than silently weakening strictness.

### 4.3 Build from a clean state

```powershell
powershell -File tools\clean_build.ps1 -Keep
ctest --test-dir build\clean --output-on-failure -j 8
```

Record:

- toolchain path and versions;
- configure/build exit codes;
- executable path, size, and SHA-256;
- clean test count;
- boot screenshot and fatal-line count.

The script must fail on configure/build failure or a missing executable and must not depend on a personal hardcoded
user path. Local success is reproducible evidence for this workstation, not a claim about every machine.

## 5. Close all implementation regressions

### 5.1 Controller and focus correctness

- [ ] Prove window focus loss invalidates absolute mouse coordinates.
- [ ] Prove keyboard/mouse/gamepad pulses are not stranded or stuck.
- [ ] Prove no stale pointer state survives pause, overlay, focus loss, scheme change, or device arbitration.
- [ ] Verify all control paths use the same persisted binding model.

Regression:

- native focus-state test;
- synthetic SDL focus-loss/gain run;
- actual window focus loss/regain with a physical mouse.

### 5.2 Module lifecycle

- [ ] Validate repeated `A -> B -> A` and longer mixed sequences through the guest flow task.
- [ ] Validate every module unload/relink clears only the current code set.
- [ ] Validate prolog/epilog execution, BSS reset, absolute pointers, and indirect dispatch after each return.
- [ ] Run at least one sequence of all eight modules in both forward and reverse order.

Gate:

```powershell
python tools/qa_report.py build/qa/<run> --sequence 1,2,1 --no-errors --max-growth-mib-per-min 8
```

The report must see `RSOUnLinkList`, the next `RSOLinkListFixed`, the target stage loaded, and live gameplay after the
return to A. No stale dispatch, missing target, or stuck state is allowed.

### 5.3 Sound-slot / game-over crash

- [ ] Prove the game-over readiness entry hook is registered in generated code.
- [ ] Reproduce the previous slot-5 unloaded handle condition.
- [ ] Verify the game-owned jingle loader is called with a complete/restored CPU context.
- [ ] Verify loader success makes slot state/handle/data valid.
- [ ] Verify loader failure installs a safe non-dereferenceable path rather than calling freed memory.
- [ ] Exercise game over in at least stages 1 and 2 and after a module swap.

Acceptance:

- no `InvokeIndirectCpu` target derived from freed slot memory;
- no crash artifact;
- the jingle loads/plays or fails safely with an explicit log;
- continue and result flow remains usable.

### 5.4 Rendering: model joins

- [ ] Validate cutscene and gameplay character joins with frame interpolation both off and on.
- [ ] Compare the same motion sequence at 60/117/120 Hz presentation modes.
- [ ] Confirm independently skinned model pieces preserve coincident vertices.
- [ ] Check normals and lighting at shoulder, waist, wrist, ankle, hair/face, and weapon joins.
- [ ] Check indexed and non-indexed GX matrix paths.

Acceptance:

- no interpolation-induced gaps, wires, black lines, or detached slivers;
- original 60 Hz frames remain unchanged by the interpolation fix;
- no added wobble, scale/shear, culling regression, or normal discontinuity.

### 5.5 Rendering: GUI/HUD/text scaling

- [ ] Keep GUI source sampling faithful to GX semantics at internal resolution.
- [ ] Scale terminal orthographic/XFB GUI after 3D resampling.
- [ ] Use nearest-neighbor for GUI presentation.
- [ ] Suppress DLSS viewport jitter from GUI passes.
- [ ] Keep 3D FSR/DLSS/MetalFX behavior independent from GUI recomposition.
- [ ] Preserve aspect, letterbox/pillarbox, crop, pointer coordinates, subtitles, and alpha.

Acceptance matrix:

| Output | Upscaler | Required GUI evidence |
|---|---|---|
| 1920x1080 | off | crisp 3x source HUD when integer scaling applies |
| 3840x2160 | off | no bilinear glow/softness or new color fringe |
| 3840x2160 | FSR 1 | GUI remains nearest while 3D is visibly FSR-scaled |
| 3840x2160 | DLSS/DLAA, where available | GUI bypasses temporal/spatial reconstruction |
| 1440p/ultrawide | off | nearest pixels, correct aspect and safe area |

At non-integer ratios, pixels must still map by nearest-neighbor; exact uniform integer blocks are impossible unless the
output permits an integer scale.

## 6. Input and controls acceptance

### 6.1 Synthetic integration

Automate:

- pointer right/left/up/down and clamp edges;
- left-stick movement in every direction;
- every action mapping;
- pointer and Classic extension hot-swap;
- Nunchuk and Classic encryption/key exchange;
- device arbitration and held-button cancellation;
- mouse absolute and relative modes;
- keyboard and mouse remapping;
- focus loss, pause, overlay, and menu input blocking;
- settings save and reset.

Capture `[aim]`, `[vmouse]`, and Bluetooth logs. Compare host intent to guest KPAD/WPAD state and to the visible reticle.

### 6.2 Physical-device acceptance

This cannot be closed by a bot. With a real controller:

- [ ] Confirm right-stick direction and range.
- [ ] Tune deadzone, saturation, response curve, smoothing, acceleration, precision, and recenter behavior.
- [ ] Confirm left-stick movement independently of aim.
- [ ] Press and hold every mapped button, trigger, shoulder, face, d-pad, pause, minus, and home input.
- [ ] Hot-plug and switch between controllers.
- [ ] Verify a d-pad-only pad can navigate all menus.
- [ ] Verify physical mouse absolute/relative behavior at multiple resolutions.
- [ ] Verify Classic Controller behavior, including file select and game-side control-priority notices.
- [ ] Restart and confirm remaps/settings persist; reset them and confirm defaults return.

## 7. Save, NAND, and quit lifecycle

### 7.1 Save path

- [ ] Snapshot all `sdat*.bin` files before a controlled save point.
- [ ] Save progress through the real game flow.
- [ ] Verify exactly the intended slot changed and remains 1600 bytes.
- [ ] Verify `banner.bin` and unrelated NAND data remain intact.
- [ ] Verify no `*.nandsafe.tmp` remains after clean exit.

### 7.2 Quit and HOME

Exercise:

- window close;
- in-game settings quit;
- OSReturnToMenu / Wii Menu;
- OSRestart / Reset;
- OSShutdownSystem / power-off where the game exposes it.

Acceptance:

- exit code 0;
- explicit guest-exit log naming the native path;
- GX and AX workers stop before memory reset;
- no crash popup/artifact.

### 7.3 Reload

- [ ] Relaunch the executable.
- [ ] Prove IOS/NAND opens and reads the changed slot.
- [ ] Confirm the title menu displays the saved progress.
- [ ] Resume and reach the next expected state.
- [ ] Repeat with the other valid slots and with a deliberately aborted/shadow write fixture.

Use `tools/qa_acceptance.py` for machine-verifiable slot, log, and SDL checks. It proves host/runtime save behavior; a real
game save point and displayed progress still require an in-game run.

## 8. Audio acceptance

### 8.1 Software path

Require:

- AI DMA callbacks near 333 blocks/second at 32 kHz;
- AX frames and voices active;
- non-zero peak and non-silent PCM;
- `SDL_OpenAudioDeviceStream` opened successfully;
- `SDL_PutAudioStreamData` accepted non-silent samples;
- no output queue drops, callback stalls, unknown AX commands, or undefined guest accesses.

Run:

- title music;
- each stage;
- bosses;
- cutscenes;
- stage transitions;
- pause/resume;
- game-over/continue;
- credits.

### 8.2 Audible hardware path

This requires a human listening test:

- [ ] music, effects, dialogue/voice, and Wii-Remote-related paths are audible at expected levels;
- [ ] channel balance is correct;
- [ ] no crackle, clipping, drift, pop, underrun, or long gap occurs;
- [ ] master volume and mute work;
- [ ] dead or unsupported category sliders are either implemented correctly or removed;
- [ ] direct AI ordering remains correct in stereo/headphones/speakers.

PCM counters cannot close this item.

## 9. Full playthrough and progression

Run the real game from a clean or documented save:

1. Boot/title/file-select.
2. Stage 0/tutorial and its completion state.
3. Every main stage and sub-area.
4. Every boss and defeat sequence.
5. Every save checkpoint and progress update.
6. Every in-engine cutscene and transition.
7. Pause, restart, options, and resume.
8. Game over, continue, and retry.
9. Stage/module A->B and repeated module reuse.
10. Final stage, ending cutscenes, and credits.
11. Return to title and reload the completed save.

For each step record:

- stage and sub-stage;
- expected/observed state;
- representative screenshot;
- fatal/stuck/error lines;
- frame-time sample;
- audio observation;
- save hash before/after where applicable.

Do not call a stage complete from an idle bot or a forced stage load. The final playthrough requires active gameplay or a
documented assist/cheat that does not bypass progression code.

## 10. Performance and stability

### 10.1 Baseline 1:1 performance

Run menus, ordinary gameplay, bosses, module swaps, and cutscenes with:

- 60 Hz logic;
- frame interpolation off;
- output at native/stock resolution and at 4K;
- shaders/pipeline cache cold, then warm;
- audio enabled.

Acceptance:

- 60 guest frames/second under the tested hardware's normal load;
- no progressive slowdown;
- no speed or `vi_time` drift;
- no sustained audio underrun;
- no unbounded memory growth;
- first-draw hitches are measured and acceptable or removed.

### 10.2 Enhanced modes

After baseline passes:

- 120 Hz/frame interpolation;
- VRR/tearing;
- FSR 1;
- DLSS/DLAA where hardware is available;
- 4K and ultrawide;
- MSAA levels exposed by the UI.

These modes must not introduce model seams, HUD blur, stale HUD, depth/copy regressions, audio drift, or pointer offset.

Use `tools/qa_report.py` with explicit gates for resolution, average frame rate, minimum samples, log growth, and fatal
markers. Example:

```powershell
python tools/qa_report.py build/qa/<run> --require-resolution 3840x2160 --min-guest-avg 59.9 --min-perf-samples 120 --max-growth-mib-per-min 8 --no-errors
```

### 10.3 Long-run robustness

- [ ] Run at least one multi-hour soak across repeated scenes and modules.
- [ ] Stress pause/resume, options, focus loss, hot-plug, and overlay open/close.
- [ ] Bound log file growth and verify it remains diagnostic, not unbounded.
- [ ] Check crash-artifact generation with a controlled injected failure.
- [ ] Confirm no stale worker thread or file handle remains after repeated exits.

## 11. Visual and behavioral fidelity

Build a synchronized reference comparison from original hardware or a known-good emulator:

- framing, aspect, safe area, and 4:3 pixel aspect;
- text/font rendering and subtitle position;
- HUD, title, menus, and file-select UI;
- bloom/glare, depth of field, fog, color/sepia filters, and post effects;
- EFB/XFB copies, indirect/scissored draws, and alpha;
- lighting, normals, vertex colors, and model seams;
- cutscene timing and transitions;
- input latency and reticle movement;
- physics, enemy behavior, scripting, and stage timing;
- audio cues and soundtrack timing.

Differences require one of:

1. a code fix back to original behavior;
2. an intentional, documented input-only feature change;
3. a clearly recorded, approved limitation in `docs/HACKS.md` with a correction path.

## 12. Documentation, licensing, and hygiene

- [ ] Reconcile `README.md`, `GOAL.md`, `docs/STATUS.md`, `docs/REMAINING.md`, and this document.
- [ ] Remove stale "open", "pending", duplicate, or corrupted evidence references.
- [ ] Keep `HACKS.md` and `MAME_KLUDGES`-equivalent notes current if any approximation exists.
- [ ] Document all third-party runtime/translator credits and licenses.
- [ ] Confirm the project license and binary release obligations.
- [ ] Confirm no `orig/`, ISO/RVZ/WBFS/WAD, save, cache, log, or proprietary SDK/game asset enters the package.
- [ ] Keep personal credentials, Wi-Fi data, controller identifiers, and machine paths out of tracked files.
- [ ] Update build instructions and record the exact supported toolchain.

## 13. Release package gate

1. Run the complete clean build and every test suite.
2. Run the accepted end-to-end, save/reload, audio, input, visual, and performance gates.
3. Refresh the package:

```powershell
powershell -File tools\update_ready.ps1 -NoDlss
```

4. Audit it:

```powershell
python tools/release_audit.py --source build/native --package ready --modules build/modules
```

5. Require:

   - matching executable and DLL hashes;
   - all eight linked stage modules;
   - `LICENSE`;
   - valid `BUILD-INFO.txt`;
   - no game data;
   - no unexpected binaries;
   - portable `UserData` layout and correct Play script;
   - package smoke boot from a separate folder.

6. Archive the final evidence report with build hash, test counts, QA run names, screenshots, physical/human checks, and
   known limitations.

## 14. Final sign-off checklist

The project is complete only when every line is checked with evidence:

- [ ] Main DOL and all eight modules regenerate and translate strictly.
- [ ] Clean build and all automated tests pass.
- [ ] Module coverage has zero actionable gaps.
- [ ] Repeated A->B->A and all-module switching passes.
- [ ] Previous sound-slot game-over crash path is safe and verified.
- [ ] Model joins are correct with interpolation off and on.
- [ ] HUD/GUI/text remains pixel-nearest through native, FSR, and supported DLSS paths.
- [ ] Full playthrough reaches bosses, continue, and credits.
- [ ] Save -> quit -> reload restores progress.
- [ ] HOME/reset/exit paths terminate cleanly.
- [ ] Real gamepad, mouse, keyboard, Classic, remap, persistence, and reset are verified.
- [ ] Audio reaches and is correct on a real output device.
- [ ] 4K/baseline performance and long-run stability pass.
- [ ] Visual/behavioral reference comparison is accepted.
- [ ] Release package contains no game data and passes `release_audit.py`.
- [ ] `docs/STATUS.md` contains the final evidence and `docs/REMAINING.md` has no open blocker.
