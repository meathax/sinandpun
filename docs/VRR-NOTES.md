# VRR (G-SYNC / FreeSync) notes

Status: code written, compile-checked (`clang++ -fsyntax-only`), limiter timing simulated. NOT yet run in the game and
NOT verified on a VRR monitor.

## How Dawn presents on D3D12 (webgpu_dawn.dll, prebuilt; source read from google/dawn `src/dawn/native/d3d/SwapChainD3D.cpp`)

The Dawn binary in `build/native/_deps/dawn_prebuilt-src` ships no sources and no symbol strings for the flags, so this
is the upstream source of the same code path; the observable behaviour (Immediate vs Mailbox vs Fifo) is what the
logs below confirm.

- Swap chain: `CreateSwapChainForHwnd`, `DXGI_SWAP_EFFECT_FLIP_DISCARD` (flip model), `DXGI_SCALING_STRETCH`,
  `DXGI_MWA_NO_ALT_ENTER`. SDL's fullscreen is borderless/flip, not DXGI exclusive fullscreen.
- Per `wgpu::PresentMode`:

| PresentMode | buffers | swapchain flags | `Present(syncInterval, flags)` |
|---|---|---|---|
| Fifo | 2 | `ALLOW_MODE_SWITCH` | `Present(1, 0)` (vsync, blocking) |
| Mailbox | 3 | `ALLOW_MODE_SWITCH` | `Present(0, 0)` (no tearing flag: replaces the queued frame, DWM-synchronized) |
| Immediate | 2 | `ALLOW_MODE_SWITCH \| ALLOW_TEARING` | `Present(0, DXGI_PRESENT_ALLOW_TEARING)` |

- Changing between a mode with and without `ALLOW_TEARING` forces a new IDXGISwapChain (Dawn does it on `Surface::Configure`).
- Dawn does not use a frame-latency waitable object; frame latency is bounded by the fence waits on back buffers.
- Previous behaviour of this port with VSync off: `select_present_mode` preferred **Mailbox** on non-Metal. The log
  (`ready/UserData/Logs/*/console.log`) shows `present mode Mailbox`. Mailbox is `Present(0, 0)`: no tearing flag,
  so a G-SYNC display stays at a fixed refresh (DWM composition) - this is why VRR never engaged before.
- Immediate is only advertised/usable when DXGI reports `DXGI_FEATURE_PRESENT_ALLOW_TEARING`; the code requires both
  the Dawn surface capability and (on D3D12) a direct `IDXGIFactory5::CheckFeatureSupport` query.

VRR on Windows needs: tearing present (`Present(0, ALLOW_TEARING)`) on a flip-model swapchain, a window that gets
Independent Flip / DirectFlip (borderless fullscreen covering the monitor; windowed works only with the NVIDIA setting
"Enable for windowed and full screen mode" / Windows 11 "Optimizations for windowed games"), and the driver's G-SYNC /
G-SYNC Compatible / FreeSync enabled. Whether the display actually goes variable is the driver's decision and cannot be
read without NVAPI/ADL, which this project does not use.

## Design

Settings (`[video]` in Config.toml):

- `vrr = "auto" | "on" | "off"` (default auto). Auto = tearing supported **and** refresh > 63.2 Hz **and** frame rate mode
  != Original. On = whenever tearing is supported (also with Original, where it just presents at 60). Off = old behaviour.
- `vrr_margin_fps = 3` (0..30): limiter margin below the refresh.
- `vrr_limiter = true`: false disables the limiter (frame-rate mode then runs unlimited in the tearing mode).

Aurora API (`include/aurora/gfx.h`): `aurora_set_vrr(mode, marginFps, limiter)`, `aurora_get_vrr_status(&status)`
(`AuroraVrrStatus`: mode, margin, limiterEnabled, tearingSupported, active, swapchainTearing, limiterActive,
refreshHz, limitFps, plannedFps, presentMode). `AuroraPresentTiming` gained `p50FrameTimeMs`, `p99FrameTimeMs`,
`maxFrameTimeMs`.

Code (all in aurora-main/lib):

- `gfx/presentation.cpp`: `resolve_vrr()` computes `{tearingSupported, active, limiterActive, refreshHz, limitFps,
  floorFps}` from the atomics, `window::refresh_period_nanos()` and `gx::get_frame_rate`. `vrr_update()` is called when
  the config, the frame-rate mode (via `webgpu::set_present_uncapped`, called by `gx::set_frame_rate`) or the display
  refresh (`window.cpp update_refresh_period`) changes; it refreshes the limiter and pushes `RefreshSurface` when the
  wanted present mode differs from the applied one (swapchain rebuild, new ALLOW_TEARING chain).
- `webgpu/gpu.cpp`: `select_present_mode` returns **Immediate** when VRR is wanted and the surface supports it (before
  the Fifo/Mailbox choice), so VSync is bypassed. `vsync_enabled()` is false while VRR is wanted (so the planner does not
  clamp to the display rate). `present_mode_name()` reports the live swapchain mode.
