# AMD FSR in the Sin PC port

## Windows release

The Graphics menu offers FSR 1 spatial upscaling and AMD temporal FSR on Direct3D 12. Temporal mode loads the signed loader and upscaler from AMD FSR SDK 2.4 beside the executable. The SDK selects FSR Upscaling 4.1.2 on compatible AMD hardware and FidelityFX Super Resolution 3.1.6 on the RTX 4090. The existing config value for temporal mode is `fsr3`. The presentation log reports which path ran. Frame generation is a separate feature.

Before configuring a Windows release build, run `tools/fetch_graphics_sdks.ps1`. After building, run `tools/update_ready.ps1` and `tools/package_recomplauncher.ps1 -Version <version>`. The packager checks the vendor runtime and license files against pinned SHA-256 values.

## Stage 1: FSR 1.0 spatial

Files: `aurora-main/lib/webgpu/upscale.{hpp,cpp}` (shaders, pipelines, state, capture), hooks in `lib/aurora.cpp`
(present path, `aurora_set_upscaler`, `aurora_get_upscale_info`, `aurora_capture_present_image`), `lib/window.cpp`
(`set_upscale_divisor`, in `get_window_size`), `include/aurora/aurora.h`, `cmake/aurora_core.cmake` (adds upscale.cpp).

Pipeline per presented frame (also for each interpolated slot, since it runs inside the `present` lambda):

1. The game renders at the internal size: `get_window_size()` returns the automatic (window-fitted) frame buffer size divided
   by the scale factor, and the resolution multiplier is ignored while an upscaler is active.
   All EFB/XFB copies, the bloom chain and texture copies already derive their destination size from
   `get_render_target_size()/logical_fb_size()` (`GXFrameBuffer.cpp: scale_copy_dst`), so they follow automatically,
   exactly as they do for any non-1x multiplier.
2. Present source = XFB display copy (or frame buffer) at internal size.
3. `EASU` (12-tap gather kernel) writes an RGBA16Float image of viewport size, `RCAS` (5-tap sharpen) writes the surface-format
   image (skipped, EASU writing the surface format directly, when sharpness is 0).
4. The normal 1:1 copy pass blits that image into the swapchain, then RmlUi/ImGui draw on top at native resolution, so the menu
   and overlay stay crisp. Brightness/black handling is unchanged.

Falls back to the old resample path (and reports `active=off`) when: mode off, factor 1.0, source not smaller than the
viewport, or setup fails. MetalFX (macOS only) is only used when no FSR mode is selected.

Shader: port of `ffx_fsr1.h` FSR_EASU_F / FSR_RCAS_F (32-bit path), unchanged math including the approximate rcp/rsq bit tricks.
Differences: `textureGather` instead of gather4 calls, plain-float constants, and the RCAS limiter divisions are clamped to avoid
`0*inf` NaNs (WGSL leaves NaN min/max unspecified). The MIT notice is in the source header; license texts are in
`third_party/amd/LICENSE-FidelityFX-*` (gitignored, along with the cloned FSR1 repo and the sparse SDK checkout).

