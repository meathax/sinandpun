# NVIDIA DLSS (Super Resolution, DLAA) and frame generation - notes

The Windows D3D12 renderer supports DLSS Super Resolution, DLAA, NVIDIA DLSS Frame Generation (2x) and Reflex. The ready and release packages include the pinned NVIDIA plugins. Section 4 explains the motion-vector quality limits. Section 7 describes the foreground presentation path and verification.

## 1. What exists

| Piece | Where |
|---|---|
| DLSS backend (Streamline manual hooking, shared Dawn/D3D12 resources, jitter, per-present dispatch) | `aurora-main/lib/webgpu/dlss.{hpp,cpp}` |
| Image-based motion vectors (4-level luma pyramid, block matching + Lucas-Kanade, WGSL) | `aurora-main/lib/webgpu/motion_flow.{hpp,cpp}` |
| Stub used on non-Windows / when the Streamline headers are not staged | `aurora-main/lib/webgpu/dlss_stub.cpp` |
| Public API: `aurora_get_dlss_status`, `aurora_set_dlss_motion_vectors`, `AURORA_UPSCALER_DLSS` | `aurora-main/include/aurora/aurora.h`, `lib/aurora.cpp` |
| Present hook (DLSS before FSR 1, MetalFX-style submit/consume), window-size sync, DLAA native lock | `lib/aurora.cpp`, `lib/webgpu/upscale.cpp`, `lib/window.cpp` |
| Jitter (viewport origin offset per scene encode) | `lib/gfx/encoding.cpp` (`apply_viewport`), `lib/gfx/frame.cpp` |
| Dawn device features for resource sharing + start-up of the detection thread | `lib/webgpu/gpu.cpp` |
| Config keys `video.upscaler = "dlss"`, `video.dlss_mode`, `video.dlss_sharpness`, `video.dlss_motion_vectors` | `runtime/include/runtime_config.h` |
| Settings UI (Graphics tab, "Upscaling" section: Upscaler = NVIDIA DLSS, DLSS mode, sharpness, motion vectors) | `runtime/src/settings_overlay.cpp` |
| `tools/update_ready.ps1` copies the DLLs into `ready\` (`-FetchDlss` downloads the SDK, `-NoDlss` skips) | `tools/update_ready.ps1` |
| Probes (not part of the game build) | `tools/dlss_probe/` |

The upscaler selector is the one the FSR work introduced (`aurora_set_upscaler(mode, scaleFactor, sharpness)`): DLSS adds
`AURORA_UPSCALER_DLSS`. `scaleFactor` is the per-axis output/render ratio and picks the mode (3.0 Ultra Performance,
2.0 Performance, 1.72 Balanced, 1.5 Quality, 1.0 DLAA). While DLSS is not ready (the first ~2 s, no DLLs, not an RTX GPU,
Vulkan backend...) or a frame cannot be upscaled, aurora runs the FSR 1 path at the same scale, so a config written
on an RTX machine still produces a picture elsewhere.

### Settings, config, modes

```toml
[video]
upscaler = "dlss"            # off | fsr1 | fsr3 | dlss
dlss_mode = "quality"        # ultra_performance (33%) | performance (50%) | balanced (58%) | quality (67%) | dlaa (100%)
dlss_sharpness = 0           # 0-100, contrast-adaptive sharpening after DLSS (0 = off)
dlss_motion_vectors = false  # estimate motion vectors from the images (experimental, see section 4)
graphics_api = "d3d12"       # DLSS needs the Direct3D 12 backend ("auto" picks it on Windows)
```

* Render sizes at 3840x2160 as reported by DLSS: UP 1280x720, Performance 1920x1080, Balanced 2227x1253, Quality 2560x1440,
  DLAA 3840x2160. Ratios 33.3 / 50 / 58 / 66.7 / 100 %.
* **Reconciling the resolution multiplier**: with an upscaler on, `video.resolution_multiplier` is ignored (the FSR work
  already does this, the settings row is greyed out). The internal size is the window-fitted size divided by the scale
  factor (`window.cpp` `get_window_size`). DLAA has scale 1.0, so it needs the same "ignore the multiplier" rule without
  a divisor: `set_upscale_divisor(divisor, lockNative)` (`upscale::locks_native_resolution()`). A resolution multiplier
  above 1 together with DLSS (supersampling) is not possible: DLSS output cannot be smaller than its input.
* The Upscaler list shows "NVIDIA DLSS" only when it is usable (or already selected, so a copied config stays visible).
  Otherwise an info row "NVIDIA DLSS: <reason>" is shown; the reason is `NVIDIA RTX GPU and nvngx_dlss.dll required
  (missing: ...)` for a missing DLL / non-NVIDIA GPU, or a specific text (Direct3D 12 backend needed, driver/Windows too
  old, hardware-accelerated GPU scheduling off, ...). All changes apply live (resize of the internal framebuffer at the
  next frame boundary; DLSS instance re-created and history reset).
* Environment knobs for the lead: `AURORA_DLSS_VERBOSE=1` (forward Streamline info logs), `AURORA_DLSS_JITTER_SIGN=-1`
  (flip the jitter sign given to DLSS; +1 is verified correct, see section 6).

## 2. How DLSS runs inside aurora / Dawn (question A1, A2)

The Windows release applies the selected NVIDIA mode's scale ratio to the actual Wii display copy. At a 1280×720 viewport, the game can produce a 1216×622 display copy. DLAA processes that image at 1216×622, then the presentation pass fits it to the viewport. Performance reconstructs a 608×311 copy to 1216×622. Ultra Performance uses three output pixels per input pixel along each axis.

Facts established with the probes (all on this machine, RTX 4090, driver 621.14, Dawn D3D12 prebuilt of the game tree):

* `dawn::native::d3d12::GetD3D12Device()` / `GetD3D12CommandQueue()` are exported by `webgpu_dawn.dll` but return
  `Microsoft::WRL::ComPtr<...>` by value with MSVC-mangled names; the game is built with mingw clang (Itanium ABI), so they
  cannot be called without hand-written ABI glue. The prebuilt also reports `SharedTextureMemoryD3D12Resource = 0`, so
  wrapping an app-created `ID3D12Resource` on Dawn's device is not available either.
* `SharedTextureMemoryDXGISharedHandle` and `SharedFenceDXGISharedHandle` are available (both `1`). They use plain C
  structs and work.

Design that works and is implemented:

1. **Streamline in manual-hooking mode** (`PreferenceFlags::eUseManualHooking | eUseFrameBasedResourceTagging |
   eDisableCLStateTracking`, features: DLSS only). `sl.interposer.dll` is loaded dynamically (signature checked with
   `WinVerifyTrust`); nothing is linked; no proxies; no swap chain involved. Without any Present on a Streamline proxy
   `slEvaluateFeature` still works (probe `probe_sr`; the "presentCommon not observed" problem of a non-manual
   integration does not arise). Resources are tagged `eOnlyValidNow`.
2. **A second `ID3D12Device`** on the same adapter (matched by vendor/device id with Dawn's `AdapterInfo`), given to
   Streamline with `slSetD3DDevice`, with its own direct queue, shared fence and a ring of 3 allocators.
3. **Shared textures**: colour input (RGBA8), depth (R32F), motion (RG16F) and output (RGBA8, UAV) are created on the
   second device (`D3D12_HEAP_FLAG_SHARED`, `ALLOW_SIMULTANEOUS_ACCESS`, + `ALLOW_RENDER_TARGET` / `ALLOW_UNORDERED_ACCESS`),
   exported with `CreateSharedHandle` and imported into Dawn as `SharedTextureMemory` textures. (`RG16Float` cannot be a
   Dawn storage texture, so the vectors are written by a render pass.)
4. **Fences**: DLSS->Dawn: one `ID3D12Fence` (shared) imported as `SharedFence`, passed to `BeginAccess`. Dawn->DLSS:
   `EndAccess` returns Dawn's fence + value, opened on the second device with `ExportInfo` (`DXGISharedHandle`) +
   `OpenSharedHandle` and waited with `ID3D12CommandQueue::Wait` (GPU-side, no CPU stall).
5. **Per present** (`dlss::dispatch`, called from the present lambda right before the FSR 1 path, like MetalFX):
   copy the game image (the XFB display copy) into the shared input, build motion vectors, submit the encoder, `EndAccess`,
   `slSetConstants` + `slSetTagForFrame` + `slEvaluateFeature` on the second queue, signal, `BeginAccess` on the output
   and hand a new encoder back; the present blit samples the output. `dlss::after_submit()` ends the output access after
   the present submit. Optional CAS sharpening runs on the output. Any failure falls back to FSR 1 for that configuration.
6. Detection runs on a background thread started in `webgpu::initialize` (about 2 s: plugin loading + NGX init); the UI
   polls `aurora_get_dlss_status()`.

Measured (RTX 4090): Streamline + DLSS `slEvaluateFeature` Performance 1080p->4K 1.8 ms; with scene render, input copy, motion
vectors and DLSS (probe `probe_e2e`, `DLSS_BENCH`): Quality 720p->1080p 3.5 ms without / 6.4 ms with estimated motion vectors,
Performance 1080p->4K 4.7 / 11.0 ms, DLAA 4K 29.6 ms with motion vectors. The estimated motion vectors cost 3 ms at 720p
and 6 ms at 1080p internal resolution: that is why they are off by default.

Cross-device sharing makes resources use the "simultaneous access" layout (no frame-buffer compression); the cost is small
at these sizes. A single-device design (Dawn's own device + queue) would need hand-written ABI glue for the Dawn getters
and a Dawn that exposes `SharedTextureMemoryD3D12Resource`; it is a possible later optimisation, not needed for correctness.

Streamline logs one benign warning ("tagged resources use an unsupported alignment ... overridden to 0": the shared
resources are 64 KB aligned; Streamline clones the tagged volatile inputs).

## 3. Where in the frame the upscale happens (question A4)

The game draws the 3D scene **and the HUD / menus into the same EFB**, does many EFB->texture copies at internal resolution
(bloom chain, post effects, shadow-style tricks) that later draws sample, and finishes with one EFB->XFB display copy
(`runtime/src/hle/gx/gx_copy.cpp`, `aurora lib/dolphin/gx/GXFrameBuffer.cpp copy_disp`). aurora presents that display copy
(`present_source`). DLSS therefore runs **after the XFB copy, on the finished frame including the HUD**, at the same point as
FSR 1 / MetalFX. Consequences (all inherent to this engine, none are bugs):

* The HUD, text and 2D menus are jittered and temporally accumulated with the scene. Static HUD converges to a clean
  anti-aliased image; animated HUD elements and fast text changes are a little soft/ghosty. Only aurora's own overlays
  (ImGui, RmlUI, the settings menu) are drawn after DLSS at native resolution.
* EFB->texture copies (render-to-texture effects) sample the jittered EFB at internal resolution; nothing in them
  is "excluded" from DLSS. Post effects that re-sample their own previous output (blur chains) see slightly different
  jitter every frame and may shimmer faintly.
* Rendering the HUD after upscaling would require splitting the EFB at the first orthographic / HUD draw and drawing the
  remainder at output resolution; aurora has no per-draw resolution switching. Not attempted.

### Jitter (question A3, first half)

DLSS needs a sub-pixel offset per frame. aurora's GX viewport is applied in one place (`apply_viewport`); shifting the
viewport **origin** by the jitter shifts the whole rasterised picture by the same amount without touching a single
generated shader, uniform or projection matrix: draw geometry is fixed in NDC, so a viewport offset is exactly a
projection-matrix jitter. Sequence: Halton(2,3), `8 * (output/render)^2` phases (8..128). Frames from `begin_frame`, and
every interpolated slot, advance the sequence once; the native frame's offset is restored before the native present
(`dlss::advance_jitter / restore_native_jitter`). Verified: sign convention (the picture shifts by +jitter right/down, the same
value goes to `sl::Constants::jitterOffset`). With the wrong sign the static-scene PSNR drops from 31.2 to 19.1 dB, with
jitter 0 it is 25.8 dB.

## 4. Motion vectors (question A3, second half) - the honest part

DLSS Super Resolution is trained on engine motion vectors and trusts them completely. Measured upper bound with exact
vectors (synthetic scene: textured background + thin lines, 1.5x / 2x scale, constant translation; PSNR against a 6x6
super-sampled ground truth, dB):

| scenario | bilinear low-res | DLSS zero vectors | DLSS estimated vectors | DLSS exact vectors |
|---|---|---|---|---|
| static, Quality (1.5x) | 25.2 | **31.2** | **31.2** | 31.2 |
| 2.5 px/frame, Quality | 25.5 | 24.9 | 22.5 | **28.3** |
| 2.5 px/frame, Performance (2x) | 22.4 | 23.5 | 23.6 .. 24.4 | **28.9** |
| 2.5 px/frame, DLAA | 30.6 (aliased) | 32.1 | 31.8 .. 32.2 | **34.3** |
| 8 px/frame, Quality | 25.1 | 24.2 | 22.7 | **29.4** |
| 20 px/frame, Quality | 25.0 | 23.8 | 22.1 | **29.3** |

(PSNR over a thin-line scene rewards blur, so absolute numbers understate DLSS visually; the comparison between the DLSS
columns is the point.) Findings:

* Static content and DLAA: DLSS works well even with zero vectors (+6 dB over bilinear for Quality).
* With motion, exact vectors are worth about +5 dB; **my image-based estimate does not beat zero vectors**. It recovers
  integer-accurate motion (median error 0.0 px up to ~50 px/frame) but the sub-pixel accuracy on aliased low-resolution
  frames is only ~0.3 px, and DLSS punishes small wrong vectors harder than missing ones. That is why
  `video.dlss_motion_vectors` defaults to **false** (zero vectors; history is rejected by DLSS itself).
* The estimator (`motion_flow.cpp`) is jitter aware (the previous pyramid is warped by the known jitter change, so a pixel
  without motion matches exactly and the vector is exactly zero), has a "prefer no motion" rule and a 9x9 median.

### What real motion vectors need (not implemented; sizing)

aurora already tracks per draw the previous-frame transform for interpolation (`record_interpolation_draw`, matching by
`FrameInterpolationDrawIdentity`). A velocity render target would reuse that: a second colour attachment next to
`ColorAttachmentSemantic::Normal` (`scene_render_target_layout` already supports a second attachment), a previous-matrix
uniform per draw filled at seal time like the interpolated uniform copies, a vertex-shader previous position
(`projection * prevPos`) and a fragment output of `(prevNdc - curNdc)` scaled to pixels, minus the jitter. Hard parts:

1. **Skinned / indexed-matrix draws** need the previous palette per vertex; **draws with no previous match** (new
   objects, merged draws) must write zero or a camera-only vector; **particles / billboards / HUD** should write zero or be
   masked (DLSS: wrong vectors are worse than none; for HUD an "exclude from DLSS" mask needs the HUD split of section 3).
2. **EFB copies / post effects**: the EFB->texture copy and the resolve passes copy colour only; a velocity attachment
   would have to be copied/cleared in lock-step (`gfx::resolve_pass_into`, `tex_copy_conv`), and the final XFB copy must also
   resolve depth + velocity next to colour so they are consistent with the colour DLSS sees.
3. **Depth** comes from the EFB snapshot associated with the display copy. With a separated HUD, the captured background depth is preferred. Frames without captured depth use the neutral plane.

Until then the practical advice is: **DLAA and Quality are good for static / slow content, Performance and below look
like a strong spatial upscaler with temporal AA in motion; try "Estimated motion vectors" only as an experiment.**

## 5. Licensing (check done on the downloaded SDK)

* The Streamline repository's framework/headers (`include/`, `license.txt`) are MIT. The binaries (`sl.*.dll`,
  `nvngx_dlss.dll`, `nvngx_dlssg.dll`) are governed by the **NVIDIA RTX SDKs LICENSE** (`NVIDIA-RTX-SDKs-LICENSE.txt` next to
  the SDK; also `nvngx_dlss.license.txt`).
* Redistribution (section 1c, 2): allowed as object code inside an application that has "material additional
  functionality", flow-down of terms at least as protective as NVIDIA's, the usual notices, no stand-alone distribution of
  the SDK, no implication of NVIDIA endorsement.
* The Windows release includes four separately licensed NVIDIA DLLs and their notices. The release packager verifies the SDK file hashes before creating the archive. The binaries remain outside the GPL source tree.
* DLSS needs an NVIDIA RTX GPU (Turing or newer), Windows 10 2004+, driver 512.15+ (the tested driver is 621.14).
  Streamline checks the signatures of its plug-ins; the app checks `sl.interposer.dll` with `WinVerifyTrust`. No
  over-the-air downloads (`eLoadDownloadedPlugins` is not set).
* SDK pins used and tested: Streamline 2.14.16, DLSS 310.9.16 (copied from the GDL tree's `third_party/streamline`, identical to
  the GitHub release `v2.14.16`: `nvngx_dlss.dll` 58 956 912 bytes). The local test DLLs listed in the task
  (Onimusha SL 2.10.3 / DLSS 310.5.2, MH Wilds SL 2.7.2, TOTK 310.9.1, Minecraft 2.1.16) are older and were not used.
  GitHub `NVIDIA-RTX/Streamline` stopped committing binaries at 2.7.32; `update_ready.ps1 -FetchDlss` downloads the release zip
  (263 MB) and keeps `include/`, the six DLLs the game needs and the licence texts (about 67 MB).

## 6. Validation (what was run)

`tools/dlss_probe/` (build with `build_probes.ps1`, binaries go to `build/tmp_dlss/` and are disposable):

* `probe_sr.cpp`: Streamline manual hooking, native D3D12 device, no swap chain: `slIsFeatureSupported(DLSS) = 0`, optimal
  sizes for all modes, 8/8 `slEvaluateFeature` calls OK on a private queue.
* `probe_dawn_share.cpp`: Dawn D3D12 <-> second device with shared textures/fences, DLSS between them: PASS (also with the
  D3D12 debug layer on).
* `probe_e2e.cpp`: the real `dlss.cpp` + `motion_flow.cpp` against a Dawn device and a synthetic animated scene rendered
  with viewport jitter: flow accuracy table, the PSNR numbers above, mode switching (instance re-creation, reset), two
  dispatches per frame (interpolation slots), output size adjustment for fixed-ratio modes (a 1 px rounding difference
  between the window code and DLSS is absorbed by looking for a nearby output size DLSS accepts, e.g. 1920x1080 ->
  1922x1082 for a 641x361 render), failure fallback (render size outside DLSS's range -> clean `false`, logged once per
  configuration, retried when the configuration changes).
* `clang++ -fsyntax-only` with the real build flags for every touched aurora file (`tools/dlss_probe/syntax_check.ps1`) and
  `runtime_config.h` (`controls_settings_tests.cpp`). `settings_overlay.cpp` could only be checked for errors at the edited
  lines (the stale build tree lacks the include paths for the whole file).

## 7. Frame generation (question C)

Enable **NVIDIA DLSS Frame Generation (2x)** under Graphics, apply, then restart with D3D12 and DLSS Quality or DLAA. The game uses its original simulation cadence and enables Reflex. The toggle is opt-in on a new install.

On a configured launch, Dawn keeps a fallback child surface. A Streamline DXGI factory creates the visible flip-model swapchain on the focused top-level game window. The presenter copies the final composited frame through shared GPU memory and supplies the scene output, depth, and estimated motion vectors. A common Streamline frame token ties inputs to Reflex markers and the real Present call. Shared fences order Dawn, the native queue and the plugin's consumption of frame inputs.

The Graphics status uses NVIDIA's actual presented-frame count. The runtime emits 120-application-frame measurement windows. Compare the reported NVIDIA presents with application frames in a focused, moving scene. Windows focus loss can suspend generation. Estimated motion vectors and guest HUD extraction can cause artifacts on fine HUD elements or newly revealed scenery.


What is offered instead (already present, labelled "Frame generation" in the Graphics tab, `video.frame_rate`):

| UI choice | `video.frame_rate` | Meaning |
|---|---|---|
| Original (60) | `"original"` | off |
| Match display | `"display"` | as many presented frames as the display refresh |
| 90 / 120 / 144 / 165 / 180 / 240 FPS | a number, e.g. `120` | fixed target (120 = 2x, 180 = 3x, 240 = 4x; any other fps can be typed into the config) |
| Uncapped | `"uncapped"` | as fast as the GPU allows, capped by `video.frame_rate_cap` (`Frame rate limit` row) |

This is **transform interpolation**: the guest still runs at 60 Hz, every extra frame re-renders the recorded draw list with the
perspective draws' previous/current transforms blended (translation/rotation/scale per draw, matched across frames), on
the GPU, with no optical flow, no neural network and no latency-hiding extra frame of lookahead beyond the one it needs.
Compared with DLSS-G / FSR 3 frame generation: it is exact for rigid motion and camera moves (no image-space
artifacts), but cannot invent in-between motion for anything it fails to match (particles, UI, texture animation,
new objects), costs a full extra scene render per generated frame (GPU time scales with the factor) and CPU time at seal
time.

DLSS works together with the interpolation: each presented slot is jittered, upscaled and counts as one DLSS frame.

## 8. Build / test for the lead

Prerequisites on the build machine: nothing new for the build itself. The Streamline **headers** are in
`third_party/nvidia/streamline/include` (staged; gitignored). Without them the build compiles `dlss_stub.cpp` and the
option reports "this build has no NVIDIA DLSS support".

1. `cmake` re-configure (new source files: `lib/webgpu/dlss.cpp`, `motion_flow.cpp`, `dlss_stub.cpp`; the cache variable
   `AURORA_STREAMLINE_INCLUDE` points at the headers), then the normal ninja build. If aurora comes from the precompiled
   package, rebuild that package.
2. `pwsh tools/update_ready.ps1` (add `-FetchDlss` if `third_party/nvidia/streamline/bin/x64` is missing). It copies
   `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll` + `ready\licenses\nvidia-*` next to the exe.
3. In `ready\UserData\Config.toml`: `[video] graphics_api = "d3d12"`, `upscaler = "dlss"`, `dlss_mode = "quality"`, or use
   Settings -> Graphics -> Upscaling -> Upscaler = NVIDIA DLSS.

Expected log lines (aurora log, a couple of seconds after the "Requesting adapter / Graphics adapter information" block):

```
[aurora] DLSS available: Streamline 2.14.16 / DLSS 310.9.16 on adapter 10de:2684
[aurora] DLSS Quality: 1280x720 -> 1920x1080 (optimal render 1280x720)          (first present with DLSS selected)
```

Possible others: `DLSS <mode> <r> -> <o> failed: ...; falling back to the normal upscaler until the configuration changes`
(one line per configuration), `DLSS <mode>: output WxH adjusted to ...` (fixed-ratio rounding), `DLSS <mode>: render size ... outside the
supported range ...`. A missing DLL gives no log line; the settings row shows `NVIDIA RTX GPU and nvngx_dlss.dll required
(missing: ...)`. `AuroraUpscaleInfo.active == AURORA_UPSCALER_DLSS` while it runs.

What to check by eye: static title/menu screens (should be clean, DLAA best), jitter shimmer on the HUD, ghosting trails
behind fast objects (then try "Estimated motion vectors" on/off), a mode switch while playing (history reset, no crash), window
resize and fullscreen toggle, pausing, the F-key capture (`aurora_capture_present_image` returns the DLSS output).

## 9. Risks / things not verified in game

* Whole in-game path untested (compile-checked only). The first run may reveal threading assumptions about the render worker
  (jitter state is only touched there; `set_jitter_enabled` is atomic).
* The resource-sharing features are now requested on every D3D12 device (`SharedTextureMemoryDXGISharedHandle`,
  `SharedFenceDXGISharedHandle`); they were enabled in all probes without side effects, but this changes device creation for
  every user of the D3D12 backend.
* A Streamline start-up (about 2 s) creates `C:\ProgramData\NVIDIA\NGX\models` on the first run (NGX does that).
* Window smaller than the internal size, or an `Ultra Performance` size mismatch that cannot be adjusted -> FSR 1 fallback.
* Estimated motion vectors cost 3-6 ms per presented frame and do not improve quality yet (section 4).
