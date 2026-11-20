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
