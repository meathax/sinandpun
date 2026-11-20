# Canonical remaining work

Updated 2026-11-20. This is the only remaining-task list for completion. Items are removed only after their named
evidence is recorded in `docs/STATUS.md`.

## Verified and closed

- Boot, first rendered frame, and the guest virtual-remote handshake.
- Isolated loading/rendering of all eight stage modules, including stages 6 and 7.
- Deterministic `A -> B -> A` module transitions, including unlink/relink and continued presentation after return.
- A full clean configure/build, automated test suites, zero actionable module-coverage gaps, and a release audit.
- Title and pause OPTIONS opening the game-styled recomp overlay, with correct close behavior.
- Mixed-audio production (not device audibility) and creation of save files.

## Open checklist

1. **Full playthrough**
   - Exercise all stages, bosses, cutscenes, game-over/continue, pause/quit, and credits.
   - Record no fatal errors, stuck states, or unbounded log growth.

2. **Save -> quit -> reload**
   - Save progress, exit the executable, restart, and verify the same slot resumes correctly.
   - Include HOME/quit flow and a check that no valid slot becomes zero length or corrupt.

3. **Real-device input and persistence**
   - Test physical gamepad input: right stick aiming, left stick movement, each mapped button, and device arbitration.
   - Test mouse pointer input, keyboard/gamepad remapping, and Classic Controller acceptance.
   - Verify changed control settings persist after a restart and reset-to-default works.
   - Confirm controller aim feel (range, direction, response, deadzone) with the real device.

4. **Audio audibility**
   - Confirm audio is audible on a real output device.
   - Verify music, SFX, and voice behavior against intended/original behavior, including sliders or their removal if dead.

5. **Performance and fidelity**
   - Profile sustained 60 fps/frame time and first-draw/shader or module-swap hitches.
   - Compare visual and behavioral output with known-good original/emulator reference: effects, filters, fog/aspect,
     pacing, physics, text, cutscenes, and module transitions.

6. **Final evidence and source hygiene**
   - Keep the verified local clean-build and release-audit evidence current if source changes after this checkpoint.
   - Record the final physical/manual test results in `docs/STATUS.md`.

Evidence in `README.md`, `GOAL.md`, `docs/STATUS.md`, and `docs/PLAN-NEXT.md` must point back to this checklist rather than
maintaining competing lists.
