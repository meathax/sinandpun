# Native scene rendering — 2026-11-21

The rejected viewport-wide 60 Hz seam fallback is withdrawn. Uncapped mode now retains completed scene packets and renders GPU geometry independently of guest simulation, without the old finite replay-slot budget. Fixed 120/display modes still use the existing interpolation path.

## Geometry

`gx/native_scene.cpp` captures CPU vertices before GPU upload, retains fully deformed position/normal endpoints, matches compatible draws, and joins coincident positions across material pieces in the same camera. Shared position endpoints prevent independent pieces opening cracks; hard-edge normals remain independent. GPU storage indirection avoids duplicate endpoint records. Capture preserves batching and reuses buffers. Reading mapped GPU upload memory had cost 40–60 ms/frame; capture now reads CPU source data. Draw identity is inferred from GX data, not explicit game object IDs.

## Smoke and feedback

Extra renders previously accumulated persistent EFB/converted-texture state. The renderer now snapshots texture state before and after the original frame, restores the before-state for every render-only sample, and restores the original after-state when retiring the packet. Render-only passes skip guest depth-peek side effects. Texture snapshots are pooled.

Frozen ship-scene evidence: `build/qa/native-scene-uncapped-stage0-async-acquire`. Identical endpoint renders differ in zero pixels; identical midpoint renders differ in zero pixels. Interpolated endpoint versus original rendering differs in 242/921600 pixels, maximum one RGB level. Before the fix, identical poses differed in about 690000 pixels. This proves feedback stability for this scene, not every particle lifetime or stage.

## Presentation and speed

Surface acquisition blocked the render worker for 13–15 ms despite Immediate mode. Acquisition now runs on a dedicated thread with one outstanding surface lease; resize/reconfiguration pause acquisition and retire retained scenes. The worker remains available for guest GPU completion while the surface waits. Measured worker-side presentation fell to about 1 ms. The tested large ship scene sustains about 59.9 guest frames/s. The original-rendering baseline also has 30 FPS cutscene sections; guest FPS divided by 60 alone is not a valid speed measure there.

Actual display throughput remains under investigation: current runs often present about 60 FPS despite Immediate mode and no software cap. This is not yet accepted as a fully verified uncapped release. Evidence: `build/qa/native-scene-off-stage0-extended-baseline` and `build/qa/native-scene-uncapped-stage0-final-display`.

## Startup

The host recomp cover and its settings choice are removed from the active UI. `StartupScreenVisible()` is false: game output appears immediately without the host startup input gate. The game's own title remains. The final-display capture at two seconds shows that title directly.

## Checks and limits

`runtime/tests/native_scene_tests.cpp` passes the old deformed-seam negative, 1001 shared-edge samples, hard-normal preservation, storage indirection and 10000 uncapped-time samples. Native build 27 succeeds. Frozen-scene GPU comparisons cover geometry, texture feedback and intermediate pose stability. Whole-game correctness and sustained presentation above the display bottleneck remain unverified; do not describe these results as universal acceptance.