- Limiter, two parts:
  1. Planner cap (`gx::effective_target_fps`): `min(fps, refresh - margin)` for every mode (Match display = refresh-3,
     Uncapped = min(cost-based, refresh-3), Fixed N above the cap is lowered). Guest timing is untouched: the grid of
     interpolated/native presents just uses a longer step.
  2. Present floor (`pace_present`, replaces `wait_until(due)` for slot and native presents in `aurora.cpp`):
     waits until `max(due, lastPresent + 1/floorFps)` with `floorFps = refresh - margin/2`. Waitable high-resolution
     timer + 400 us spin (the existing `wait_until`). The floor is above the planner rate on purpose: equal rates would let
     one late present ratchet the whole cadence late forever (measured 140.2 vs 141 fps in simulation; fixed).
     It only prevents bursts above the VRR ceiling after a stall.
- Runtime: Display tab rows "VRR (G-SYNC / FreeSync)" (Auto/On/Off), "Latency limiter", "Latency limiter margin",
  status lines (Present mode with limit and target, Display refresh, VRR state, warnings). Applied live, persisted
  immediately, not part of the keep/revert countdown. Reset-page restores auto / 3 / on. The "uneven pacing" overlay
  warning is suppressed while the limiter is active.
- `SIN_PERF_LOG`: new `[perf-present]` line per second.

## Simulation of the limiter (standalone, copy of wait_until/pace_present, 600 guest frames, +-150 us schedule noise,
random 9 ms stalls)

| refresh | limiter | mean fps | min interval | p50 | p99 | max |
|---|---|---|---|---|---|---|
| 144 (limit 141) | on, no stalls | 141.00 | 7.017 ms | 7.018 | 7.58 | 8.20 |
| 144 | on, stalls | 140.99 | 7.017 | 7.018 | 7.61 | 9.57 |
| 144 | **off**, stalls | 141.00 | **4.44** (225 fps burst) | 7.10 | 7.78 | 9.44 |
| 119.88 (limit 116.88) | on, stalls | 116.88 | 8.447 | 8.496 | 9.16 | 9.75 |
| 165 / 240 | on, stalls | 161.5 / 234.1 | 6.116 / 4.191 | ~floor | 6.7 / 4.8 | 9.5 / 10.0 |

Without the floor a stall is followed by a catch-up burst well above the refresh rate (tearing + out of VRR window); with
it the minimum interval is exactly the floor and the max is the stall itself.

## Verification steps (lead)

1. Build, start with `SIN_PERF_LOG=1` and `video.frame_rate = "display"` (or 144/uncapped), `video.vrr = "auto"`, a 144 Hz
   display, borderless fullscreen. Expected console lines:
   - `[aurora::gpu] Using present mode Immediate` (after the startup rebuild; previously `Mailbox`).
   - `[perf-present] present_mode=Immediate tearing_supported=1 vrr=auto vrr_active=1 limiter=1 limit=141.0 target=141.0
     refresh=144.00 p50=7.09ms p99<=~7.6ms max<~9ms` and `[perf] present=141.0` (+-1), `vi_time=1.000`, `speed=1.000`.
   - 119.88 Hz display: limit=116.9. 60 Hz display: vrr_active=0 in auto, `present_mode=Fifo/Mailbox`.
   - `vrr=off`: `present_mode=Mailbox` (or Fifo with VSync), `limit=0.0`, target = refresh as before.
   - `vrr_limiter=false`: vrr_active=1, limiter=0, limit=0.
2. Display tab shows "Present mode: Immediate (tearing allowed), limit 141 FPS, target 141 FPS", "VRR: Active".
3. Toggling VRR/limiter/margin live causes a one-off swapchain rebuild (log `Using present mode ...`), no restart.

Only a VRR monitor can confirm: that the display really runs variable (monitor OSD refresh readout / NVIDIA "G-SYNC
indicator" in the control panel's Display menu), absence of tearing at 141 fps inside the window (Immediate can tear if
the frame rate exceeds the max refresh; the limiter prevents that), windowed vs borderless behaviour, and the latency
benefit. VSync-off tearing on a non-VRR display is expected whenever the rate is not a refresh multiple.

## Risks

- `Present(0, ALLOW_TEARING)` in windowed (composed) mode is not tear-free (DWM may compose; no VRR without the NVIDIA
  windowed setting). Borderless fullscreen recommended; the Display tab says so.
- Present-mode switch recreates the DXGI swap chain (a hitch of a frame or two when toggling; not during play).
- 2-buffer Immediate swapchain with no frame-latency waitable: queue depth is bounded by the limiter, not by DXGI. If the
  GPU cannot sustain the target, frames queue as with the old Mailbox path.
- SDL's refresh readout on Windows can be rounded (144 for 143.98); the 3 fps margin covers it.
- `vrr=on` with `frame_rate=original` presents at 60 on an Immediate swapchain (no pacing benefit, can tear on non-VRR displays).
- Exclusive fullscreen mode (SDL display-mode change) was not examined for ALLOW_TEARING behaviour; borderless is the target.