Quality presets (per-axis ratio, AMD standard): Ultra Quality 1.3 (77%), Quality 1.5 (67%), Balanced 1.7 (59%),
Performance 2.0 (50%), Ultra Performance 3.0 (33%), Native 1.0 (= off). Sharpness 0-100%: 0 disables RCAS; otherwise
stops = 2*(1 - s) fed to `exp2(-stops)` (100% = 0 stops = strongest, FSR's own default 0.25 stops is ~88%); default 80.

Verification done (no game launched): `python tools/fsr_check/test_fsr.py` extracts the two WGSL sources from `upscale.cpp`,
runs them on the RTX 4090 (Vulkan, wgpu-native) and compares with an independent numpy port that names taps by position
(so it also checks the gather ordering): EASU max error 0.0005-0.0010 (float), RCAS max 1/255 (8-bit rounding), no NaN,
EASU beats bilinear in PSNR vs ground truth (2x: 29.2 vs 26.0 dB, 1.5x: 26.3 vs 25.7, 3x: 22.9 vs 22.3). It validated with naga;
Dawn's Tint was not run, so a Tint-only complaint would show as a shader-compile error at first use (see steps below).
`clang++ -fsyntax-only` passes for upscale.cpp, aurora.cpp, window.cpp, settings_overlay.cpp, perf_log.cpp.
Not verified: the in-game result, the options menu rendering (the offline `tools/ui_preview` did not produce a shot), and
sub-1x internal rendering of the stash/partial-copy paths (see Risks).

## Config and UI

`[video]` keys (tolerant parsing, case/space/`_`/`-` ignored, `fsr`/`ultra quality` aliases; invalid values warn and are ignored):
`upscaler = off | fsr1 | fsr3 | dlss`, `fsr_quality = native | ultra_quality | quality | balanced | performance | ultra_performance`,
`fsr_sharpness = 0..100`. Changes apply live (`aurora_set_upscaler`), persist to Config.toml, and reset with the Graphics page reset.

Measurement: `SIN_PERF_LOG=1` adds
`[perf-upscale] upscaler=fsr1 active=fsr1 factor=1.50 sharpness=80% sharpened=1 internal=1280x720 output=1920x1080 scale=1.500`.
`SIN_SCREENSHOT=<file.png>` (`SIN_SCREENSHOT_DELAY` s, default 30) saves the final upscaled game image (before the overlay);
there was no in-engine screenshot path before. Note it is the game layer at viewport size, without ImGui.

## Temporal upscaling

The renderer snapshots EFB depth before the display copy clears it. It maps the copy rectangle into a forward-depth input, estimates pixel motion from successive frames, and supplies the viewport jitter and elapsed frame time to the AMD dispatch. A separate Direct3D 12 queue shares textures and fences with Dawn. The game falls back to spatial upscaling if temporal dispatch is unavailable.

## Stage 3: frame generation

FSR 3 FG (`frame-interpolation*.md`) needs the HUD-less backbuffer plus a UI composition callback, motion vectors/depth, and in the
swapchain form replaces `IDXGISwapChain4` / the Vulkan swapchain entry points to pace presents. Dawn owns the swapchain and queue; replacing
it means forking Dawn or round-tripping every frame through shared textures with a second device, and the game draws its HUD into the same
EFB as the scene, so HUD-less tagging would need per-draw splitting. FSR 4 FG additionally needs RDNA 4. The built-in generator re-renders
with interpolated true object/camera transforms, which is better information than optical flow. Decision: keep it, section renamed
"Frame generation"; no FSR FG.

## Risks

* Internal sizes below the 640x480-ish base (Performance/Ultra Performance at small windows) go through copy paths that were only
  exercised at >= 1x; see lesson on the 152x112 stash copy. Check menus/cutscenes for the top-left rectangle regression at 2x/3x.
* EASU input aspect can differ from the present viewport (4:3 frame buffer shown 16:9); EASU handles non-uniform scale, but check softness.
* Tint compile of the WGSL is unverified (naga passed); `textureGather` with a non-filtering sampler is valid WebGPU.
* `window.cpp`, `aurora.cpp`, `gpu`-adjacent files are shared with the VRR work; my edits are small and local.

## Build and test steps (lead)

1. `cmake --build build/native --parallel 30` (re-run CMake configure: `upscale.cpp` is new in `aurora_core.cmake`).
2. Config.toml `[video] upscaler="fsr1"`, `fsr_quality="performance"`, run with `SIN_PERF_LOG=1 SIN_SCREENSHOT=build/fsr.png`.
   Expected: `[perf-upscale] ... active=fsr1 ... internal=960x540 output=1920x1080 scale=2.000` (window 1920x1080), no
   `[aurora::gpu]` shader error lines, `Saved present image 1920x1080 to build/fsr.png` (a `[screenshot]` line precedes it).
3. Graphics tab: pick Upscaler = FSR 1, cycle quality (resolution info rows change within a frame), drag sharpness, confirm the
   resolution-scale row is disabled; with a frame rate above 60 confirm interpolated frames still present (`[perf] ratio` > 1).
4. Switch back to Off: `active=off`, `internal` equals the old multiplier-based size.
