#pragma once

#include <aurora/aurora.h>

#include "gpu.hpp"

#include <cstdint>
#include <string>

// Upscaling between the game render target and the swapchain.
//
//   * FSR 1.0 spatial (EASU + RCAS) is implemented here as WGSL render passes on the Dawn device, so it
//     needs no vendor SDK, no D3D12/Vulkan interop and works on every GPU/backend Dawn supports.
//   * Temporal upscalers (FSR 3.x, DLSS) need per-pixel motion vectors, depth, sub-pixel jitter and exposure.
//     TemporalInputs / TemporalUpscaler below are the contract such a backend plugs into; nothing implements it
//     yet (see docs/FSR-NOTES.md "Stage 2").
namespace aurora::webgpu::upscale {

struct Settings {
  AuroraUpscaler mode = AURORA_UPSCALER_OFF;
  // Per-axis ratio of output to internal resolution: 1.0 native, 1.3 ultra quality ... 3.0 ultra performance.
  float scaleFactor = 1.f;
  // 0 disables RCAS; (0, 1] maps to 2 stops (weakest) down to 0 stops (strongest) of AMD's RCAS attenuation.
  float sharpness = 0.f;
};

void set_settings(const Settings& settings) noexcept;
Settings settings() noexcept;
// Internal render size is the aspect-fitted output size divided by this (>= 1; exactly 1 when not upscaling).
float resolution_divisor() noexcept;
// True when an upscaler that runs 1:1 (DLSS in DLAA mode) is active: the internal size is the window-fitted size and
// the resolution multiplier must not apply.
bool locks_native_resolution() noexcept;

struct Result {
  wgpu::BindGroup bindGroup;
  // Image the bind group samples, at viewport size. Valid until the next dispatch.
  const TextureWithSampler* image = nullptr;
};

// FSR 1.0 EASU (+ RCAS when sharpness > 0). Encodes into `encoder`; returns an empty Result when it cannot
// run (disabled, native scale, source not smaller than the viewport, ...) so the caller falls back to
// resample_present_source.
Result fsr1_dispatch(const wgpu::CommandEncoder& encoder, const TextureWithSampler& source, const Viewport& viewport);

// Records what the present path actually did, for aurora_get_upscale_info().
void note_present(uint32_t inputWidth, uint32_t inputHeight, uint32_t outputWidth, uint32_t outputHeight,
                  AuroraUpscaler active, bool sharpened) noexcept;
AuroraUpscaleInfo info() noexcept;

// One-shot PNG capture of the final game image (after upscaling, before ImGui is composited).
void request_capture(const std::string& path);
// Render worker, present path: copies `image` out when a capture is pending.
void capture_encode(const wgpu::CommandEncoder& encoder, const TextureWithSampler& image);
// Render worker, after the command buffer holding capture_encode's copy was submitted.
void capture_after_submit();
bool gi_comparison_pending();
bool culled_comparison_pending();
void capture_culled_before(const wgpu::CommandEncoder&, const TextureWithSampler&);
void capture_rtgi(const wgpu::CommandEncoder& encoder, const TextureWithSampler& lit);
void capture_gi_pair(const wgpu::CommandEncoder& encoder, const TextureWithSampler& original,
                     const TextureWithSampler& lit, const TextureWithSampler& depth, std::array<float,4> projection);

void shutdown() noexcept;

// ---- temporal upscaler contract (not implemented) ----------------------------------------------------
struct TemporalInputs {
  const TextureWithSampler* color = nullptr;     // render resolution, perceptual or linear per `colorIsLinear`
  const TextureWithSampler* depth = nullptr;     // render resolution
  const TextureWithSampler* motion = nullptr;    // RG16F, render resolution, pixels or NDC per `motionInPixels`
  uint32_t renderWidth = 0, renderHeight = 0;
  uint32_t outputWidth = 0, outputHeight = 0;
  float jitterX = 0.f, jitterY = 0.f;            // sub-pixel offset applied to the projection, in render pixels
  float cameraNear = 0.f, cameraFar = 0.f, cameraFovY = 0.f;
  bool depthInverted = false;
  bool depthInfinite = false;
  bool colorIsLinear = false;
  bool motionInPixels = true;
  bool reset = false;                            // camera cut / scene change: drop history
  float frameTimeMs = 0.f;
  float preExposure = 1.f;
};

class TemporalUpscaler {
public:
  virtual ~TemporalUpscaler() = default;
  virtual const char* name() const noexcept = 0;
  // Output must be a viewport-sized texture in the surface format.
  virtual bool dispatch(const wgpu::CommandEncoder& encoder, const TemporalInputs& in, Result& out) = 0;
};

// Jitter sequence FSR 2/3 expect, identical to ffxFsr3UpscalerGetJitterPhaseCount / GetJitterOffset in the
// FidelityFX SDK (truncating 8 * (output/render)^2; Halton(2,3) of (index % phases) + 1, minus 0.5).
uint32_t jitter_phase_count(uint32_t renderWidth, uint32_t outputWidth) noexcept;
void halton_jitter(uint32_t index, uint32_t phaseCount, float& x, float& y) noexcept;

} // namespace aurora::webgpu::upscale
