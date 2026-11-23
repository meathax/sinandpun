# DLSS probes

Standalone test programs for the NVIDIA DLSS work (docs/DLSS-NOTES.md). None of them touch the game or its build tree;
they compile with the game's mingw clang and need the staged SDK in `third_party/nvidia/streamline` (headers + DLLs) and
the Dawn prebuilt of the game build tree (`build/native/_deps/dawn_prebuilt-src`).

```powershell
pwsh tools/dlss_probe/build_probes.ps1            # all three, output in build/tmp_dlss (delete the folder when done)
.\build\tmp_dlss\probe_sr.exe                      # Streamline manual hooking + DLSS on a private D3D12 queue
.\build\tmp_dlss\probe_dawn_share.exe [debuglayer] # Dawn <-> second device shared textures/fences around a DLSS evaluate
.\build\tmp_dlss\probe_e2e.exe quality 40          # real dlss.cpp + motion_flow.cpp on a synthetic scene: flow table + PSNR
pwsh tools/dlss_probe/syntax_check.ps1             # clang -fsyntax-only of the aurora files touched by the DLSS work
```

`probe_e2e.exe <quality|balanced|performance|ultra|dlaa> [frames] [nomotion]` environment knobs: `DLSS_VX`/`DLSS_VY` (motion in
output px/frame), `DLSS_TRUEMV=1` (exact constant vectors), `DLSS_OW`/`DLSS_OH` (output size), `DLSS_RWADJ` (render size offset),
`DLSS_SWITCH=<mode>` (change mode halfway), `DLSS_REPEAT=2` (two dispatches per frame), `DLSS_DUMP=<prefix>` (PPM of the last
frame: ground truth / DLSS / bilinear), `DLSS_BENCH=1` (timing, no read-back), `AURORA_DLSS_JITTER_SIGN=-1`.

The probe also checks the GX velocity shader on the GPU. It draws a stationary background and two overlapping layers with different signed velocities and nonuniform clip W, then checks all 2,048 pixels.

Set `DLSS_GEOMETRY_MV=1` to exercise the game's EFB crop and pixel-unit conversion with a known vector texture. For example, use `DLSS_OW=960`, `DLSS_OH=540`, `DLSS_VX=3` and `DLSS_VY=-1.5` with `quality 124 nomotion`. At Quality the expected SDK input is (+2, -1) render pixels after reset. With `SIN_DLSS_CAPTURE_MOTION=<path-prefix>`, the game and probe write every 120th GX-sourced SDK input to `<prefix>-<frame>-<width>x<height>.rg16f`. Each row contains little-endian binary16 XY pairs without padding.
